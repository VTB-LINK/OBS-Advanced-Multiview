/*
OBS Advanced Multiview
Copyright (C) 2025 VTB-LINK

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "multiview-instance.hpp"

#include <QUuid>
#include <obs-data.h>
#include <plugin-support.h>

#include <cstring>

/* ========== Enum serialization helpers ========== */

static const char *inheritance_mode_to_str(InheritanceMode m)
{
	switch (m) {
	case InheritanceMode::Override:
		return "override";
	default:
		return "inherit";
	}
}

static InheritanceMode inheritance_mode_from_str(const char *s)
{
	if (s && strcmp(s, "override") == 0)
		return InheritanceMode::Override;
	return InheritanceMode::Inherit;
}

/* Phase 3 / M5: Lost Signal enum string mapping. Strings instead of ints so
 * future additions (e.g. ColorBars, PreviousFrameFreeze) won't shift the
 * numeric meaning of older configs. */
static const char *internal_missing_behavior_to_str(InternalMissingBehavior b)
{
	switch (b) {
	case InternalMissingBehavior::PlaceholderImage:
		return "placeholder_image";
	case InternalMissingBehavior::ClearCell:
		return "clear_cell";
	default:
		return "black";
	}
}

static InternalMissingBehavior internal_missing_behavior_from_str(const char *s)
{
	if (!s)
		return InternalMissingBehavior::Black;
	if (strcmp(s, "placeholder_image") == 0)
		return InternalMissingBehavior::PlaceholderImage;
	if (strcmp(s, "clear_cell") == 0)
		return InternalMissingBehavior::ClearCell;
	return InternalMissingBehavior::Black;
}

static const char *external_lost_behavior_to_str(ExternalLostBehavior b)
{
	switch (b) {
	case ExternalLostBehavior::RetryOnly:
		return "retry_only";
	case ExternalLostBehavior::RetryWithFallback:
		return "retry_with_fallback";
	case ExternalLostBehavior::SignalLostImage:
		return "signal_lost_image";
	default:
		return "signal_lost_overlay";
	}
}

static ExternalLostBehavior external_lost_behavior_from_str(const char *s)
{
	if (!s)
		return ExternalLostBehavior::SignalLostOverlay;
	if (strcmp(s, "retry_only") == 0)
		return ExternalLostBehavior::RetryOnly;
	if (strcmp(s, "retry_with_fallback") == 0)
		return ExternalLostBehavior::RetryWithFallback;
	if (strcmp(s, "signal_lost_image") == 0)
		return ExternalLostBehavior::SignalLostImage;
	return ExternalLostBehavior::SignalLostOverlay;
}

static const char *label_display_mode_to_str(LabelDisplayMode m)
{
	switch (m) {
	case LabelDisplayMode::Overlay:
		return "overlay";
	case LabelDisplayMode::Below:
		return "below";
	default:
		return "none";
	}
}

static LabelDisplayMode label_display_mode_from_str(const char *s)
{
	if (!s)
		return LabelDisplayMode::None;
	if (strcmp(s, "overlay") == 0)
		return LabelDisplayMode::Overlay;
	if (strcmp(s, "below") == 0)
		return LabelDisplayMode::Below;
	return LabelDisplayMode::None;
}

static const char *label_position_to_str(LabelPosition p)
{
	switch (p) {
	case LabelPosition::Top:
		return "top";
	default:
		return "bottom";
	}
}

static LabelPosition label_position_from_str(const char *s)
{
	if (s && strcmp(s, "top") == 0)
		return LabelPosition::Top;
	return LabelPosition::Bottom;
}

static const char *font_scale_mode_to_str(FontScaleMode m)
{
	switch (m) {
	case FontScaleMode::ScaleWithCell:
		return "scaleWithCell";
	default:
		return "fixed";
	}
}

static FontScaleMode font_scale_mode_from_str(const char *s)
{
	if (s && strcmp(s, "scaleWithCell") == 0)
		return FontScaleMode::ScaleWithCell;
	return FontScaleMode::Fixed;
}

static const char *image_fit_mode_to_str(ImageFitMode m)
{
	switch (m) {
	case ImageFitMode::Stretch:
		return "stretch";
	default:
		return "fit";
	}
}

static ImageFitMode image_fit_mode_from_str(const char *s)
{
	if (s && strcmp(s, "stretch") == 0)
		return ImageFitMode::Stretch;
	return ImageFitMode::Fit;
}

static const char *bg_fill_mode_to_str(BackgroundFillMode m)
{
	switch (m) {
	case BackgroundFillMode::FillEntireCell:
		return "cell";
	default:
		return "signal";
	}
}

static BackgroundFillMode bg_fill_mode_from_str(const char *s)
{
	if (s && strcmp(s, "cell") == 0)
		return BackgroundFillMode::FillEntireCell;
	return BackgroundFillMode::FillSignalOnly;
}

static const char *safe_area_preset_to_str(SafeAreaPreset p)
{
	(void)p;
	return "ebu_r95";
}

static SafeAreaPreset safe_area_preset_from_str(const char *s)
{
	(void)s;
	return SafeAreaPreset::EBU_R95;
}

static const char *vu_meter_position_to_str(VuMeterPosition p)
{
	switch (p) {
	case VuMeterPosition::Left:
		return "left";
	case VuMeterPosition::Bottom:
		return "bottom";
	case VuMeterPosition::Top:
		return "top";
	default:
		return "right";
	}
}

static VuMeterPosition vu_meter_position_from_str(const char *s)
{
	if (!s)
		return VuMeterPosition::Right;
	if (strcmp(s, "left") == 0)
		return VuMeterPosition::Left;
	if (strcmp(s, "bottom") == 0)
		return VuMeterPosition::Bottom;
	if (strcmp(s, "top") == 0)
		return VuMeterPosition::Top;
	return VuMeterPosition::Right;
}

static const char *vu_meter_anchor_to_str(VuMeterAnchorMode a)
{
	switch (a) {
	case VuMeterAnchorMode::Signal:
		return "signal";
	default:
		return "cell";
	}
}

static VuMeterAnchorMode vu_meter_anchor_from_str(const char *s)
{
	if (!s)
		return VuMeterAnchorMode::Cell;
	if (strcmp(s, "signal") == 0)
		return VuMeterAnchorMode::Signal;
	return VuMeterAnchorMode::Cell;
}

static const char *vu_meter_decay_to_str(VuMeterDecayRate r)
{
	switch (r) {
	case VuMeterDecayRate::Medium:
		return "medium";
	case VuMeterDecayRate::Slow:
		return "slow";
	default:
		return "fast";
	}
}

static VuMeterDecayRate vu_meter_decay_from_str(const char *s)
{
	if (!s)
		return VuMeterDecayRate::Fast;
	if (strcmp(s, "medium") == 0)
		return VuMeterDecayRate::Medium;
	if (strcmp(s, "slow") == 0)
		return VuMeterDecayRate::Slow;
	return VuMeterDecayRate::Fast;
}

static const char *vu_meter_alignment_to_str(VuMeterAlignment a)
{
	switch (a) {
	case VuMeterAlignment::Start:
		return "start";
	default:
		return "center";
	}
}

static VuMeterAlignment vu_meter_alignment_from_str(const char *s)
{
	if (!s)
		return VuMeterAlignment::Center;
	if (strcmp(s, "start") == 0)
		return VuMeterAlignment::Start;
	return VuMeterAlignment::Center;
}

static const char *vu_meter_track_mode_to_str(VuMeterTrackMode m)
{
	switch (m) {
	case VuMeterTrackMode::Manual:
		return "manual";
	case VuMeterTrackMode::Auto:
		return "auto";
	case VuMeterTrackMode::ExternalSource:
		return "external_source";
	default:
		return "auto_follow_streaming";
	}
}

static VuMeterTrackMode vu_meter_track_mode_from_str(const char *s)
{
	if (!s)
		return VuMeterTrackMode::AutoFollowStreaming;
	if (strcmp(s, "manual") == 0)
		return VuMeterTrackMode::Manual;
	if (strcmp(s, "auto") == 0)
		return VuMeterTrackMode::Auto;
	if (strcmp(s, "external_source") == 0)
		return VuMeterTrackMode::ExternalSource;
	return VuMeterTrackMode::AutoFollowStreaming;
}

static const char *vu_meter_scale_side_to_str(VuMeterScaleSide side)
{
	switch (side) {
	case VuMeterScaleSide::Same:
		return "same";
	case VuMeterScaleSide::Opposite:
		return "opposite";
	default:
		return "auto";
	}
}

