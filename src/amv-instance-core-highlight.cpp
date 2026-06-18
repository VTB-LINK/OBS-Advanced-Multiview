/*
OBS Advanced Multiview - PGM/PRVW highlight border rendering
Split from multiview-window.cpp for maintainability.
All functions remain members of AmvInstanceCore.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-frontend-cache.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

static void collect_tree_cb(obs_source_t * /*parent*/, obs_source_t *child, void *param)
{
	auto *out = static_cast<std::unordered_set<obs_source_t *> *>(param);
	if (child)
		out->insert(child);
}

static void collect_tree_sources(obs_source_t *root, std::unordered_set<obs_source_t *> &out)
{
	if (!root)
		return;
	out.insert(root);
	obs_source_enum_active_tree(root, collect_tree_cb, &out);
}

static inline void startRegion(int vX, int vY, int vCX, int vCY, float oL, float oR, float oT, float oB)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vX, vY, vCX, vCY);
	gs_ortho(oL, oR, oT, oB, -100.0f, 100.0f);
}

static inline void endRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}
void AmvInstanceCore::refresh_highlight_tree_sets()
{
	pgm_tree_set_.clear();
	prvw_tree_set_.clear();
	OBSSourceAutoRelease pgm = amv_frontend::current_program_scene();
	if (pgm)
		collect_tree_sources(pgm, pgm_tree_set_);
	OBSSourceAutoRelease prvw = amv_frontend::current_preview_scene();
	if (prvw)
		collect_tree_sources(prvw, prvw_tree_set_);
}

AmvInstanceCore::HighlightKind AmvInstanceCore::compute_cell_highlight(int cellIndex)
{
	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return HighlightKind::None;

	const auto &cs = cell_sources_[cellIndex];

	/* Dedicated PGM / PRVW viewer cells already serve as the visual primary
	 * monitor of their respective bus, so an additional colored border on
	 * top of them is just noise. The OBS-native multiview behaves the same
	 * way — only scene/source cells get the red/green outline. */
	if (cs.type == "pgm" || cs.type == "prvw")
		return HighlightKind::None;

	/* Scene/source cell: resolve to a raw pointer for set comparison.
	 * We pull a strong ref to ensure the pointer is valid while compared,
	 * and release it at scope end. */
	if (cs.type.empty() || !cs.weak_ref)
		return HighlightKind::None;

	OBSSourceAutoRelease cellHolder = OBSGetStrongRef(cs.weak_ref);
	obs_source_t *cellSrc = cellHolder;
	if (!cellSrc)
		return HighlightKind::None;

	OBSSourceAutoRelease pgm = amv_frontend::current_program_scene();
	OBSSourceAutoRelease prvw = amv_frontend::current_preview_scene();

	if (pgm && cellSrc == pgm.Get())
		return HighlightKind::PgmDirect;
	if (prvw && cellSrc == prvw.Get())
		return HighlightKind::PrvwDirect;
	/* PGM nested check first so PGM outranks PRVW when both contain the
	 * same source nested inside. */
	if (pgm_tree_set_.find(cellSrc) != pgm_tree_set_.end())
		return HighlightKind::PgmNested;
	if (prvw_tree_set_.find(cellSrc) != prvw_tree_set_.end())
		return HighlightKind::PrvwNested;
	return HighlightKind::None;
}

