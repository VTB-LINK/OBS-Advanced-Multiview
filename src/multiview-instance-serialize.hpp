#pragma once

/*
OBS Advanced Multiview - enum<->string serialization helpers shared across the
multiview-instance-serialize-*.cpp split (issue #10). Several enums (InheritanceMode,
ImageFitMode, ...) are used by both the visual and signal serialization, so the
mappers live here as inline rather than duplicated.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-instance.hpp"

#include <cstring>

inline const char *inheritance_mode_to_str(InheritanceMode m)
{
	switch (m) {
	case InheritanceMode::Override:
		return "override";
	default:
		return "inherit";
	}
}

inline InheritanceMode inheritance_mode_from_str(const char *s)
{
	if (s && strcmp(s, "override") == 0)
		return InheritanceMode::Override;
	return InheritanceMode::Inherit;
}

/* Phase 3 / M5: Lost Signal enum string mapping. Strings instead of ints so
 * future additions (e.g. ColorBars, PreviousFrameFreeze) won't shift the
 * numeric meaning of older configs. */
inline const char *internal_missing_behavior_to_str(InternalMissingBehavior b)
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

inline InternalMissingBehavior internal_missing_behavior_from_str(const char *s)
{
	if (!s)
		return InternalMissingBehavior::Black;
	if (strcmp(s, "placeholder_image") == 0)
		return InternalMissingBehavior::PlaceholderImage;
	if (strcmp(s, "clear_cell") == 0)
		return InternalMissingBehavior::ClearCell;
	return InternalMissingBehavior::Black;
}

inline const char *external_lost_behavior_to_str(ExternalLostBehavior b)
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

inline ExternalLostBehavior external_lost_behavior_from_str(const char *s)
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

inline const char *label_display_mode_to_str(LabelDisplayMode m)
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

inline LabelDisplayMode label_display_mode_from_str(const char *s)
{
	if (!s)
		return LabelDisplayMode::None;
	if (strcmp(s, "overlay") == 0)
		return LabelDisplayMode::Overlay;
	if (strcmp(s, "below") == 0)
		return LabelDisplayMode::Below;
	return LabelDisplayMode::None;
}

inline const char *label_position_to_str(LabelPosition p)
{
	switch (p) {
	case LabelPosition::Top:
		return "top";
	default:
		return "bottom";
	}
}

inline LabelPosition label_position_from_str(const char *s)
{
	if (s && strcmp(s, "top") == 0)
		return LabelPosition::Top;
	return LabelPosition::Bottom;
}

inline const char *font_scale_mode_to_str(FontScaleMode m)
{
	switch (m) {
	case FontScaleMode::ScaleWithCell:
		return "scaleWithCell";
	default:
		return "fixed";
	}
}

inline FontScaleMode font_scale_mode_from_str(const char *s)
{
	if (s && strcmp(s, "scaleWithCell") == 0)
		return FontScaleMode::ScaleWithCell;
	return FontScaleMode::Fixed;
}

inline const char *image_fit_mode_to_str(ImageFitMode m)
{
	switch (m) {
	case ImageFitMode::Stretch:
		return "stretch";
	default:
		return "fit";
	}
}

inline ImageFitMode image_fit_mode_from_str(const char *s)
{
	if (s && strcmp(s, "stretch") == 0)
		return ImageFitMode::Stretch;
	return ImageFitMode::Fit;
}

inline const char *bg_fill_mode_to_str(BackgroundFillMode m)
{
	switch (m) {
	case BackgroundFillMode::FillEntireCell:
		return "cell";
	default:
		return "signal";
	}
}

inline BackgroundFillMode bg_fill_mode_from_str(const char *s)
{
	if (s && strcmp(s, "cell") == 0)
		return BackgroundFillMode::FillEntireCell;
	return BackgroundFillMode::FillSignalOnly;
}

inline const char *safe_area_preset_to_str(SafeAreaPreset p)
{
	(void)p;
	return "ebu_r95";
}

inline SafeAreaPreset safe_area_preset_from_str(const char *s)
{
	(void)s;
	return SafeAreaPreset::EBU_R95;
}