static VuMeterScaleSide vu_meter_scale_side_from_str(const char *s)
{
	if (!s)
		return VuMeterScaleSide::Auto;
	if (strcmp(s, "same") == 0)
		return VuMeterScaleSide::Same;
	if (strcmp(s, "opposite") == 0)
		return VuMeterScaleSide::Opposite;
	return VuMeterScaleSide::Auto;
}

static const char *vu_meter_style_to_str(VuMeterStyle st)
{
	(void)st;
	return "bar";
}

static VuMeterStyle vu_meter_style_from_str(const char *s)
{
	(void)s;
	return VuMeterStyle::Bar;
}

static const char *overlay_fit_mode_to_str(OverlayFitMode m)
{
	switch (m) {
	case OverlayFitMode::Stretch:
		return "stretch";
	default:
		return "fit";
	}
}

static OverlayFitMode overlay_fit_mode_from_str(const char *s)
{
	if (s && strcmp(s, "stretch") == 0)
		return OverlayFitMode::Stretch;
	return OverlayFitMode::Fit;
}

static const char *overlay_anchor_mode_to_str(OverlayAnchorMode m)
{
	switch (m) {
	case OverlayAnchorMode::Cell:
		return "cell";
	default:
		return "signal";
	}
}

static OverlayAnchorMode overlay_anchor_mode_from_str(const char *s)
{
	if (s && strcmp(s, "cell") == 0)
		return OverlayAnchorMode::Cell;
	return OverlayAnchorMode::Signal;
}

static const char *safe_area_anchor_mode_to_str(SafeAreaAnchorMode m)
{
	switch (m) {
	case SafeAreaAnchorMode::Cell:
		return "cell";
	default:
		return "signal";
	}
}

static SafeAreaAnchorMode safe_area_anchor_mode_from_str(const char *s)
{
	if (s && strcmp(s, "cell") == 0)
		return SafeAreaAnchorMode::Cell;
	if (s && strcmp(s, "signal") == 0)
		return SafeAreaAnchorMode::Signal;
	return SafeAreaAnchorMode::Cell;
}

/* ========== BackgroundSettings ========== */

obs_data_t *BackgroundSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "colorEnabled", colorEnabled);
	obs_data_set_int(data, "color", (long long)color);
	obs_data_set_string(data, "fillMode", bg_fill_mode_to_str(fillMode));
	obs_data_set_bool(data, "labelRegionFill", labelRegionFill);
	obs_data_set_bool(data, "imageEnabled", imageEnabled);
	obs_data_set_string(data, "imagePath", imagePath.c_str());
	obs_data_set_string(data, "imageFitMode", image_fit_mode_to_str(imageFitMode));
	return data;
}

BackgroundSettings BackgroundSettings::from_obs_data(obs_data_t *data)
{
	BackgroundSettings s;
	if (!data)
		return s;
	if (obs_data_has_user_value(data, "colorEnabled"))
		s.colorEnabled = obs_data_get_bool(data, "colorEnabled");
	s.color = (uint32_t)obs_data_get_int(data, "color");
	if (!obs_data_has_user_value(data, "color"))
		s.color = 0xFF000000;
	if (obs_data_has_user_value(data, "fillMode"))
		s.fillMode = bg_fill_mode_from_str(obs_data_get_string(data, "fillMode"));
	s.labelRegionFill = obs_data_get_bool(data, "labelRegionFill");
	s.imageEnabled = obs_data_get_bool(data, "imageEnabled");
	s.imagePath = obs_data_get_string(data, "imagePath");
	/* Phase 3 / M6.6 H.5 hardening: clamp path length, same idiom as
	 * LabelSettings::fontFamily and LostSignalSettings paths. */
	if (s.imagePath.size() > 4096)
		s.imagePath.resize(4096);
	s.imageFitMode = image_fit_mode_from_str(obs_data_get_string(data, "imageFitMode"));
	return s;
}

/* ========== LabelSettings ========== */

obs_data_t *LabelSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "displayMode", label_display_mode_to_str(displayMode));
	obs_data_set_string(data, "position", label_position_to_str(position));
	obs_data_set_string(data, "fontFamily", fontFamily.c_str());
	obs_data_set_int(data, "fontSize", fontSize);
	obs_data_set_string(data, "fontScaleMode", font_scale_mode_to_str(fontScaleMode));
	obs_data_set_int(data, "minFontSize", minFontSize);
	obs_data_set_int(data, "maxFontSize", maxFontSize);
	obs_data_set_int(data, "textColor", (long long)textColor);
	obs_data_set_double(data, "backgroundOpacity", backgroundOpacity);
	obs_data_set_bool(data, "backgroundRounded", backgroundRounded);
	obs_data_set_int(data, "margin", margin);
	return data;
}

LabelSettings LabelSettings::from_obs_data(obs_data_t *data)
{
	LabelSettings s;
	if (!data)
		return s;
	s.displayMode = label_display_mode_from_str(obs_data_get_string(data, "displayMode"));
	s.position = label_position_from_str(obs_data_get_string(data, "position"));
	s.fontFamily = obs_data_get_string(data, "fontFamily");
	/* Defensive: clamp fontFamily length to prevent pathological strings
	 * from a manually edited config from breaking Qt font enumeration. */
	if (s.fontFamily.size() > 128)
		s.fontFamily.resize(128);
	s.fontSize = (int)obs_data_get_int(data, "fontSize");
	if (s.fontSize < 1)
		s.fontSize = 14;
	if (s.fontSize > 200)
		s.fontSize = 200;
	if (obs_data_has_user_value(data, "fontScaleMode"))
		s.fontScaleMode = font_scale_mode_from_str(obs_data_get_string(data, "fontScaleMode"));
	s.minFontSize = (int)obs_data_get_int(data, "minFontSize");
	if (s.minFontSize < 1)
		s.minFontSize = 8;
	if (s.minFontSize > 200)
		s.minFontSize = 200;
	s.maxFontSize = (int)obs_data_get_int(data, "maxFontSize");
	if (!obs_data_has_user_value(data, "maxFontSize"))
		s.maxFontSize = 80;
	if (s.maxFontSize < s.minFontSize)
		s.maxFontSize = s.minFontSize;
	if (s.maxFontSize > 400)
		s.maxFontSize = 400;
	s.textColor = (uint32_t)obs_data_get_int(data, "textColor");
	if (!obs_data_has_user_value(data, "textColor"))
		s.textColor = 0xFFFFFFFF;
	s.backgroundOpacity = obs_data_get_double(data, "backgroundOpacity");
	if (!obs_data_has_user_value(data, "backgroundOpacity"))
		s.backgroundOpacity = 0.2;
	if (s.backgroundOpacity < 0.0)
		s.backgroundOpacity = 0.0;
	if (s.backgroundOpacity > 1.0)
		s.backgroundOpacity = 1.0;
	s.backgroundRounded = obs_data_get_bool(data, "backgroundRounded");
	s.margin = (int)obs_data_get_int(data, "margin");
	if (!obs_data_has_user_value(data, "margin"))
		s.margin = 4;
	if (s.margin < 0)
		s.margin = 0;
	if (s.margin > 200)
		s.margin = 200;
	return s;
}

/* ========== SafeAreaSettings ========== */

obs_data_t *SafeAreaSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_string(data, "preset", safe_area_preset_to_str(preset));
	obs_data_set_string(data, "anchorMode", safe_area_anchor_mode_to_str(anchorMode));
	obs_data_set_int(data, "color", (long long)color);
	obs_data_set_double(data, "opacity", opacity);
	return data;
}

SafeAreaSettings SafeAreaSettings::from_obs_data(obs_data_t *data)
{
	SafeAreaSettings s;
	if (!data)
		return s;
	s.enabled = obs_data_get_bool(data, "enabled");
	s.preset = safe_area_preset_from_str(obs_data_get_string(data, "preset"));
	s.anchorMode = safe_area_anchor_mode_from_str(obs_data_get_string(data, "anchorMode"));
	s.color = (uint32_t)obs_data_get_int(data, "color");
	if (!obs_data_has_user_value(data, "color"))
		s.color = 0xFFD0D0D0;
	s.opacity = obs_data_get_double(data, "opacity");
	if (!obs_data_has_user_value(data, "opacity"))
		s.opacity = 1.0;
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;
	return s;
}

/* ========== VuMeterSettings ========== */

