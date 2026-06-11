/*
OBS Advanced Multiview - Phase 3 / M5 status overlay (Missing Source, Signal Lost, ...)
Split from multiview-window.cpp for maintainability.
All functions remain members of MultiviewWindow.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-window.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <plugin-support.h>

#include <algorithm>
#include <string>

/* Same startRegion/endRegion idiom used elsewhere; duplicated for unit-local
 * inlining. (gs_projection_push/gs_viewport_push are cheap but inlining keeps
 * the per-cell branch in render_status_overlay() flat.) */
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

/* Render-resolution font size used when creating the source. We scale the
 * resulting texture DOWN to each cell, mirroring `label.cpp`'s strategy of
 * always shrinking instead of upsampling a small bitmap. 64 keeps the source
 * sharp on a 4K monitor without consuming excessive VRAM (4 textures total). */
static constexpr int STATUS_TEXT_RENDER_SIZE = 64;

/* ARGB color for status overlay text. Pure white keeps the text readable on
 * top of any background image or fallback content. */
static constexpr uint32_t STATUS_TEXT_COLOR = 0xFFFFFFFF;

/* ---- private helpers ---- */

void MultiviewWindow::ensure_status_text_source(StatusTextEntry &entry, const char *text)
{
	if (entry.source || !text || !*text)
		return;

	/* Pad with spaces so the resulting source has a small horizontal margin
	 * matching the OBS native multiview label aesthetic. */
	std::string paddedText = std::string(" ") + text + " ";

	std::string srcName = "adv_mv_status_" + uuid_ + "_" + text;

#ifdef _WIN32
	/* Windows: text_gdiplus_v2 has solid CJK font fallback and matches what
	 * `rebuild_label_sources()` already uses. Bold weight makes the overlay
	 * stand out without needing an outline. */
	obs_data_t *fontObj = obs_data_create();
	obs_data_set_int(fontObj, "size", STATUS_TEXT_RENDER_SIZE);
	obs_data_set_string(fontObj, "face", "Arial");
	obs_data_set_int(fontObj, "flags", 1); /* Bold */

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", paddedText.c_str());
	obs_data_set_obj(settings, "font", fontObj);
	obs_data_set_int(settings, "color", STATUS_TEXT_COLOR);
	obs_data_set_int(settings, "opacity", 100);
	obs_data_set_bool(settings, "outline", false);
	obs_data_set_int(settings, "align", 0); /* left */

	obs_source_t *src = obs_source_create_private("text_gdiplus", srcName.c_str(), settings);
	obs_data_release(settings);
	obs_data_release(fontObj);
#else
	/* Mac/Linux: text_ft2_source_v2 is the canonical text source. Same
	 * font fallback ladder as `rebuild_label_sources()`. */
#ifdef __APPLE__
	const char *fontFace = "Helvetica";
#else
	const char *fontFace = "Monospace";
#endif
	obs_data_t *fontObj = obs_data_create();
	obs_data_set_int(fontObj, "size", STATUS_TEXT_RENDER_SIZE);
	obs_data_set_string(fontObj, "face", fontFace);
	obs_data_set_int(fontObj, "flags", 1); /* Bold */

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", paddedText.c_str());
	obs_data_set_obj(settings, "font", fontObj);
	obs_data_set_int(settings, "color1", STATUS_TEXT_COLOR);
	obs_data_set_int(settings, "color2", STATUS_TEXT_COLOR);
	obs_data_set_bool(settings, "outline", false);
	obs_data_set_bool(settings, "drop_shadow", false);

	obs_source_t *src = obs_source_create_private("text_ft2_source_v2", srcName.c_str(), settings);
	obs_data_release(settings);
	obs_data_release(fontObj);

	if (!src) {
		/* Fallback: try legacy text_ft2_source. Same pattern as label.cpp. */
		fontObj = obs_data_create();
		obs_data_set_int(fontObj, "size", STATUS_TEXT_RENDER_SIZE);
		obs_data_set_string(fontObj, "face", fontFace);
		obs_data_set_int(fontObj, "flags", 1);

		settings = obs_data_create();
		obs_data_set_string(settings, "text", paddedText.c_str());
		obs_data_set_obj(settings, "font", fontObj);
		obs_data_set_int(settings, "color1", STATUS_TEXT_COLOR);
		obs_data_set_int(settings, "color2", STATUS_TEXT_COLOR);
		src = obs_source_create_private("text_ft2_source", srcName.c_str(), settings);
		obs_data_release(settings);
		obs_data_release(fontObj);
	}
#endif

	if (!src) {
		obs_log(LOG_WARNING, "status overlay: failed to create text source for '%s'", text);
		return;
	}

	entry.source = src;
	entry.width = 0;
	entry.height = 0;
	obs_source_release(src);
}

