/*
OBS Advanced Multiview - Phase 3 / M5 status overlay (Missing Source, Signal Lost, ...)
Split from multiview-window.cpp for maintainability.
All functions remain members of AmvInstanceCore.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-i18n.hpp"
#include "signal-provider.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <plugin-support.h>

#include <algorithm>
#include <string>

/* Phase 3 / M6.6: log prefix helper. Returns "[<inst-name>(<uuid8>)] " so
 * obs_log lines can be matched back to the originating multiview instance
 * even when several windows are open in parallel. Resolved each call so
 * instance renames take effect immediately; the lookup is a single
 * config_->find_instance(uuid_) which is cheap. */
std::string AmvInstanceCore::log_prefix() const
{
	const std::string short_uuid = uuid_.size() > 8 ? uuid_.substr(0, 8) : uuid_;
	MultiviewInstance *inst = config_ ? config_->find_instance(uuid_) : nullptr;
	const std::string &name = inst ? inst->name : std::string();
	std::string out = "[";
	out += name.empty() ? "?" : name;
	out += "(";
	out += short_uuid.empty() ? "?" : short_uuid;
	out += ")] ";
	return out;
}

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

static const char *default_status_font_face()
{
#ifdef _WIN32
	return "Arial";
#elif __APPLE__
	return "Helvetica";
#else
	return "Monospace";
#endif
}

/* ---- private helpers ---- */