obs_data_t *VuMeterSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_string(data, "position", vu_meter_position_to_str(position));
	obs_data_set_double(data, "opacity", opacity);
	obs_data_set_int(data, "width", width);
	obs_data_set_string(data, "style", vu_meter_style_to_str(style));
	obs_data_set_string(data, "anchor", vu_meter_anchor_to_str(anchor));
	obs_data_set_bool(data, "flip", flip);
	obs_data_set_double(data, "lengthRatio", lengthRatio);
	obs_data_set_double(data, "warningDB", warningDB);
	obs_data_set_double(data, "errorDB", errorDB);
	obs_data_set_string(data, "decayRate", vu_meter_decay_to_str(decayRate));
	obs_data_set_string(data, "alignment", vu_meter_alignment_to_str(alignment));
	obs_data_set_string(data, "trackMode", vu_meter_track_mode_to_str(trackMode));
	obs_data_set_int(data, "manualTrackIndex", manualTrackIndex);
	/* Peak Hold */
	obs_data_set_bool(data, "peakHoldEnabled", peakHoldEnabled);
	obs_data_set_int(data, "peakHoldMs", peakHoldMs);
	obs_data_set_double(data, "peakHoldDecayDbPerSec", peakHoldDecayDbPerSec);
	obs_data_set_int(data, "peakHoldWidthPx", peakHoldWidthPx);
	/* dB Scale */
	obs_data_set_bool(data, "scaleEnabled", scaleEnabled);
	obs_data_set_string(data, "scaleTicks", scaleTicks.c_str());
	obs_data_set_bool(data, "scaleShowLabels", scaleShowLabels);
	obs_data_set_int(data, "scaleColor", (long long)scaleColor);
	obs_data_set_string(data, "scaleSide", vu_meter_scale_side_to_str(scaleSide));
	return data;
}

VuMeterSettings VuMeterSettings::from_obs_data(obs_data_t *data)
{
	VuMeterSettings s;
	if (!data)
		return s;
	if (obs_data_has_user_value(data, "enabled"))
		s.enabled = obs_data_get_bool(data, "enabled");
	if (obs_data_has_user_value(data, "position"))
		s.position = vu_meter_position_from_str(obs_data_get_string(data, "position"));
	s.opacity = obs_data_get_double(data, "opacity");
	if (!obs_data_has_user_value(data, "opacity"))
		s.opacity = 0.75;
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;
	s.width = (int)obs_data_get_int(data, "width");
	if (!obs_data_has_user_value(data, "width"))
		s.width = 24;
	if (s.width < 1)
		s.width = 8;
	if (s.width > 64)
		s.width = 64;
	s.style = vu_meter_style_from_str(obs_data_get_string(data, "style"));
	s.anchor = vu_meter_anchor_from_str(obs_data_get_string(data, "anchor"));
	s.flip = obs_data_get_bool(data, "flip");
	s.lengthRatio = obs_data_get_double(data, "lengthRatio");
	if (!obs_data_has_user_value(data, "lengthRatio"))
		s.lengthRatio = 1.0;
	if (s.lengthRatio < 0.0)
		s.lengthRatio = 0.0;
	if (s.lengthRatio > 1.0)
		s.lengthRatio = 1.0;
	s.warningDB = obs_data_get_double(data, "warningDB");
	if (!obs_data_has_user_value(data, "warningDB"))
		s.warningDB = -20.0;
	if (s.warningDB < -96.0)
		s.warningDB = -96.0;
	if (s.warningDB > 0.0)
		s.warningDB = 0.0;
	s.errorDB = obs_data_get_double(data, "errorDB");
	if (!obs_data_has_user_value(data, "errorDB"))
		s.errorDB = -9.0;
	if (s.errorDB < -96.0)
		s.errorDB = -96.0;
	if (s.errorDB > 0.0)
		s.errorDB = 0.0;
	/* Ensure errorDB >= warningDB (red zone above yellow zone on dB axis) */
	if (s.errorDB < s.warningDB)
		s.errorDB = s.warningDB;
	s.decayRate = vu_meter_decay_from_str(obs_data_get_string(data, "decayRate"));
	s.alignment = vu_meter_alignment_from_str(obs_data_get_string(data, "alignment"));
	/* Track routing — added Phase 2 / VU redesign. Old configs without these
	 * fields fall back to AutoFollowStreaming + Track 1 (the v1 behavior is to
	 * pre-existing). */
	s.trackMode = vu_meter_track_mode_from_str(obs_data_get_string(data, "trackMode"));
	if (obs_data_has_user_value(data, "manualTrackIndex"))
		s.manualTrackIndex = (int)obs_data_get_int(data, "manualTrackIndex");
	if (s.manualTrackIndex < 1)
		s.manualTrackIndex = 1;
	if (s.manualTrackIndex > 6)
		s.manualTrackIndex = 6;
	/* Peak Hold (optional, safe defaults for old configs) */
	if (obs_data_has_user_value(data, "peakHoldEnabled"))
		s.peakHoldEnabled = obs_data_get_bool(data, "peakHoldEnabled");
	if (obs_data_has_user_value(data, "peakHoldMs"))
		s.peakHoldMs = (int)obs_data_get_int(data, "peakHoldMs");
	if (s.peakHoldMs < 100)
		s.peakHoldMs = 100;
	if (s.peakHoldMs > 5000)
		s.peakHoldMs = 5000;
	if (obs_data_has_user_value(data, "peakHoldDecayDbPerSec"))
		s.peakHoldDecayDbPerSec = obs_data_get_double(data, "peakHoldDecayDbPerSec");
	if (s.peakHoldDecayDbPerSec < 1.0)
		s.peakHoldDecayDbPerSec = 1.0;
	if (s.peakHoldDecayDbPerSec > 60.0)
		s.peakHoldDecayDbPerSec = 60.0;
	if (obs_data_has_user_value(data, "peakHoldWidthPx"))
		s.peakHoldWidthPx = (int)obs_data_get_int(data, "peakHoldWidthPx");
	if (s.peakHoldWidthPx < 1)
		s.peakHoldWidthPx = 1;
	if (s.peakHoldWidthPx > 4)
		s.peakHoldWidthPx = 4;
	/* dB Scale (optional, safe defaults for old configs) */
	if (obs_data_has_user_value(data, "scaleEnabled"))
		s.scaleEnabled = obs_data_get_bool(data, "scaleEnabled");
	if (obs_data_has_user_value(data, "scaleTicks"))
		s.scaleTicks = obs_data_get_string(data, "scaleTicks");
	if (obs_data_has_user_value(data, "scaleShowLabels"))
		s.scaleShowLabels = obs_data_get_bool(data, "scaleShowLabels");
	if (obs_data_has_user_value(data, "scaleColor"))
		s.scaleColor = (uint32_t)obs_data_get_int(data, "scaleColor");
	if (obs_data_has_user_value(data, "scaleSide"))
		s.scaleSide = vu_meter_scale_side_from_str(obs_data_get_string(data, "scaleSide"));
	return s;
}

/* ========== OverlaySettings ========== */

obs_data_t *OverlaySettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_string(data, "imagePath", imagePath.c_str());
	obs_data_set_double(data, "opacity", opacity);
	obs_data_set_string(data, "fitMode", overlay_fit_mode_to_str(fitMode));
	obs_data_set_string(data, "anchorMode", overlay_anchor_mode_to_str(anchorMode));
	return data;
}

OverlaySettings OverlaySettings::from_obs_data(obs_data_t *data)
{
	OverlaySettings s;
	if (!data)
		return s;
	s.enabled = obs_data_get_bool(data, "enabled");
	s.imagePath = obs_data_get_string(data, "imagePath");
	/* Phase 3 / M6.6 H.5 hardening: clamp path length. */
	if (s.imagePath.size() > 4096)
		s.imagePath.resize(4096);
	s.opacity = obs_data_get_double(data, "opacity");
	if (!obs_data_has_user_value(data, "opacity"))
		s.opacity = 1.0;
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;
	s.fitMode = overlay_fit_mode_from_str(obs_data_get_string(data, "fitMode"));
	if (obs_data_has_user_value(data, "anchorMode"))
		s.anchorMode = overlay_anchor_mode_from_str(obs_data_get_string(data, "anchorMode"));
	return s;
}

/* ========== HighlightSettings ========== */

obs_data_t *HighlightSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_int(data, "pgmColor", pgmColor);
	obs_data_set_int(data, "prvwColor", prvwColor);
	obs_data_set_bool(data, "nestedDashed", nestedDashed);
	obs_data_set_int(data, "dashLengthPx", dashLengthPx);
	obs_data_set_int(data, "dashGapPx", dashGapPx);
	obs_data_set_int(data, "minThicknessPx", minThicknessPx);
	return data;
}