void MultiviewWindow::release_status_text_sources()
{
	/* OBSSource (RAII wrapper) drops its strong ref on assignment. We're
	 * either on the UI thread inside `release_source_refs()` or on window
	 * destruction, so there's no graphics-lock concern. */
	status_missing_source_.source = nullptr;
	status_missing_source_.width = 0;
	status_missing_source_.height = 0;
	status_signal_lost_.source = nullptr;
	status_signal_lost_.width = 0;
	status_signal_lost_.height = 0;
	status_reconnecting_.source = nullptr;
	status_reconnecting_.width = 0;
	status_reconnecting_.height = 0;
	status_fallback_.source = nullptr;
	status_fallback_.width = 0;
	status_fallback_.height = 0;
}

MultiviewWindow::StatusOverlayKind MultiviewWindow::status_overlay_kind_for_state(SignalRuntimeState state) const
{
	switch (state) {
	case SignalRuntimeState::MissingInternal:
		return StatusOverlayKind::MissingSource;
	case SignalRuntimeState::Lost:
	case SignalRuntimeState::Error:
		return StatusOverlayKind::SignalLost;
	case SignalRuntimeState::Connecting:
	case SignalRuntimeState::RetryScheduled:
		return StatusOverlayKind::Reconnecting;
	case SignalRuntimeState::FallbackActive:
		return StatusOverlayKind::Fallback;
	default:
		return StatusOverlayKind::None;
	}
}

/* ---- public render hook ---- */

void MultiviewWindow::render_status_overlay(int cellIndex, int cellX, int cellY, int cellW, int cellH)
{
	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return;
	if (cellW <= 0 || cellH <= 0)
		return;

	const auto &cs = cell_sources_[cellIndex];
	const StatusOverlayKind kind = status_overlay_kind_for_state(cs.state);
	if (kind == StatusOverlayKind::None)
		return;

	/* Phase 3 / M5.1b first pass: only MissingInternal is wired up. Other
	 * kinds are reserved for later milestones (M5.3 Reconnecting, M5.4
	 * Fallback, M6 Lost). They short-circuit until then so we never draw
	 * an empty banner for an un-resourced kind. */
	StatusTextEntry *entry = nullptr;
	const char *text = nullptr;
	uint32_t bandColor = 0xC0202020; /* default: 75% black */
	switch (kind) {
	case StatusOverlayKind::MissingSource:
		text = "MISSING SOURCE";
		bandColor = 0xC0202020;
		entry = &status_missing_source_;
		break;
	default:
		return;
	}

	ensure_status_text_source(*entry, text);
	if (!entry->source)
		return;

	/* Skip on micro-cells: the band would dominate the cell and provide
	 * no signal beyond the existing black-fill. 80x32 is the smallest
	 * size that keeps the text legible at 1080p. */
	if (cellW < 80 || cellH < 32)
		return;

	/* Compute band geometry: a horizontal band centered vertically. The
	 * band height is bounded both relatively (18% of cell) and absolutely
	 * (24~64 px) so it's recognizable across 1x1..10x10 layouts.   */
	int bandH = (int)(cellH * 0.18 + 0.5);
	if (bandH < 24)
		bandH = 24;
	if (bandH > 64)
		bandH = 64;
	int bandY = cellY + (cellH - bandH) / 2;

	/* Draw the translucent band using the solid effect, matching the
	 * gutter-fill approach already used for cell backgrounds. */
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	startRegion(cellX, bandY, cellW, bandH, 0.0f, (float)cellW, 0.0f, (float)bandH);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_effect_set_color(colorParam, bandColor);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, cellW, bandH);
	gs_blend_state_pop();
	endRegion();

	/* Refresh cached text dimensions. text_gdiplus may report 0 until the
	 * first frame after creation; we'll redraw next render once non-zero. */
	uint32_t srcW = obs_source_get_width(entry->source);
	uint32_t srcH = obs_source_get_height(entry->source);
	entry->width = srcW;
	entry->height = srcH;
	if (srcW == 0 || srcH == 0)
		return;

	/* Fit text inside the band, leaving 8 px horizontal padding. Source
	 * texture is rendered at STATUS_TEXT_RENDER_SIZE so we always scale
	 * down — texture-space sampling stays sharp. */
	int textMaxW = cellW - 16;
	if (textMaxW < 1)
		textMaxW = 1;
	int textMaxH = bandH - 8;
	if (textMaxH < 1)
		textMaxH = 1;

	double scale = std::min((double)textMaxW / (double)srcW, (double)textMaxH / (double)srcH);
	if (scale <= 0.0)
		return;
	int drawW = (int)((double)srcW * scale + 0.5);
	int drawH = (int)((double)srcH * scale + 0.5);
	if (drawW < 1 || drawH < 1)
		return;
	int drawX = cellX + (cellW - drawW) / 2;
	int drawY = bandY + (bandH - drawH) / 2;

	startRegion(drawX, drawY, drawW, drawH, 0.0f, (float)srcW, 0.0f, (float)srcH);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(entry->source);
	gs_blend_state_pop();
	endRegion();
}