void AmvInstanceCore::ensure_status_text_source(StatusTextEntry &entry, const char *text, const std::string &fontFamily)
{
	if (!text || !*text)
		return;
	if (entry.source && entry.fontFamily == fontFamily)
		return;
	entry.source = nullptr;
	entry.width = 0;
	entry.height = 0;
	entry.fontFamily = fontFamily;

	/* Pad with spaces so the resulting source has a small horizontal margin
	 * matching the OBS native multiview label aesthetic. */
	std::string paddedText = std::string(" ") + text + " ";

	std::string srcName = "adv_mv_status_" + uuid_ + "_" + text;

#ifdef _WIN32
	/* Windows: text_gdiplus_v2 has solid CJK font fallback and matches what
	 * `rebuild_label_sources()` already uses. Bold weight makes the overlay
	 * stand out without needing an outline. */
	const char *fontFace = fontFamily.empty() ? default_status_font_face() : fontFamily.c_str();
	obs_data_t *fontObj = obs_data_create();
	obs_data_set_int(fontObj, "size", STATUS_TEXT_RENDER_SIZE);
	obs_data_set_string(fontObj, "face", fontFace);
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
	const char *fontFace = fontFamily.empty() ? default_status_font_face() : fontFamily.c_str();
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

void AmvInstanceCore::release_status_text_sources()
{
	/* OBSSource (RAII wrapper) drops its strong ref on assignment. We're
	 * either on the UI thread inside `release_source_refs()` or on window
	 * destruction, so there's no graphics-lock concern. */
	status_missing_source_.source = nullptr;
	status_missing_source_.width = 0;
	status_missing_source_.height = 0;
	status_missing_source_.fontFamily.clear();
	status_missing_scene_.source = nullptr;
	status_missing_scene_.width = 0;
	status_missing_scene_.height = 0;
	status_missing_scene_.fontFamily.clear();
	status_signal_lost_.source = nullptr;
	status_signal_lost_.width = 0;
	status_signal_lost_.height = 0;
	status_signal_lost_.fontFamily.clear();
	status_reconnecting_.source = nullptr;
	status_reconnecting_.width = 0;
	status_reconnecting_.height = 0;
	status_reconnecting_.fontFamily.clear();
	status_fallback_.source = nullptr;
	status_fallback_.width = 0;
	status_fallback_.height = 0;
	status_fallback_.fontFamily.clear();
	status_provider_missing_.source = nullptr;
	status_provider_missing_.width = 0;
	status_provider_missing_.height = 0;
	status_provider_missing_.fontFamily.clear();
	status_paused_.source = nullptr;
	status_paused_.width = 0;
	status_paused_.height = 0;
	status_paused_.fontFamily.clear();
	status_audio_only_.source = nullptr;
	status_audio_only_.width = 0;
	status_audio_only_.height = 0;
	status_audio_only_.fontFamily.clear();
}

AmvInstanceCore::StatusOverlayKind AmvInstanceCore::status_overlay_kind_for_state(SignalRuntimeState state,
										  const std::string &cellType,
										  SignalProviderType providerType) const
{
	switch (state) {
	case SignalRuntimeState::MissingInternal:
		/* Differentiate the wording so the user immediately knows
		 * whether OBS lost a scene assignment or a non-scene source
		 * (image, capture, browser, etc). cellType comes from the
		 * CellAssignment that fed CellSource — "scene" / "source" /
		 * "pgm" / "prvw"; the latter two are always Active so they
		 * never reach this path. */
		if (cellType == "scene")
			return StatusOverlayKind::MissingScene;
		return StatusOverlayKind::MissingSource;
	case SignalRuntimeState::Lost:
	case SignalRuntimeState::Error: {
		/* Phase 3 / M6.2: external cell in Error — distinguish between
		 * 'host plugin missing' (DistroAV not installed, obs-spout2
		 * uninstalled, etc.) and 'plugin installed but the source
		 * went away' so the user gets actionable feedback instead of
		 * a generic SIGNAL LOST. The provider registry returns null /
		 * unavailable when the host integration isn't loaded; either
		 * case means recreating the cell now is hopeless until the
		 * user installs/re-enables the plugin. */
		if (providerType != SignalProviderType::Unknown && !signal_provider_is_internal(providerType)) {
			const auto *p = SignalProviderRegistry::instance().find(providerType);
			if (!p || !p->is_available())
				return StatusOverlayKind::ProviderMissing;
		}
		return StatusOverlayKind::SignalLost;
	}
	case SignalRuntimeState::RetryScheduled:
		return StatusOverlayKind::Reconnecting;
	case SignalRuntimeState::Connecting:
		/* Phase 3 / M6.6 hardening: the brief window between
		 * refresh_cell installing a new private_source and the
		 * provider reporting its first verdict (typical 100-300 ms
		 * for ffmpeg / vlc; up to kConnectingTotalNs == 30 s before
		 * the supervisor escalates to Lost). Without this case the
		 * default branch returned None and the cell rendered a
		 * blank black frame — visually identical to "no source
		 * configured", which is wrong: a recreate IS in flight.
		 *
		 * Use the same blue CONNECTING... band as RetryScheduled —
		 * from the user's perspective both states mean "we are
		 * actively trying to bring this cell back up". The two
		 * states differ only in supervisor bookkeeping (waiting for
		 * the cooldown timer vs waiting for the first frame). */
		return StatusOverlayKind::Reconnecting;
	case SignalRuntimeState::FallbackActive:
		return StatusOverlayKind::Fallback;
	case SignalRuntimeState::Paused:
		return StatusOverlayKind::Paused;
	default:
		return StatusOverlayKind::None;
	}
}

/* ---- public render hook ---- */

void AmvInstanceCore::render_status_overlay(int cellIndex, int cellX, int cellY, int cellW, int cellH)
{
	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return;
	if (cellW <= 0 || cellH <= 0)
		return;

	const auto &cs = cell_sources_[cellIndex];

	/* Phase 3 / M5.1 ClearCell: while we wait for the queued main-thread
	 * mutation to fire, render the cell as Empty so MISSING SOURCE doesn't
	 * flash for the 1-2 frames between source_remove and the timer firing. */
	if (cs.pending_clear)
		return;

	const StatusOverlayKind kind = (cs.audio_only && cs.state == SignalRuntimeState::Active)
					       ? StatusOverlayKind::AudioOnly
					       : status_overlay_kind_for_state(cs.state, cs.type, cs.provider_type);
	if (kind == StatusOverlayKind::None)
		return;

	/* Phase 3 / M5.1b first pass: only MissingInternal is wired up. Other
	 * kinds are reserved for later milestones (M5.3 Reconnecting, M5.4
	 * Fallback, M6 Lost). They short-circuit until then so we never draw
	 * an empty banner for an un-resourced kind. */
	StatusTextEntry *entry = nullptr;
	const char *text = nullptr;
	QByteArray textBytes;
	auto set_text_key = [&](const char *key, const char *fallback) {
		textBytes = amv::text_or(key, fallback).toUtf8();
		text = textBytes.constData();
	};
	uint32_t bandColor = 0xC0202020; /* default: 75% black */
	switch (kind) {
	case StatusOverlayKind::MissingSource:
		set_text_key("AMVPlugin.Status.MissingSource", "MISSING SOURCE");
		bandColor = 0xC0601020; /* same red family as SIGNAL LOST */
		entry = &status_missing_source_;
		break;
	case StatusOverlayKind::MissingScene:
		set_text_key("AMVPlugin.Status.MissingScene", "MISSING SCENE");
		bandColor = 0xC0601020;
		entry = &status_missing_scene_;
		break;
	case StatusOverlayKind::Fallback:
		/* Phase 3 / M5.4: when a cell renders a Lost-Signal fallback
		 * (PGM / PRVW / Scene / Source / static image) instead of its
		 * configured source, surface that with a translucent yellow band
		 * so the user can tell at a glance the cell is on a fallback,
		 * not the real assignment. */
		set_text_key("AMVPlugin.Status.Fallback", "FALLBACK");
		bandColor = 0xC0806000; /* warm amber, ~75% opacity */
		entry = &status_fallback_;
		break;
	case StatusOverlayKind::Reconnecting:
		/* Phase 3 / M6 step 10: external-cell health supervisor put
		 * the cell in Connecting / RetryScheduled. Blue band so it's
		 * distinct from MISSING (grey) and SIGNAL LOST (red).
		 *
		 * Same overlay covers both initial connection (cell just
		 * created, source still resolving its URL / opening codec)
		 * and post-Lost retry escalation. Wording is "CONNECTING..."
		 * for both cases since the supervisor doesn't distinguish
		 * them and "RECONNECTING" reads awkwardly during the very
		 * first attempt. */
		set_text_key("AMVPlugin.Status.Connecting", "CONNECTING...");
		bandColor = 0xC0204060; /* deep blue, ~75% opacity */
		entry = &status_reconnecting_;
		break;
	case StatusOverlayKind::SignalLost:
		/* Phase 3 / M6 step 10: external cell escalated to Lost / Error.
		 * Red band signals the user the source is gone and may not
		 * recover without intervention (Reconnect Now / Edit Source). */
		set_text_key("AMVPlugin.Status.SignalLost", "SIGNAL LOST");
		bandColor = 0xC0601020; /* dark red, ~75% opacity */
		entry = &status_signal_lost_;
		break;
	case StatusOverlayKind::ProviderMissing:
		/* Phase 3 / M6.2: host plugin (DistroAV / obs-spout2 / VLC) is
		 * not installed or failed to load. Distinct purple band so the
		 * user knows the fix is to install the missing plugin, not to
		 * troubleshoot the network / source. */
		set_text_key("AMVPlugin.Status.ProviderMissing", "PROVIDER MISSING");
		bandColor = 0xC0401060; /* deep purple, ~75% opacity */
		entry = &status_provider_missing_;
		break;
	case StatusOverlayKind::Paused:
		/* Phase 3 / M6.6: user pressed Play/Pause from the cell
		 * context menu (obs_source_media_play_pause). Not a failure;
		 * the cell paints the last decoded frame. Soft cyan band so it
		 * reads as informational rather than an error state. */
		set_text_key("AMVPlugin.Status.Paused", "PAUSED");
		bandColor = 0xC0205060; /* desaturated teal, ~75% opacity */
		entry = &status_paused_;
		break;
	case StatusOverlayKind::AudioOnly:
		set_text_key("AMVPlugin.Status.AudioOnly", "AUDIO ONLY");
		bandColor = 0xC0204060; /* same blue family as CONNECTING... */
		entry = &status_audio_only_;
		break;
	default:
		return;
	}

	const LabelSettings *labelSettings =
		cellIndex < (int)effective_visuals_.size() ? &effective_visuals_[cellIndex].label : nullptr;
	std::string fontFamily;
	if (labelSettings)
		fontFamily = labelSettings->statusFontFamily.empty() ? labelSettings->fontFamily
								     : labelSettings->statusFontFamily;
	ensure_status_text_source(*entry, text, fontFamily);
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