HighlightSettings HighlightSettings::from_obs_data(obs_data_t *data)
{
	HighlightSettings s;
	if (!data)
		return s;
	/* `enabled` defaults to true; only override if the key is present so that
	 * legacy configs (no "highlight" obj at all) keep the default-on behavior. */
	if (obs_data_has_user_value(data, "enabled"))
		s.enabled = obs_data_get_bool(data, "enabled");
	if (obs_data_has_user_value(data, "pgmColor"))
		s.pgmColor = (uint32_t)obs_data_get_int(data, "pgmColor");
	if (obs_data_has_user_value(data, "prvwColor"))
		s.prvwColor = (uint32_t)obs_data_get_int(data, "prvwColor");
	if (obs_data_has_user_value(data, "nestedDashed"))
		s.nestedDashed = obs_data_get_bool(data, "nestedDashed");
	if (obs_data_has_user_value(data, "dashLengthPx"))
		s.dashLengthPx = (int)obs_data_get_int(data, "dashLengthPx");
	if (obs_data_has_user_value(data, "dashGapPx"))
		s.dashGapPx = (int)obs_data_get_int(data, "dashGapPx");
	if (obs_data_has_user_value(data, "minThicknessPx"))
		s.minThicknessPx = (int)obs_data_get_int(data, "minThicknessPx");
	/* Clamps: match the UI spin ranges so an out-of-range value in disk config
	 * cannot crash GS draw paths or produce zero-length dashes. */
	if (s.dashLengthPx < 4)
		s.dashLengthPx = 4;
	if (s.dashLengthPx > 32)
		s.dashLengthPx = 32;
	if (s.dashGapPx < 2)
		s.dashGapPx = 2;
	if (s.dashGapPx > 16)
		s.dashGapPx = 16;
	if (s.minThicknessPx < 2)
		s.minThicknessPx = 2;
	if (s.minThicknessPx > 16)
		s.minThicknessPx = 16;
	return s;
}

/* ========== LostSignalSettings (Phase 3 / M5) ==========
 *
 * fallbackType is intentionally a free-form string mirroring CellAssignment:
 *   - ""        : fallback disabled
 *   - "image"   : fallbackName is an absolute file path
 *   - "pgm"     : current Program scene
 *   - "prvw"    : current Preview scene
 *   - "scene"   : OBS scene name in fallbackName
 *   - "source"  : OBS source name in fallbackName
 * Future external-source fallback can extend this set without breaking
 * persistence; unknown values resolve to "" (disabled) on load.
 */

obs_data_t *LostSignalSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "internalMissingBehavior", internal_missing_behavior_to_str(internalMissingBehavior));
	obs_data_set_string(data, "externalLostBehavior", external_lost_behavior_to_str(externalLostBehavior));
	obs_data_set_string(data, "placeholderImagePath", placeholderImagePath.c_str());
	obs_data_set_string(data, "signalLostImagePath", signalLostImagePath.c_str());
	obs_data_set_string(data, "placeholderImageFitMode", image_fit_mode_to_str(placeholderImageFitMode));
	obs_data_set_string(data, "signalLostImageFitMode", image_fit_mode_to_str(signalLostImageFitMode));
	obs_data_set_string(data, "fallbackImageFitMode", image_fit_mode_to_str(fallbackImageFitMode));
	obs_data_set_string(data, "fallbackType", fallbackType.c_str());
	obs_data_set_string(data, "fallbackName", fallbackName.c_str());
	obs_data_set_int(data, "retryInitialMs", retryInitialMs);
	obs_data_set_int(data, "retryMaxMs", retryMaxMs);
	obs_data_set_int(data, "manualReconnectCooldownMs", manualReconnectCooldownMs);
	return data;
}

LostSignalSettings LostSignalSettings::from_obs_data(obs_data_t *data)
{
	LostSignalSettings s;
	if (!data)
		return s;
	s.internalMissingBehavior =
		internal_missing_behavior_from_str(obs_data_get_string(data, "internalMissingBehavior"));
	s.externalLostBehavior = external_lost_behavior_from_str(obs_data_get_string(data, "externalLostBehavior"));
	s.placeholderImagePath = obs_data_get_string(data, "placeholderImagePath");
	s.signalLostImagePath = obs_data_get_string(data, "signalLostImagePath");

	/* Phase 3 / M6.6 H.5 hardening: clamp path strings to a reasonable
	 * upper bound so a hand-edited config can't blow up the renderer's
	 * gs_image_file loader with a multi-MB filename. PATH_MAX on Windows
	 * is 260 by default, but Win32 long-path support extends it to 32767.
	 * We pick 4096 as a generous compromise: large enough for any real
	 * path the user might pick (including UNC + Unicode), small enough
	 * that bad data is bounded. Same idiom as LabelSettings::fontFamily
	 * clamp from Phase 2 hardening. */
	constexpr size_t kMaxPathBytes = 4096;
	if (s.placeholderImagePath.size() > kMaxPathBytes)
		s.placeholderImagePath.resize(kMaxPathBytes);
	if (s.signalLostImagePath.size() > kMaxPathBytes)
		s.signalLostImagePath.resize(kMaxPathBytes);

	/* Fit modes: missing / unknown values fall back to enum default
	 * (Stretch) via image_fit_mode_from_str() — consistent with how
	 * BackgroundSettings handles the same key on legacy configs. */
	if (obs_data_has_user_value(data, "placeholderImageFitMode"))
		s.placeholderImageFitMode =
			image_fit_mode_from_str(obs_data_get_string(data, "placeholderImageFitMode"));
	if (obs_data_has_user_value(data, "signalLostImageFitMode"))
		s.signalLostImageFitMode = image_fit_mode_from_str(obs_data_get_string(data, "signalLostImageFitMode"));
	if (obs_data_has_user_value(data, "fallbackImageFitMode"))
		s.fallbackImageFitMode = image_fit_mode_from_str(obs_data_get_string(data, "fallbackImageFitMode"));

	/* Whitelist fallbackType so an arbitrary string from disk can never reach
	 * the runtime. Anything unrecognised becomes "" (disabled) and the
	 * fallbackName is kept verbatim so the user can re-enable later. */
	const char *ft = obs_data_get_string(data, "fallbackType");
	if (ft && (strcmp(ft, "image") == 0 || strcmp(ft, "pgm") == 0 || strcmp(ft, "prvw") == 0 ||
		   strcmp(ft, "scene") == 0 || strcmp(ft, "source") == 0)) {
		s.fallbackType = ft;
	} else {
		s.fallbackType.clear();
	}
	s.fallbackName = obs_data_get_string(data, "fallbackName");

	/* Phase 3 / M6.6 H.5 hardening: clamp fallbackName length too. When
	 * fallbackType == "image" this is an absolute path (same renderer as
	 * placeholderImagePath); for "scene"/"source" it's an OBS source name
	 * (which OBS itself caps in practice but our config could carry
	 * arbitrary length). Same 4096 cap as the image-path fields above. */
	if (s.fallbackName.size() > 4096)
		s.fallbackName.resize(4096);

	if (obs_data_has_user_value(data, "retryInitialMs"))
		s.retryInitialMs = (int)obs_data_get_int(data, "retryInitialMs");
	if (obs_data_has_user_value(data, "retryMaxMs"))
		s.retryMaxMs = (int)obs_data_get_int(data, "retryMaxMs");
	if (obs_data_has_user_value(data, "manualReconnectCooldownMs"))
		s.manualReconnectCooldownMs = (int)obs_data_get_int(data, "manualReconnectCooldownMs");

	/* Clamps: match design defaults in [docs/phase-3-signal-lost-and-external-sources-design.md] §10
	 * (1s..30s backoff, 1s manual cooldown). Out-of-range values can come from
	 * hand-edited configs or future versions; clamp instead of reject. */
	if (s.retryInitialMs < 100)
		s.retryInitialMs = 100;
	if (s.retryInitialMs > 60000)
		s.retryInitialMs = 60000;
	if (s.retryMaxMs < s.retryInitialMs)
		s.retryMaxMs = s.retryInitialMs;
	if (s.retryMaxMs > 600000)
		s.retryMaxMs = 600000;
	if (s.manualReconnectCooldownMs < 0)
		s.manualReconnectCooldownMs = 0;
	if (s.manualReconnectCooldownMs > 60000)
		s.manualReconnectCooldownMs = 60000;
	return s;
}

/* ========== CellLostSignalSettings (Phase 3 / M5) ========== */

obs_data_t *CellLostSignalSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "row", row);
	obs_data_set_int(data, "col", col);
	obs_data_set_string(data, "mode", inheritance_mode_to_str(mode));
	obs_data_t *inner = settings.to_obs_data();
	obs_data_set_obj(data, "settings", inner);
	obs_data_release(inner);
	return data;
}

