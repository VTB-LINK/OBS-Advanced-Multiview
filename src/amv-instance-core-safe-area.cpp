/*
OBS Advanced Multiview - Safe area vertex buffer and rendering
Split from multiview-window.cpp for maintainability.
All functions remain members of AmvInstanceCore.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
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

/* EBU R95 safe area percentages */
#define ACTION_SAFE_PERCENT 0.035f
#define GRAPHICS_SAFE_PERCENT 0.05f
#define FOURBYTHREE_SAFE_PERCENT 0.1625f
#define CENTER_LINE_LENGTH 0.02f
void AmvInstanceCore::init_safe_area_vbs()
{
	if (safe_area_vb_init_)
		return;

	/* Action Safe (3.5% margin) */
	gs_render_start(true);
	gs_vertex2f(ACTION_SAFE_PERCENT, ACTION_SAFE_PERCENT);
	gs_vertex2f(ACTION_SAFE_PERCENT, 1.0f - ACTION_SAFE_PERCENT);
	gs_vertex2f(1.0f - ACTION_SAFE_PERCENT, 1.0f - ACTION_SAFE_PERCENT);
	gs_vertex2f(1.0f - ACTION_SAFE_PERCENT, ACTION_SAFE_PERCENT);
	gs_vertex2f(ACTION_SAFE_PERCENT, ACTION_SAFE_PERCENT);
	safe_action_vb_ = gs_render_save();

	/* Graphics Safe (5.0% margin) */
	gs_render_start(true);
	gs_vertex2f(GRAPHICS_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(GRAPHICS_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - GRAPHICS_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - GRAPHICS_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(GRAPHICS_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	safe_graphics_vb_ = gs_render_save();

	/* 4:3 safe for widescreen (16.25% horizontal margin) */
	gs_render_start(true);
	gs_vertex2f(FOURBYTHREE_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - FOURBYTHREE_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - FOURBYTHREE_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(FOURBYTHREE_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(FOURBYTHREE_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	safe_4x3_vb_ = gs_render_save();

	/* Center horizontal line - LEFT */
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.5f);
	gs_vertex2f(CENTER_LINE_LENGTH, 0.5f);
	safe_center_left_vb_ = gs_render_save();

	/* Center vertical line - TOP */
	gs_render_start(true);
	gs_vertex2f(0.5f, 0.0f);
	gs_vertex2f(0.5f, CENTER_LINE_LENGTH);
	safe_center_top_vb_ = gs_render_save();

	/* Center horizontal line - RIGHT */
	gs_render_start(true);
	gs_vertex2f(1.0f, 0.5f);
	gs_vertex2f(1.0f - CENTER_LINE_LENGTH, 0.5f);
	safe_center_right_vb_ = gs_render_save();

	safe_area_vb_init_ = true;
}

void AmvInstanceCore::release_safe_area_vbs()
{
	if (!safe_area_vb_init_)
		return;

	gs_vertexbuffer_destroy(safe_action_vb_);
	gs_vertexbuffer_destroy(safe_graphics_vb_);
	gs_vertexbuffer_destroy(safe_4x3_vb_);
	gs_vertexbuffer_destroy(safe_center_left_vb_);
	gs_vertexbuffer_destroy(safe_center_top_vb_);
	gs_vertexbuffer_destroy(safe_center_right_vb_);
	safe_action_vb_ = nullptr;
	safe_graphics_vb_ = nullptr;
	safe_4x3_vb_ = nullptr;
	safe_center_left_vb_ = nullptr;
	safe_center_top_vb_ = nullptr;
	safe_center_right_vb_ = nullptr;
	safe_area_vb_init_ = false;
}

void AmvInstanceCore::render_safe_area(int cellIndex, int cellX, int cellY, int cellW, int cellH, int vrX, int vrY,
				       int vrW, int vrH)
{
	if (cellIndex < 0 || cellIndex >= (int)effective_visuals_.size())
		return;

	const SafeAreaSettings &sa = effective_visuals_[cellIndex].safeArea;
	if (!sa.enabled)
		return;

	const bool useCellAnchor = sa.anchorMode == SafeAreaAnchorMode::Cell;
	const int anchorX = useCellAnchor ? cellX : vrX;
	const int anchorY = useCellAnchor ? cellY : vrY;
	const int anchorW = useCellAnchor ? cellW : vrW;
	const int anchorH = useCellAnchor ? cellH : vrH;

	if (anchorW <= 0 || anchorH <= 0)
		return;

	/* Lazily init vertex buffers (we're already in graphics context during render) */
	if (!safe_area_vb_init_)
		init_safe_area_vbs();

	/* Compute color with user-specified opacity */
	uint8_t alpha = (uint8_t)(sa.opacity * 255.0);
	uint32_t saColor = ((uint32_t)alpha << 24) | (sa.color & 0x00FFFFFF);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	/* Helper lambda: render a vertex buffer scaled to the configured anchor rect.
	 *
	 * The vertex buffers are normalized 0..1, so we map them onto the anchor
	 * rect with a dedicated region (viewport = anchor rect, ortho = 0..1)
	 * rather than a model matrix against the ambient projection. Every other
	 * draw_grid helper already wraps its own startRegion; doing the same here
	 * makes safe-area self-contained so it renders correctly into both the
	 * on-screen display and the offscreen output texrender, whose ambient
	 * projections differ (issue #11). */
	auto renderVB = [&](gs_vertbuffer_t *vb) {
		if (!vb)
			return;

		gs_load_vertexbuffer(vb);

		startRegion(anchorX, anchorY, anchorW, anchorH, 0.0f, 1.0f, 0.0f, 1.0f);

		gs_effect_set_color(colorParam, saColor);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw(GS_LINESTRIP, 0, 0);

		endRegion();
	};

	/* Draw all safe area guides */
	renderVB(safe_action_vb_);
	renderVB(safe_graphics_vb_);
	renderVB(safe_4x3_vb_);
	renderVB(safe_center_left_vb_);
	renderVB(safe_center_top_vb_);
	renderVB(safe_center_right_vb_);
}

/* ---- PGM / PRVW cell highlight borders ----
 *
 * Strategy: each frame we snapshot the recursive active-source trees of the
 * current PGM and PRVW scenes into raw-pointer hash sets. For each rendered
 * cell, the cell's resolved source is classified by:
 *   1. cell.type == "pgm"  → PgmDirect (uses obs_render_main_texture)
 *   2. cell.type == "prvw" → PrvwDirect when Studio Mode is on, else PgmDirect
 *      (PRVW visually falls back to PGM, so the highlight should follow suit)
 *   3. scene/source cell whose pointer == current PGM scene → PgmDirect
 *   4. scene/source cell whose pointer == current PRVW scene → PrvwDirect
 *   5. scene/source cell whose pointer is in pgm_tree_set_ → PgmNested
 *   6. scene/source cell whose pointer is in prvw_tree_set_ → PrvwNested
 *
 * Priority: PgmDirect > PrvwDirect > PgmNested > PrvwNested. Direct matches
 * always render as solid borders; nested matches render as dashed when
 * HighlightSettings.nestedDashed is true, solid otherwise.
 *
 * Border geometry: thickness defaults to gutter_px_ so the border fills the
 * gutter (matches OBS native multiview look). When gutter == 0, we fall back
 * to a thin inner inset of HighlightSettings.minThicknessPx so the border
 * remains visible without spilling outside the cell.
 *
 * No per-cell override: highlight is a window-wide concept driven by the
 * scene-tree of the OBS frontend, so the resolve chain in
 * resolve_effective_visual_settings only considers Global + Instance. */