inline const char *vu_meter_position_to_str(VuMeterPosition p)
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

inline VuMeterPosition vu_meter_position_from_str(const char *s)
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

inline const char *vu_meter_anchor_to_str(VuMeterAnchorMode a)
{
	switch (a) {
	case VuMeterAnchorMode::Signal:
		return "signal";
	default:
		return "cell";
	}
}

inline VuMeterAnchorMode vu_meter_anchor_from_str(const char *s)
{
	if (!s)
		return VuMeterAnchorMode::Cell;
	if (strcmp(s, "signal") == 0)
		return VuMeterAnchorMode::Signal;
	return VuMeterAnchorMode::Cell;
}

inline const char *vu_meter_decay_to_str(VuMeterDecayRate r)
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

inline VuMeterDecayRate vu_meter_decay_from_str(const char *s)
{
	if (!s)
		return VuMeterDecayRate::Fast;
	if (strcmp(s, "medium") == 0)
		return VuMeterDecayRate::Medium;
	if (strcmp(s, "slow") == 0)
		return VuMeterDecayRate::Slow;
	return VuMeterDecayRate::Fast;
}

inline const char *vu_meter_alignment_to_str(VuMeterAlignment a)
{
	switch (a) {
	case VuMeterAlignment::Start:
		return "start";
	default:
		return "center";
	}
}

inline VuMeterAlignment vu_meter_alignment_from_str(const char *s)
{
	if (!s)
		return VuMeterAlignment::Center;
	if (strcmp(s, "start") == 0)
		return VuMeterAlignment::Start;
	return VuMeterAlignment::Center;
}

inline const char *vu_meter_track_mode_to_str(VuMeterTrackMode m)
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

inline VuMeterTrackMode vu_meter_track_mode_from_str(const char *s)
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

inline const char *vu_meter_scale_side_to_str(VuMeterScaleSide side)
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

inline VuMeterScaleSide vu_meter_scale_side_from_str(const char *s)
{
	if (!s)
		return VuMeterScaleSide::Auto;
	if (strcmp(s, "same") == 0)
		return VuMeterScaleSide::Same;
	if (strcmp(s, "opposite") == 0)
		return VuMeterScaleSide::Opposite;
	return VuMeterScaleSide::Auto;
}

inline const char *vu_meter_style_to_str(VuMeterStyle st)
{
	(void)st;
	return "bar";
}

inline VuMeterStyle vu_meter_style_from_str(const char *s)
{
	(void)s;
	return VuMeterStyle::Bar;
}

inline const char *overlay_fit_mode_to_str(OverlayFitMode m)
{
	switch (m) {
	case OverlayFitMode::Stretch:
		return "stretch";
	default:
		return "fit";
	}
}

inline OverlayFitMode overlay_fit_mode_from_str(const char *s)
{
	if (s && strcmp(s, "stretch") == 0)
		return OverlayFitMode::Stretch;
	return OverlayFitMode::Fit;
}

inline const char *overlay_anchor_mode_to_str(OverlayAnchorMode m)
{
	switch (m) {
	case OverlayAnchorMode::Cell:
		return "cell";
	default:
		return "signal";
	}
}

inline OverlayAnchorMode overlay_anchor_mode_from_str(const char *s)
{
	if (s && strcmp(s, "cell") == 0)
		return OverlayAnchorMode::Cell;
	return OverlayAnchorMode::Signal;
}

inline const char *safe_area_anchor_mode_to_str(SafeAreaAnchorMode m)
{
	switch (m) {
	case SafeAreaAnchorMode::Cell:
		return "cell";
	default:
		return "signal";
	}
}

inline SafeAreaAnchorMode safe_area_anchor_mode_from_str(const char *s)
{
	if (s && strcmp(s, "cell") == 0)
		return SafeAreaAnchorMode::Cell;
	if (s && strcmp(s, "signal") == 0)
		return SafeAreaAnchorMode::Signal;
	return SafeAreaAnchorMode::Cell;
}