CellLostSignalSettings CellLostSignalSettings::from_obs_data(obs_data_t *data)
{
	CellLostSignalSettings c;
	if (!data)
		return c;
	c.row = (int)obs_data_get_int(data, "row");
	c.col = (int)obs_data_get_int(data, "col");
	c.mode = inheritance_mode_from_str(obs_data_get_string(data, "mode"));
	obs_data_t *inner = obs_data_get_obj(data, "settings");
	c.settings = LostSignalSettings::from_obs_data(inner);
	if (inner)
		obs_data_release(inner);
	return c;
}

LostSignalSettings resolve_effective_lost_signal(const LostSignalSettings &global, const CellLostSignalSettings *cell)
{
	if (cell && cell->mode == InheritanceMode::Override)
		return cell->settings;
	return global;
}

/* ========== SceneClickSwitchSettings ========== */

obs_data_t *SceneClickSwitchSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	return data;
}

SceneClickSwitchSettings SceneClickSwitchSettings::from_obs_data(obs_data_t *data)
{
	SceneClickSwitchSettings s;
	if (!data)
		return s;
	/* Default true (parity with OBS built-in MultiviewMouseSwitch) when key
	 * is absent on legacy configs. */
	if (obs_data_has_user_value(data, "enabled"))
		s.enabled = obs_data_get_bool(data, "enabled");
	return s;
}

/* ========== GlobalVisualSettings ========== */

obs_data_t *GlobalVisualSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();

	obs_data_t *bg = background.to_obs_data();
	obs_data_set_obj(data, "background", bg);
	obs_data_release(bg);

	obs_data_t *lbl = label.to_obs_data();
	obs_data_set_obj(data, "label", lbl);
	obs_data_release(lbl);

	obs_data_t *sa = safeArea.to_obs_data();
	obs_data_set_obj(data, "safeArea", sa);
	obs_data_release(sa);

	obs_data_t *vu = vuMeter.to_obs_data();
	obs_data_set_obj(data, "vuMeter", vu);
	obs_data_release(vu);

	obs_data_t *ov = overlay.to_obs_data();
	obs_data_set_obj(data, "overlay", ov);
	obs_data_release(ov);

	obs_data_t *hl = highlight.to_obs_data();
	obs_data_set_obj(data, "highlight", hl);
	obs_data_release(hl);

	return data;
}

GlobalVisualSettings GlobalVisualSettings::from_obs_data(obs_data_t *data)
{
	GlobalVisualSettings vs;
	if (!data)
		return vs;

	obs_data_t *bg = obs_data_get_obj(data, "background");
	vs.background = BackgroundSettings::from_obs_data(bg);
	if (bg)
		obs_data_release(bg);

	obs_data_t *lbl = obs_data_get_obj(data, "label");
	vs.label = LabelSettings::from_obs_data(lbl);
	if (lbl)
		obs_data_release(lbl);

	obs_data_t *sa = obs_data_get_obj(data, "safeArea");
	vs.safeArea = SafeAreaSettings::from_obs_data(sa);
	if (sa)
		obs_data_release(sa);

	obs_data_t *vu = obs_data_get_obj(data, "vuMeter");
	vs.vuMeter = VuMeterSettings::from_obs_data(vu);
	if (vu)
		obs_data_release(vu);

	obs_data_t *ov = obs_data_get_obj(data, "overlay");
	vs.overlay = OverlaySettings::from_obs_data(ov);
	if (ov)
		obs_data_release(ov);

	obs_data_t *hl = obs_data_get_obj(data, "highlight");
	vs.highlight = HighlightSettings::from_obs_data(hl);
	if (hl)
		obs_data_release(hl);

	return vs;
}

/* ========== InstanceVisualSettings ========== */

obs_data_t *InstanceVisualSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();

	obs_data_set_string(data, "backgroundMode", inheritance_mode_to_str(backgroundMode));
	obs_data_t *bg = background.to_obs_data();
	obs_data_set_obj(data, "background", bg);
	obs_data_release(bg);

	obs_data_set_string(data, "labelMode", inheritance_mode_to_str(labelMode));
	obs_data_t *lbl = label.to_obs_data();
	obs_data_set_obj(data, "label", lbl);
	obs_data_release(lbl);

	obs_data_set_string(data, "safeAreaMode", inheritance_mode_to_str(safeAreaMode));
	obs_data_t *sa = safeArea.to_obs_data();
	obs_data_set_obj(data, "safeArea", sa);
	obs_data_release(sa);

	obs_data_set_string(data, "vuMeterMode", inheritance_mode_to_str(vuMeterMode));
	obs_data_t *vu = vuMeter.to_obs_data();
	obs_data_set_obj(data, "vuMeter", vu);
	obs_data_release(vu);

	obs_data_set_string(data, "overlayMode", inheritance_mode_to_str(overlayMode));
	obs_data_t *ov = overlay.to_obs_data();
	obs_data_set_obj(data, "overlay", ov);
	obs_data_release(ov);

	obs_data_set_string(data, "highlightMode", inheritance_mode_to_str(highlightMode));
	obs_data_t *hl = highlight.to_obs_data();
	obs_data_set_obj(data, "highlight", hl);
	obs_data_release(hl);

	return data;
}

InstanceVisualSettings InstanceVisualSettings::from_obs_data(obs_data_t *data)
{
	InstanceVisualSettings vs;
	if (!data)
		return vs;

	vs.backgroundMode = inheritance_mode_from_str(obs_data_get_string(data, "backgroundMode"));
	obs_data_t *bg = obs_data_get_obj(data, "background");
	vs.background = BackgroundSettings::from_obs_data(bg);
	if (bg)
		obs_data_release(bg);

	vs.labelMode = inheritance_mode_from_str(obs_data_get_string(data, "labelMode"));
	obs_data_t *lbl = obs_data_get_obj(data, "label");
	vs.label = LabelSettings::from_obs_data(lbl);
	if (lbl)
		obs_data_release(lbl);

	vs.safeAreaMode = inheritance_mode_from_str(obs_data_get_string(data, "safeAreaMode"));
	obs_data_t *sa = obs_data_get_obj(data, "safeArea");
	vs.safeArea = SafeAreaSettings::from_obs_data(sa);
	if (sa)
		obs_data_release(sa);

	vs.vuMeterMode = inheritance_mode_from_str(obs_data_get_string(data, "vuMeterMode"));
	obs_data_t *vu = obs_data_get_obj(data, "vuMeter");
	vs.vuMeter = VuMeterSettings::from_obs_data(vu);
	if (vu)
		obs_data_release(vu);

	vs.overlayMode = inheritance_mode_from_str(obs_data_get_string(data, "overlayMode"));
	obs_data_t *ov = obs_data_get_obj(data, "overlay");
	vs.overlay = OverlaySettings::from_obs_data(ov);
	if (ov)
		obs_data_release(ov);

	vs.highlightMode = inheritance_mode_from_str(obs_data_get_string(data, "highlightMode"));
	obs_data_t *hl = obs_data_get_obj(data, "highlight");
	vs.highlight = HighlightSettings::from_obs_data(hl);
	if (hl)
		obs_data_release(hl);

	return vs;
}

/* ========== CellVisualSettings ========== */

obs_data_t *CellVisualSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "row", row);
	obs_data_set_int(data, "col", col);

	obs_data_set_string(data, "backgroundMode", inheritance_mode_to_str(backgroundMode));
	obs_data_t *bg = background.to_obs_data();
	obs_data_set_obj(data, "background", bg);
	obs_data_release(bg);

	obs_data_set_string(data, "labelMode", inheritance_mode_to_str(labelMode));
	obs_data_t *lbl = label.to_obs_data();
	obs_data_set_obj(data, "label", lbl);
	obs_data_release(lbl);

	obs_data_set_string(data, "safeAreaMode", inheritance_mode_to_str(safeAreaMode));
	obs_data_t *sa = safeArea.to_obs_data();
	obs_data_set_obj(data, "safeArea", sa);
	obs_data_release(sa);

	obs_data_set_string(data, "vuMeterMode", inheritance_mode_to_str(vuMeterMode));
	obs_data_t *vu = vuMeter.to_obs_data();
	obs_data_set_obj(data, "vuMeter", vu);
	obs_data_release(vu);

	obs_data_set_string(data, "overlayMode", inheritance_mode_to_str(overlayMode));
	obs_data_t *ov = overlay.to_obs_data();
	obs_data_set_obj(data, "overlay", ov);
	obs_data_release(ov);

	return data;
}