void AmvInstanceCore::render_cell_highlight(const CellRect &cell, int vpX, int vpY, HighlightKind kind,
					    const HighlightSettings &hs)
{
	if (kind == HighlightKind::None || !hs.enabled)
		return;

	uint32_t color = (kind == HighlightKind::PgmDirect || kind == HighlightKind::PgmNested) ? hs.pgmColor
												: hs.prvwColor;

	const bool nested = (kind == HighlightKind::PgmNested || kind == HighlightKind::PrvwNested);
	const bool dashed = nested && hs.nestedDashed;

	int cellX = cell.x + vpX;
	int cellY = cell.y + vpY;
	int cellW = cell.w;
	int cellH = cell.h;
	if (cellW <= 0 || cellH <= 0)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	/* draw_rect: emit a single filled rectangle in absolute viewport coords.
	 * Mirrors the pattern used by the gutter fill, label background, and
	 * PRVW fallback yellow bar above.
	 *
	 * NOTE: gs_effect_set_color must be called BEFORE each effect_loop —
	 * the SOLID effect's color uniform does not survive across separate
	 * loop invocations in a reliable way. Hoisting the color set above the
	 * 4-rect / dash sequence produced "only the first rect colored" bug
	 * (top edge red, the other three edges white). */
	auto draw_rect = [&](int x, int y, int w, int h) {
		if (w <= 0 || h <= 0)
			return;
		startRegion(x, y, w, h, 0.0f, (float)w, 0.0f, (float)h);
		gs_effect_set_color(colorParam, color);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, w, h);
		endRegion();
	};

	/* Determine geometry mode:
	 *   gutter_px_ > 0  → "gutter fill" mode: border occupies the
	 *                     gutter zone OUTSIDE the cell rect (matches
	 *                     OBS native multiview).
	 *   gutter_px_ == 0 → "inset" mode: border drawn INSIDE the cell
	 *                     at thickness = hs.minThicknessPx so it
	 *                     remains visible with zero-gutter layouts.
	 *
	 * In both modes we draw 4 rectangles (top/bottom/left/right) for
	 * solid borders. Dashed borders walk each side and emit a series
	 * of small rectangles separated by `dashGapPx`. */
	int t;
	int outerX, outerY, outerW, outerH; /* full outer bounding box */
	if (gutter_px_ > 0) {
		t = gutter_px_;
		outerX = cellX - t;
		outerY = cellY - t;
		outerW = cellW + 2 * t;
		outerH = cellH + 2 * t;
	} else {
		t = hs.minThicknessPx > 0 ? hs.minThicknessPx : 1;
		outerX = cellX;
		outerY = cellY;
		outerW = cellW;
		outerH = cellH;
	}

	/* Tiny-cell safety: when the outer box is smaller than 2*t in either
	 * dimension, the 4 "side strip" rects (solid mode) and 4 corner
	 * squares (dashed mode) start to overlap/invert. In the gutter==0
	 * inset case this also means the left/right strips would paint
	 * outside the cell (overflowing into the neighbour or window
	 * background). Skip the border entirely — a cell that small is
	 * unreadable anyway. */
	if (outerW < 2 * t || outerH < 2 * t)
		return;

	if (!dashed) {
		/* Solid 4-rect border. Side strips overlap at corners but
		 * that's harmless for opaque fills. */
		draw_rect(outerX, outerY, outerW, t);                          /* top */
		draw_rect(outerX, outerY + outerH - t, outerW, t);             /* bottom */
		draw_rect(outerX, outerY + t, t, outerH - 2 * t);              /* left */
		draw_rect(outerX + outerW - t, outerY + t, t, outerH - 2 * t); /* right */
		return;
	}

	/* Dashed border with miter-clean corners.
	 *
	 * Step 1: paint 4 solid t×t corner squares. This guarantees every
	 * corner is a closed 90° L regardless of dash phase — fixes the
	 * "gap at the corner" / "misaligned tip" artefact that occurs when
	 * the first/last dash on adjacent sides don't line up.
	 *
	 * Step 2: walk each side BETWEEN the corner squares and emit short
	 * dashes. Horizontal sides span [outerX+t, outerX+outerW-t],
	 * vertical sides span [outerY+t, outerY+outerH-t]. The last dash is
	 * truncated if it would overshoot the available span. */
	draw_rect(outerX, outerY, t, t);                           /* TL */
	draw_rect(outerX + outerW - t, outerY, t, t);              /* TR */
	draw_rect(outerX, outerY + outerH - t, t, t);              /* BL */
	draw_rect(outerX + outerW - t, outerY + outerH - t, t, t); /* BR */

	int dash = hs.dashLengthPx > 0 ? hs.dashLengthPx : 1;
	int gap = hs.dashGapPx > 0 ? hs.dashGapPx : 1;
	int period = dash + gap;

	int hSpan = outerW - 2 * t; /* horizontal run between L/R corner squares */
	int vSpan = outerH - 2 * t; /* vertical run between T/B corner squares */

	/* Top side */
	for (int off = 0; off < hSpan; off += period) {
		int segLen = dash;
		if (off + segLen > hSpan)
			segLen = hSpan - off;
		draw_rect(outerX + t + off, outerY, segLen, t);
	}
	/* Bottom side */
	for (int off = 0; off < hSpan; off += period) {
		int segLen = dash;
		if (off + segLen > hSpan)
			segLen = hSpan - off;
		draw_rect(outerX + t + off, outerY + outerH - t, segLen, t);
	}
	/* Left side */
	for (int off = 0; off < vSpan; off += period) {
		int segLen = dash;
		if (off + segLen > vSpan)
			segLen = vSpan - off;
		draw_rect(outerX, outerY + t + off, t, segLen);
	}
	/* Right side */
	for (int off = 0; off < vSpan; off += period) {
		int segLen = dash;
		if (off + segLen > vSpan)
			segLen = vSpan - off;
		draw_rect(outerX + outerW - t, outerY + t + off, t, segLen);
	}
}