CellVisualSettings CellVisualSettings::from_obs_data(obs_data_t *data)
{
	CellVisualSettings vs;
	if (!data)
		return vs;

	vs.row = (int)obs_data_get_int(data, "row");
	vs.col = (int)obs_data_get_int(data, "col");

	vs.backgroundMode = inheritance_mode_from_str(obs_data_get_string(data, "backgroundMode"));
	obs_data_t *bg = obs_data_get_obj(data, "background");
	vs.background = BackgroundSettings::from_obs_data(bg);
	if (bg)
		obs_data_release(bg);

	vs.labelMode = inheritance_mode_from_str(obs_data_get_string(data, "labelMode"));
	obs_data_t *lbl = obs_data_get_obj(data, "label");
	vs.label = LabelSettings::from_obs_data(lbl);
	if (lbl)
		obs_data_release(lbl);

	vs.safeAreaMode = inheritance_mode_from_str(obs_data_get_string(data, "safeAreaMode"));
	obs_data_t *sa = obs_data_get_obj(data, "safeArea");
	vs.safeArea = SafeAreaSettings::from_obs_data(sa);
	if (sa)
		obs_data_release(sa);

	vs.vuMeterMode = inheritance_mode_from_str(obs_data_get_string(data, "vuMeterMode"));
	obs_data_t *vu = obs_data_get_obj(data, "vuMeter");
	vs.vuMeter = VuMeterSettings::from_obs_data(vu);
	if (vu)
		obs_data_release(vu);

	vs.overlayMode = inheritance_mode_from_str(obs_data_get_string(data, "overlayMode"));
	obs_data_t *ov = obs_data_get_obj(data, "overlay");
	vs.overlay = OverlaySettings::from_obs_data(ov);
	if (ov)
		obs_data_release(ov);

	return vs;
}

/* ========== resolve_effective_visual_settings ========== */

EffectiveCellVisualSettings resolve_effective_visual_settings(const GlobalVisualSettings &global,
							      const InstanceVisualSettings &instance,
							      const CellVisualSettings *cell)
{
	EffectiveCellVisualSettings eff;

	/* Background */
	if (cell && cell->backgroundMode == InheritanceMode::Override)
		eff.background = cell->background;
	else if (instance.backgroundMode == InheritanceMode::Override)
		eff.background = instance.background;
	else
		eff.background = global.background;

	/* Label */
	if (cell && cell->labelMode == InheritanceMode::Override)
		eff.label = cell->label;
	else if (instance.labelMode == InheritanceMode::Override)
		eff.label = instance.label;
	else
		eff.label = global.label;

	/* Safe Area */
	if (cell && cell->safeAreaMode == InheritanceMode::Override)
		eff.safeArea = cell->safeArea;
	else if (instance.safeAreaMode == InheritanceMode::Override)
		eff.safeArea = instance.safeArea;
	else
		eff.safeArea = global.safeArea;

	/* VU Meter */
	if (cell && cell->vuMeterMode == InheritanceMode::Override)
		eff.vuMeter = cell->vuMeter;
	else if (instance.vuMeterMode == InheritanceMode::Override)
		eff.vuMeter = instance.vuMeter;
	else
		eff.vuMeter = global.vuMeter;

	/* Overlay */
	if (cell && cell->overlayMode == InheritanceMode::Override)
		eff.overlay = cell->overlay;
	else if (instance.overlayMode == InheritanceMode::Override)
		eff.overlay = instance.overlay;
	else
		eff.overlay = global.overlay;

	/* Highlight
	 *
	 * Per-cell override intentionally not supported — highlight is a
	 * window-wide concept driven by PGM/PRVW scene-tree containment. The
	 * Cell Display Settings dialog disables this group entirely. Resolution
	 * is thus a 2-step chain: instance (if override) -> global. */
	if (instance.highlightMode == InheritanceMode::Override)
		eff.highlight = instance.highlight;
	else
		eff.highlight = global.highlight;

	return eff;
}

/* ---------- SignalProviderType ---------- */

const char *signal_provider_to_string(SignalProviderType p)
{
	switch (p) {
	case SignalProviderType::InternalPgm:
		return "internal_pgm";
	case SignalProviderType::InternalPrvw:
		return "internal_prvw";
	case SignalProviderType::InternalScene:
		return "internal_scene";
	case SignalProviderType::InternalSource:
		return "internal_source";
	case SignalProviderType::Ffmpeg:
		return "ffmpeg";
	case SignalProviderType::Ndi:
		return "ndi";
	case SignalProviderType::Spout:
		return "spout";
	case SignalProviderType::Vlc:
		return "vlc";
	case SignalProviderType::WebRtcReserved:
		return "webrtc_reserved";
	case SignalProviderType::Unknown:
	default:
		return "unknown";
	}
}

SignalProviderType signal_provider_from_string(const char *s)
{
	if (!s || !*s)
		return SignalProviderType::Unknown;
	if (strcmp(s, "internal_pgm") == 0)
		return SignalProviderType::InternalPgm;
	if (strcmp(s, "internal_prvw") == 0)
		return SignalProviderType::InternalPrvw;
	if (strcmp(s, "internal_scene") == 0)
		return SignalProviderType::InternalScene;
	if (strcmp(s, "internal_source") == 0)
		return SignalProviderType::InternalSource;
	if (strcmp(s, "ffmpeg") == 0)
		return SignalProviderType::Ffmpeg;
	if (strcmp(s, "ndi") == 0)
		return SignalProviderType::Ndi;
	if (strcmp(s, "spout") == 0)
		return SignalProviderType::Spout;
	if (strcmp(s, "vlc") == 0)
		return SignalProviderType::Vlc;
	if (strcmp(s, "webrtc_reserved") == 0)
		return SignalProviderType::WebRtcReserved;
	return SignalProviderType::Unknown;
}

bool signal_provider_is_internal(SignalProviderType p)
{
	switch (p) {
	case SignalProviderType::InternalPgm:
	case SignalProviderType::InternalPrvw:
	case SignalProviderType::InternalScene:
	case SignalProviderType::InternalSource:
		return true;
	default:
		return false;
	}
}

bool signal_provider_supported_on_platform(SignalProviderType p)
{
	switch (p) {
	case SignalProviderType::Spout:
		/* obs-spout2 is built on Windows-only DirectX shared
		 * textures and Off-World-Live ships no macOS / Linux
		 * build. Hard-gate on _WIN32 so non-Windows OBS never
		 * tries to construct a spout_capture private source. */
#if defined(_WIN32)
		return true;
#else
		return false;
#endif
	default:
		/* Internal providers, FFmpeg (built into libobs), NDI
		 * (DistroAV ships universal), VLC (vlc-video bundled with
		 * official builds), and the reserved WebRTC slot are all
		 * cross-platform. */
		return true;
	}
}

const char *signal_provider_unsupported_platform_reason(SignalProviderType p)
{
	if (signal_provider_supported_on_platform(p))
		return "";
	switch (p) {
	case SignalProviderType::Spout:
		return "Spout is a Windows-only protocol (DirectX shared textures); obs-spout2 has no macOS or Linux build.";
	default:
		return "This provider is not supported on the current platform.";
	}
}

/* ---------- SignalConfig ---------- */

/* OBS does not expose a deep-copy API for obs_data_t, so we round-trip
 * through JSON. This is the same approach used elsewhere in OBS plugins
 * for cloning settings objects and is safe for the simple key/value
 * shapes we expect from provider settings. */
static obs_data_t *clone_obs_data(obs_data_t *src)
{
	if (!src)
		return nullptr;
	const char *json = obs_data_get_json(src);
	if (!json || !*json)
		return obs_data_create();
	return obs_data_create_from_json(json);
}

SignalConfig::SignalConfig(const SignalConfig &other)
	: provider(other.provider),
	  displayName(other.displayName),
	  providerSettings(clone_obs_data(other.providerSettings))
{
}

SignalConfig::SignalConfig(SignalConfig &&other) noexcept
	: provider(other.provider),
	  displayName(std::move(other.displayName)),
	  providerSettings(other.providerSettings)
{
	other.provider = SignalProviderType::Unknown;
	other.providerSettings = nullptr;
}

SignalConfig &SignalConfig::operator=(const SignalConfig &other)
{
	if (this == &other)
		return *this;
	if (providerSettings)
		obs_data_release(providerSettings);
	provider = other.provider;
	displayName = other.displayName;
	providerSettings = clone_obs_data(other.providerSettings);
	return *this;
}

SignalConfig &SignalConfig::operator=(SignalConfig &&other) noexcept
{
	if (this == &other)
		return *this;
	if (providerSettings)
		obs_data_release(providerSettings);
	provider = other.provider;
	displayName = std::move(other.displayName);
	providerSettings = other.providerSettings;
	other.provider = SignalProviderType::Unknown;
	other.providerSettings = nullptr;
	return *this;
}

SignalConfig::~SignalConfig()
{
	if (providerSettings) {
		obs_data_release(providerSettings);
		providerSettings = nullptr;
	}
}

obs_data_t *SignalConfig::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "provider", signal_provider_to_string(provider));
	obs_data_set_string(data, "displayName", displayName.c_str());
	if (providerSettings) {
		obs_data_t *snap = clone_obs_data(providerSettings);
		if (snap) {
			obs_data_set_obj(data, "settings", snap);
			obs_data_release(snap);
		}
	}
	return data;
}

SignalConfig SignalConfig::from_obs_data(obs_data_t *data)
{
	SignalConfig cfg;
	if (!data)
		return cfg;
	cfg.provider = signal_provider_from_string(obs_data_get_string(data, "provider"));
	cfg.displayName = obs_data_get_string(data, "displayName");
	if (obs_data_has_user_value(data, "settings")) {
		obs_data_t *inner = obs_data_get_obj(data, "settings");
		if (inner) {
			cfg.providerSettings = clone_obs_data(inner);
			obs_data_release(inner);
		}
	}
	return cfg;
}

/* ---------- CellAssignment ---------- */

obs_data_t *CellAssignment::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "row", row);
	obs_data_set_int(data, "col", col);
	obs_data_set_string(data, "type", type.c_str());
	obs_data_set_string(data, "name", name.c_str());

	/* Phase 3 / M6: only persist signalConfig for non-empty external
	 * cells, plus the forward-compat case where an unknown provider type
	 * was loaded with settings we don't yet understand (we keep the raw
	 * payload so a future build can resume it). Internal cells stay
	 * byte-compatible with M5 v3 configs — no extra keys, no empty
	 * objects. */
	const bool persist_signal_config =
		signalConfig.is_external() ||
		(signalConfig.provider == SignalProviderType::Unknown && signalConfig.providerSettings != nullptr);
	if (persist_signal_config) {
		obs_data_t *cfg = signalConfig.to_obs_data();
		if (cfg) {
			obs_data_set_obj(data, "signalConfig", cfg);
			obs_data_release(cfg);
		}
	}
	return data;
}

CellAssignment CellAssignment::from_obs_data(obs_data_t *data)
{
	CellAssignment ca;
	ca.row = (int)obs_data_get_int(data, "row");
	ca.col = (int)obs_data_get_int(data, "col");
	/* Legacy data without row/col will read as 0,0 - we use a sentinel
	 * to detect this case: if "row" key doesn't exist, mark as -1 */
	if (!obs_data_has_user_value(data, "row"))
		ca.row = -1;
	if (!obs_data_has_user_value(data, "col"))
		ca.col = -1;
	ca.type = obs_data_get_string(data, "type");
	ca.name = obs_data_get_string(data, "name");

	if (obs_data_has_user_value(data, "signalConfig")) {
		obs_data_t *cfg = obs_data_get_obj(data, "signalConfig");
		ca.signalConfig = SignalConfig::from_obs_data(cfg);
		obs_data_release(cfg);
	}
	return ca;
}

/* ---------- SpanRegion ---------- */

obs_data_t *SpanRegion::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "row", row);
	obs_data_set_int(data, "col", col);
	obs_data_set_int(data, "rowSpan", rowSpan);
	obs_data_set_int(data, "colSpan", colSpan);
	return data;
}

SpanRegion SpanRegion::from_obs_data(obs_data_t *data)
{
	SpanRegion s;
	s.row = (int)obs_data_get_int(data, "row");
	s.col = (int)obs_data_get_int(data, "col");
	s.rowSpan = (int)obs_data_get_int(data, "rowSpan");
	s.colSpan = (int)obs_data_get_int(data, "colSpan");
	if (s.rowSpan < 1)
		s.rowSpan = 1;
	if (s.colSpan < 1)
		s.colSpan = 1;
	return s;
}

/* ---------- LayoutData ---------- */

obs_data_t *LayoutData::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "rows", rows);
	obs_data_set_int(data, "columns", columns);
	obs_data_set_int(data, "gutterPx", gutterPx);

	obs_data_array_t *arr = obs_data_array_create();
	for (auto &s : spans) {
		obs_data_t *item = s.to_obs_data();
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "spans", arr);
	obs_data_array_release(arr);

	return data;
}

LayoutData LayoutData::from_obs_data(obs_data_t *data)
{
	LayoutData ld;
	ld.rows = (int)obs_data_get_int(data, "rows");
	ld.columns = (int)obs_data_get_int(data, "columns");
	ld.gutterPx = (int)obs_data_get_int(data, "gutterPx");

	if (ld.rows < 1)
		ld.rows = 4;
	if (ld.rows > 10)
		ld.rows = 10;
	if (ld.columns < 1)
		ld.columns = 4;
	if (ld.columns > 10)
		ld.columns = 10;
	if (ld.gutterPx < 0)
		ld.gutterPx = 0;
	if (ld.gutterPx > 50)
		ld.gutterPx = 50;

	obs_data_array_t *arr = obs_data_get_array(data, "spans");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			ld.spans.push_back(SpanRegion::from_obs_data(item));
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	return ld;
}

/* ---------- MultiviewInstance ---------- */

obs_data_t *MultiviewInstance::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "uuid", uuid.c_str());
	obs_data_set_string(data, "name", name.c_str());
	if (!folder.empty())
		obs_data_set_string(data, "folder", folder.c_str());
	obs_data_set_bool(data, "useGlobalGutter", useGlobalGutter);
	obs_data_set_bool(data, "useGlobalSceneClickSwitch", useGlobalSceneClickSwitch);

	obs_data_t *scs = sceneClickSwitch.to_obs_data();
	obs_data_set_obj(data, "sceneClickSwitch", scs);
	obs_data_release(scs);

	obs_data_t *layoutData = layout.to_obs_data();
	obs_data_set_obj(data, "layout", layoutData);
	obs_data_release(layoutData);

	obs_data_array_t *arr = obs_data_array_create();
	for (auto &ca : cellAssignments) {
		/* Skip empty slots. Phase 3 / M6.1: external-provider cells
		 * leave `type` empty and carry their binding in `signalConfig`,
		 * so we must persist either an internal type or a non-empty
		 * signalConfig. The previous M5-only check dropped every
		 * Media / NDI / Spout assignment on save \u2014 next OBS launch
		 * loaded an empty cellAssignments list and the cell appeared
		 * cleared. */
		if (ca.type.empty() && ca.signalConfig.empty())
			continue;
		obs_data_t *item = ca.to_obs_data();
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "cellAssignments", arr);
	obs_data_array_release(arr);

	/* Instance visual settings */
	obs_data_t *vs = visualSettings.to_obs_data();
	obs_data_set_obj(data, "visualSettings", vs);
	obs_data_release(vs);

	/* Per-cell visual settings (only save non-all-inherit entries) */
	obs_data_array_t *cvs_arr = obs_data_array_create();
	for (auto &cvs : cellVisualSettings) {
		/* Only persist cells that have at least one Override */
		if (cvs.backgroundMode == InheritanceMode::Inherit && cvs.labelMode == InheritanceMode::Inherit &&
		    cvs.safeAreaMode == InheritanceMode::Inherit && cvs.vuMeterMode == InheritanceMode::Inherit &&
		    cvs.overlayMode == InheritanceMode::Inherit)
			continue;
		obs_data_t *item = cvs.to_obs_data();
		obs_data_array_push_back(cvs_arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "cellVisualSettings", cvs_arr);
	obs_data_array_release(cvs_arr);

	/* Phase 3 / M5: per-cell Lost Signal overrides. Mirror the cellVisualSettings
	 * sparse-persistence rule — only cells with mode == Override hit disk so
	 * default-everywhere instances keep a clean JSON shape. */
	obs_data_array_t *cls_arr = obs_data_array_create();
	for (auto &cls : cellLostSignalSettings) {
		if (cls.mode != InheritanceMode::Override)
			continue;
		obs_data_t *item = cls.to_obs_data();
		obs_data_array_push_back(cls_arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "cellLostSignalSettings", cls_arr);
	obs_data_array_release(cls_arr);

	return data;
}

MultiviewInstance MultiviewInstance::from_obs_data(obs_data_t *data)
{
	MultiviewInstance inst;
	inst.uuid = obs_data_get_string(data, "uuid");
	inst.name = obs_data_get_string(data, "name");
	inst.folder = obs_data_get_string(data, "folder");

	if (obs_data_has_user_value(data, "useGlobalGutter"))
		inst.useGlobalGutter = obs_data_get_bool(data, "useGlobalGutter");
	else
		inst.useGlobalGutter = true;

	/* Scene-click switching: default to inherit when key absent on legacy
	 * configs so existing instances pick up the global default. */
	if (obs_data_has_user_value(data, "useGlobalSceneClickSwitch"))
		inst.useGlobalSceneClickSwitch = obs_data_get_bool(data, "useGlobalSceneClickSwitch");
	else
		inst.useGlobalSceneClickSwitch = true;

	obs_data_t *scs = obs_data_get_obj(data, "sceneClickSwitch");
	inst.sceneClickSwitch = SceneClickSwitchSettings::from_obs_data(scs);
	if (scs)
		obs_data_release(scs);

	obs_data_t *layoutData = obs_data_get_obj(data, "layout");
	if (layoutData) {
		inst.layout = LayoutData::from_obs_data(layoutData);
		obs_data_release(layoutData);
	}

	obs_data_array_t *arr = obs_data_get_array(data, "cellAssignments");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			CellAssignment ca = CellAssignment::from_obs_data(item);
			obs_data_release(item);
			/* Skip empty assignments. Phase 3 / M6.1: external-provider
			 * cells leave `type` empty (legacy field is internal-only)
			 * and carry their binding in `signalConfig` instead, so we
			 * must accept either an internal type or a non-empty
			 * signalConfig as "this slot is occupied". Without this,
			 * persisted Media / NDI / Spout cells silently disappear
			 * after an OBS restart. */
			if (ca.type.empty() && ca.signalConfig.empty())
				continue;
			/* Legacy migration: if row/col not stored, compute from flat index */
			if (ca.row < 0 || ca.col < 0) {
				int cols = inst.layout.columns;
				if (cols < 1)
					cols = 4;
				ca.row = (int)i / cols;
				ca.col = (int)i % cols;
			}
			inst.cellAssignments.push_back(ca);
		}
		obs_data_array_release(arr);
	}

	/* Instance visual settings */
	obs_data_t *vs = obs_data_get_obj(data, "visualSettings");
	inst.visualSettings = InstanceVisualSettings::from_obs_data(vs);
	if (vs)
		obs_data_release(vs);

	/* Per-cell visual settings */
	obs_data_array_t *cvs_arr = obs_data_get_array(data, "cellVisualSettings");
	if (cvs_arr) {
		size_t count = obs_data_array_count(cvs_arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(cvs_arr, i);
			inst.cellVisualSettings.push_back(CellVisualSettings::from_obs_data(item));
			obs_data_release(item);
		}
		obs_data_array_release(cvs_arr);
	}

	/* Phase 3 / M5: per-cell Lost Signal overrides. Absent in v1/v2 configs;
	 * resolver falls back to global default when the override list is empty. */
	obs_data_array_t *cls_arr = obs_data_get_array(data, "cellLostSignalSettings");
	if (cls_arr) {
		size_t count = obs_data_array_count(cls_arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(cls_arr, i);
			inst.cellLostSignalSettings.push_back(CellLostSignalSettings::from_obs_data(item));
			obs_data_release(item);
		}
		obs_data_array_release(cls_arr);
	}

	return inst;
}

MultiviewInstance MultiviewInstance::create_new(const std::string &instanceName)
{
	MultiviewInstance inst;
	inst.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	inst.name = instanceName;
	inst.useGlobalGutter = true;
	return inst;
}

int MultiviewInstance::effective_gutter(int globalGutter) const
{
	return useGlobalGutter ? globalGutter : layout.gutterPx;
}

SceneClickSwitchSettings
MultiviewInstance::effective_scene_click_switch(const SceneClickSwitchSettings &globalDefault) const
{
	return useGlobalSceneClickSwitch ? globalDefault : sceneClickSwitch;
}

MultiviewInstance MultiviewInstance::clone_instance(const std::string &newName) const
{
	MultiviewInstance cloned = *this;
	cloned.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
	cloned.name = newName;
	cloned.layoutDirty = false;
	cloned.signalDirty = false;
	return cloned;
}

const CellVisualSettings *MultiviewInstance::find_cell_visual(int row, int col) const
{
	for (auto &cvs : cellVisualSettings) {
		if (cvs.row == row && cvs.col == col)
			return &cvs;
	}
	return nullptr;
}

const CellLostSignalSettings *MultiviewInstance::find_cell_lost_signal(int row, int col) const
{
	for (auto &cls : cellLostSignalSettings) {
		if (cls.row == row && cls.col == col)
			return &cls;
	}
	return nullptr;
}

/* ---------- LayoutPreset ---------- */

obs_data_t *LayoutPreset::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "uuid", uuid.c_str());
	obs_data_set_string(data, "name", name.c_str());

	obs_data_t *layoutData = layout.to_obs_data();
	obs_data_set_obj(data, "layout", layoutData);
	obs_data_release(layoutData);

	return data;
}

LayoutPreset LayoutPreset::from_obs_data(obs_data_t *data)
{
	LayoutPreset lp;
	lp.uuid = obs_data_get_string(data, "uuid");
	lp.name = obs_data_get_string(data, "name");

	obs_data_t *layoutData = obs_data_get_obj(data, "layout");
	if (layoutData) {
		lp.layout = LayoutData::from_obs_data(layoutData);
		obs_data_release(layoutData);
	}

	return lp;
}

/* ---------- GlobalSettings ---------- */

obs_data_t *GlobalSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "defaultGutterPx", defaultGutterPx);
	obs_data_set_bool(data, "reResolveInheritObs", reResolveInheritObs);
	obs_data_set_double(data, "reResolveCustomFps", reResolveCustomFps);

	obs_data_t *vs = visualSettings.to_obs_data();
	obs_data_set_obj(data, "visualSettings", vs);
	obs_data_release(vs);

	/* Phase 3 / M5: project-wide Lost Signal default. */
	obs_data_t *ls = lostSignal.to_obs_data();
	obs_data_set_obj(data, "lostSignal", ls);
	obs_data_release(ls);

	/* Project-wide scene-click switching default. */
	obs_data_t *scs = sceneClickSwitch.to_obs_data();
	obs_data_set_obj(data, "sceneClickSwitch", scs);
	obs_data_release(scs);

	/* Phase 3 hardening tail: persist the Detailed logs toggle so a
	 * fresh OBS startup respects whatever the user picked last session. */
	obs_data_set_bool(data, "detailedLogs", detailedLogs);

	return data;
}

GlobalSettings GlobalSettings::from_obs_data(obs_data_t *data)
{
	GlobalSettings gs;
	if (!data)
		return gs;
	gs.defaultGutterPx = (int)obs_data_get_int(data, "defaultGutterPx");
	if (!obs_data_has_user_value(data, "defaultGutterPx"))
		gs.defaultGutterPx = 7;
	if (gs.defaultGutterPx < 0)
		gs.defaultGutterPx = 0;
	if (gs.defaultGutterPx > 50)
		gs.defaultGutterPx = 50;

	gs.reResolveInheritObs = obs_data_get_bool(data, "reResolveInheritObs");
	/* Default to true if key absent (new config) */
	if (!obs_data_has_user_value(data, "reResolveInheritObs"))
		gs.reResolveInheritObs = true;

	gs.reResolveCustomFps = obs_data_get_double(data, "reResolveCustomFps");
	if (gs.reResolveCustomFps < 1.0)
		gs.reResolveCustomFps = 30.0;
	if (gs.reResolveCustomFps > 120.0)
		gs.reResolveCustomFps = 120.0;

	obs_data_t *vs = obs_data_get_obj(data, "visualSettings");
	gs.visualSettings = GlobalVisualSettings::from_obs_data(vs);
	if (vs)
		obs_data_release(vs);

	/* Phase 3 / M5: Absent on v1/v2 configs — from_obs_data(nullptr) returns
	 * struct defaults, matching the documented project-wide defaults. */
	obs_data_t *ls = obs_data_get_obj(data, "lostSignal");
	gs.lostSignal = LostSignalSettings::from_obs_data(ls);
	if (ls)
		obs_data_release(ls);

	/* Scene-click switching default: absent key resolves to enabled=true so
	 * upgrades from older configs match the OBS built-in default. */
	obs_data_t *scs = obs_data_get_obj(data, "sceneClickSwitch");
	gs.sceneClickSwitch = SceneClickSwitchSettings::from_obs_data(scs);
	if (scs)
		obs_data_release(scs);

	/* Phase 3 hardening tail: Detailed logs toggle. Default false when
	 * the key is absent so existing configs do not silently flip on. */
	if (obs_data_has_user_value(data, "detailedLogs"))
		gs.detailedLogs = obs_data_get_bool(data, "detailedLogs");
	else
		gs.detailedLogs = false;

	return gs;
}
