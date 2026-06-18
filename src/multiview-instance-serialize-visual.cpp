/*
OBS Advanced Multiview - visual settings serialization (issue #10 split)

Background/Label/SafeArea/VuMeter/Overlay/Highlight + Global/Instance/Cell visual
settings (de)serialization and resolve_effective_visual_settings. Struct
declarations live in multiview-instance.hpp; enum<->string helpers in
multiview-instance-serialize.hpp.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-instance.hpp"
#include "multiview-instance-serialize.hpp"

#include <obs.h>
#include <obs-data.h>

#include <cstring>

/* ========== BackgroundSettings ========== */

obs_data_t *BackgroundSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();

	obs_data_t *fill = obs_data_create();
	obs_data_set_bool(fill, "colorEnabled", colorEnabled);
	obs_data_set_int(fill, "color", (long long)color);
	obs_data_set_string(fill, "mode", bg_fill_mode_to_str(fillMode));
	obs_data_set_obj(data, "fill", fill);
	obs_data_release(fill);

	obs_data_t *image = obs_data_create();
	obs_data_set_bool(image, "enabled", imageEnabled);
	obs_data_set_string(image, "path", imagePath.c_str());
	obs_data_set_string(image, "fit", image_fit_mode_to_str(imageFitMode));
	obs_data_set_obj(data, "image", image);
	obs_data_release(image);

	return data;
}

BackgroundSettings BackgroundSettings::from_obs_data(obs_data_t *data)
{
	BackgroundSettings s;
	if (!data)
		return s;

	obs_data_t *fill = obs_data_get_obj(data, "fill");
	if (fill) {
		if (obs_data_has_user_value(fill, "colorEnabled"))
			s.colorEnabled = obs_data_get_bool(fill, "colorEnabled");
		s.color = (uint32_t)obs_data_get_int(fill, "color");
		if (!obs_data_has_user_value(fill, "color"))
			s.color = 0xFF000000;
		if (obs_data_has_user_value(fill, "mode"))
			s.fillMode = bg_fill_mode_from_str(obs_data_get_string(fill, "mode"));
		obs_data_release(fill);
	}

	obs_data_t *image = obs_data_get_obj(data, "image");
	if (image) {
		s.imageEnabled = obs_data_get_bool(image, "enabled");
		s.imagePath = obs_data_get_string(image, "path");
		s.imageFitMode = image_fit_mode_from_str(obs_data_get_string(image, "fit"));
		obs_data_release(image);
	}
	/* Phase 3 / M6.6 H.5 hardening: clamp path length, same idiom as
	 * LabelSettings::fontFamily and LostSignalSettings paths. */
	if (s.imagePath.size() > 4096)
		s.imagePath.resize(4096);
	return s;
}

/* ========== LabelSettings ========== */

obs_data_t *LabelSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();

	obs_data_t *display = obs_data_create();
	obs_data_set_string(display, "mode", label_display_mode_to_str(displayMode));
	obs_data_set_string(display, "position", label_position_to_str(position));
	obs_data_set_obj(data, "display", display);
	obs_data_release(display);

	obs_data_t *typography = obs_data_create();
	obs_data_set_string(typography, "fontFamily", fontFamily.c_str());
	obs_data_set_string(typography, "statusFontFamily", statusFontFamily.c_str());
	obs_data_set_int(typography, "fontSize", fontSize);
	obs_data_set_string(typography, "scaleMode", font_scale_mode_to_str(fontScaleMode));
	obs_data_set_int(typography, "minFontSize", minFontSize);
	obs_data_set_int(typography, "maxFontSize", maxFontSize);
	obs_data_set_obj(data, "typography", typography);
	obs_data_release(typography);

	obs_data_t *colors = obs_data_create();
	obs_data_set_int(colors, "text", (long long)textColor);
	obs_data_set_double(colors, "backgroundOpacity", backgroundOpacity);
	obs_data_set_obj(data, "colors", colors);
	obs_data_release(colors);

	obs_data_t *box = obs_data_create();
	obs_data_set_int(box, "margin", margin);
	obs_data_set_bool(box, "regionFill", labelRegionFill);
	obs_data_set_bool(box, "backgroundRounded", backgroundRounded);
	obs_data_set_obj(data, "box", box);
	obs_data_release(box);

	return data;
}

LabelSettings LabelSettings::from_obs_data(obs_data_t *data)
{
	LabelSettings s;
	if (!data)
		return s;

	obs_data_t *display = obs_data_get_obj(data, "display");
	if (display) {
		s.displayMode = label_display_mode_from_str(obs_data_get_string(display, "mode"));
		s.position = label_position_from_str(obs_data_get_string(display, "position"));
		obs_data_release(display);
	}

	obs_data_t *typography = obs_data_get_obj(data, "typography");
	if (typography) {
		s.fontFamily = obs_data_get_string(typography, "fontFamily");
		s.statusFontFamily = obs_data_get_string(typography, "statusFontFamily");
		s.fontSize = (int)obs_data_get_int(typography, "fontSize");
		if (obs_data_has_user_value(typography, "scaleMode"))
			s.fontScaleMode = font_scale_mode_from_str(obs_data_get_string(typography, "scaleMode"));
		s.minFontSize = (int)obs_data_get_int(typography, "minFontSize");
		s.maxFontSize = (int)obs_data_get_int(typography, "maxFontSize");
		if (!obs_data_has_user_value(typography, "maxFontSize"))
			s.maxFontSize = 80;
		obs_data_release(typography);
	}
	/* Defensive: clamp fontFamily length to prevent pathological strings
	 * from a manually edited config from breaking Qt font enumeration. */
	if (s.fontFamily.size() > 128)
		s.fontFamily.resize(128);
	if (s.statusFontFamily.size() > 128)
		s.statusFontFamily.resize(128);
	if (s.fontSize < 1)
		s.fontSize = 14;
	if (s.fontSize > 200)
		s.fontSize = 200;
	if (s.minFontSize < 1)
		s.minFontSize = 8;
	if (s.minFontSize > 200)
		s.minFontSize = 200;
	if (s.maxFontSize < s.minFontSize)
		s.maxFontSize = s.minFontSize;
	if (s.maxFontSize > 400)
		s.maxFontSize = 400;

	obs_data_t *colors = obs_data_get_obj(data, "colors");
	if (colors) {
		s.textColor = (uint32_t)obs_data_get_int(colors, "text");
		if (!obs_data_has_user_value(colors, "text"))
			s.textColor = 0xFFFFFFFF;
		s.backgroundOpacity = obs_data_get_double(colors, "backgroundOpacity");
		if (!obs_data_has_user_value(colors, "backgroundOpacity"))
			s.backgroundOpacity = 0.2;
		obs_data_release(colors);
	}
	if (s.backgroundOpacity < 0.0)
		s.backgroundOpacity = 0.0;
	if (s.backgroundOpacity > 1.0)
		s.backgroundOpacity = 1.0;

	obs_data_t *box = obs_data_get_obj(data, "box");
	if (box) {
		s.margin = (int)obs_data_get_int(box, "margin");
		if (!obs_data_has_user_value(box, "margin"))
			s.margin = 4;
		s.labelRegionFill = obs_data_get_bool(box, "regionFill");
		s.backgroundRounded = obs_data_get_bool(box, "backgroundRounded");
		obs_data_release(box);
	}
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

	obs_data_t *visibility = obs_data_create();
	obs_data_set_bool(visibility, "enabled", enabled);
	obs_data_set_obj(data, "visibility", visibility);
	obs_data_release(visibility);

	obs_data_t *style = obs_data_create();
	obs_data_set_int(style, "color", (long long)color);
	obs_data_set_double(style, "opacity", opacity);
	obs_data_set_obj(data, "style", style);
	obs_data_release(style);

	obs_data_t *geometry = obs_data_create();
	obs_data_set_string(geometry, "preset", safe_area_preset_to_str(preset));
	obs_data_set_string(geometry, "anchor", safe_area_anchor_mode_to_str(anchorMode));
	obs_data_set_obj(data, "geometry", geometry);
	obs_data_release(geometry);

	return data;
}

SafeAreaSettings SafeAreaSettings::from_obs_data(obs_data_t *data)
{
	SafeAreaSettings s;
	if (!data)
		return s;

	obs_data_t *visibility = obs_data_get_obj(data, "visibility");
	if (visibility) {
		s.enabled = obs_data_get_bool(visibility, "enabled");
		obs_data_release(visibility);
	}

	obs_data_t *style = obs_data_get_obj(data, "style");
	if (style) {
		s.color = (uint32_t)obs_data_get_int(style, "color");
		if (!obs_data_has_user_value(style, "color"))
			s.color = 0xFFD0D0D0;
		s.opacity = obs_data_get_double(style, "opacity");
		if (!obs_data_has_user_value(style, "opacity"))
			s.opacity = 1.0;
		obs_data_release(style);
	}
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;

	obs_data_t *geometry = obs_data_get_obj(data, "geometry");
	if (geometry) {
		s.preset = safe_area_preset_from_str(obs_data_get_string(geometry, "preset"));
		s.anchorMode = safe_area_anchor_mode_from_str(obs_data_get_string(geometry, "anchor"));
		obs_data_release(geometry);
	}

	return s;
}

/* ========== VuMeterSettings ========== */

obs_data_t *VuMeterSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();

	obs_data_t *visibility = obs_data_create();
	obs_data_set_bool(visibility, "enabled", enabled);
	obs_data_set_obj(data, "visibility", visibility);
	obs_data_release(visibility);

	obs_data_t *source = obs_data_create();
	obs_data_set_string(source, "trackMode", vu_meter_track_mode_to_str(trackMode));
	obs_data_set_int(source, "manualTrack", manualTrackIndex);
	obs_data_set_obj(data, "source", source);
	obs_data_release(source);

	obs_data_t *placement = obs_data_create();
	obs_data_set_string(placement, "position", vu_meter_position_to_str(position));
	obs_data_set_string(placement, "anchor", vu_meter_anchor_to_str(anchor));
	obs_data_set_int(placement, "widthPx", width);
	obs_data_set_double(placement, "lengthRatio", lengthRatio);
	obs_data_set_string(placement, "alignment", vu_meter_alignment_to_str(alignment));
	obs_data_set_bool(placement, "flip", flip);
	obs_data_set_obj(data, "placement", placement);
	obs_data_release(placement);

	obs_data_t *look = obs_data_create();
	obs_data_set_double(look, "opacity", opacity);
	obs_data_set_string(look, "style", vu_meter_style_to_str(style));
	obs_data_set_bool(look, "multiChannel", multiChannelEnabled);
	obs_data_set_obj(data, "look", look);
	obs_data_release(look);

	obs_data_t *levels = obs_data_create();
	obs_data_set_double(levels, "warningDb", warningDB);
	obs_data_set_double(levels, "errorDb", errorDB);
	obs_data_set_string(levels, "decayRate", vu_meter_decay_to_str(decayRate));
	obs_data_set_obj(data, "levels", levels);
	obs_data_release(levels);

	obs_data_t *peakHold = obs_data_create();
	obs_data_set_bool(peakHold, "enabled", peakHoldEnabled);
	obs_data_set_int(peakHold, "holdMs", peakHoldMs);
	obs_data_set_double(peakHold, "decayDbPerSec", peakHoldDecayDbPerSec);
	obs_data_set_int(peakHold, "widthPx", peakHoldWidthPx);
	obs_data_set_obj(data, "peakHold", peakHold);
	obs_data_release(peakHold);

	obs_data_t *scale = obs_data_create();
	obs_data_set_bool(scale, "enabled", scaleEnabled);
	obs_data_set_string(scale, "ticksDb", scaleTicks.c_str());
	obs_data_set_bool(scale, "showLabels", scaleShowLabels);
	obs_data_set_string(scale, "fontFamily", fontFamily.c_str());
	obs_data_set_int(scale, "color", (long long)scaleColor);
	obs_data_set_string(scale, "side", vu_meter_scale_side_to_str(scaleSide));
	obs_data_set_obj(data, "scale", scale);
	obs_data_release(scale);

	return data;
}

VuMeterSettings VuMeterSettings::from_obs_data(obs_data_t *data)
{
	VuMeterSettings s;
	if (!data)
		return s;

	obs_data_t *visibility = obs_data_get_obj(data, "visibility");
	if (visibility) {
		if (obs_data_has_user_value(visibility, "enabled"))
			s.enabled = obs_data_get_bool(visibility, "enabled");
		obs_data_release(visibility);
	}

	obs_data_t *source = obs_data_get_obj(data, "source");
	if (source) {
		s.trackMode = vu_meter_track_mode_from_str(obs_data_get_string(source, "trackMode"));
		if (obs_data_has_user_value(source, "manualTrack"))
			s.manualTrackIndex = (int)obs_data_get_int(source, "manualTrack");
		obs_data_release(source);
	}
	if (s.manualTrackIndex < 1)
		s.manualTrackIndex = 1;
	if (s.manualTrackIndex > 6)
		s.manualTrackIndex = 6;

	obs_data_t *placement = obs_data_get_obj(data, "placement");
	if (placement) {
		if (obs_data_has_user_value(placement, "position"))
			s.position = vu_meter_position_from_str(obs_data_get_string(placement, "position"));
		s.anchor = vu_meter_anchor_from_str(obs_data_get_string(placement, "anchor"));
		s.width = (int)obs_data_get_int(placement, "widthPx");
		if (!obs_data_has_user_value(placement, "widthPx"))
			s.width = 24;
		s.lengthRatio = obs_data_get_double(placement, "lengthRatio");
		if (!obs_data_has_user_value(placement, "lengthRatio"))
			s.lengthRatio = 1.0;
		s.alignment = vu_meter_alignment_from_str(obs_data_get_string(placement, "alignment"));
		s.flip = obs_data_get_bool(placement, "flip");
		obs_data_release(placement);
	}

	obs_data_t *look = obs_data_get_obj(data, "look");
	if (look) {
		s.opacity = obs_data_get_double(look, "opacity");
		if (!obs_data_has_user_value(look, "opacity"))
			s.opacity = 0.75;
		s.style = vu_meter_style_from_str(obs_data_get_string(look, "style"));
		if (obs_data_has_user_value(look, "multiChannel"))
			s.multiChannelEnabled = obs_data_get_bool(look, "multiChannel");
		obs_data_release(look);
	}
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;
	if (s.width < 1)
		s.width = 8;
	if (s.width > 64)
		s.width = 64;
	if (s.lengthRatio < 0.0)
		s.lengthRatio = 0.0;
	if (s.lengthRatio > 1.0)
		s.lengthRatio = 1.0;

	obs_data_t *levels = obs_data_get_obj(data, "levels");
	if (levels) {
		s.warningDB = obs_data_get_double(levels, "warningDb");
		if (!obs_data_has_user_value(levels, "warningDb"))
			s.warningDB = -20.0;
		s.errorDB = obs_data_get_double(levels, "errorDb");
		if (!obs_data_has_user_value(levels, "errorDb"))
			s.errorDB = -9.0;
		s.decayRate = vu_meter_decay_from_str(obs_data_get_string(levels, "decayRate"));
		obs_data_release(levels);
	}
	if (s.warningDB < -96.0)
		s.warningDB = -96.0;
	if (s.warningDB > 0.0)
		s.warningDB = 0.0;
	if (s.errorDB < -96.0)
		s.errorDB = -96.0;
	if (s.errorDB > 0.0)
		s.errorDB = 0.0;
	/* Ensure errorDB >= warningDB (red zone above yellow zone on dB axis) */
	if (s.errorDB < s.warningDB)
		s.errorDB = s.warningDB;

	obs_data_t *peakHold = obs_data_get_obj(data, "peakHold");
	if (peakHold) {
		if (obs_data_has_user_value(peakHold, "enabled"))
			s.peakHoldEnabled = obs_data_get_bool(peakHold, "enabled");
		if (obs_data_has_user_value(peakHold, "holdMs"))
			s.peakHoldMs = (int)obs_data_get_int(peakHold, "holdMs");
		if (obs_data_has_user_value(peakHold, "decayDbPerSec"))
			s.peakHoldDecayDbPerSec = obs_data_get_double(peakHold, "decayDbPerSec");
		if (obs_data_has_user_value(peakHold, "widthPx"))
			s.peakHoldWidthPx = (int)obs_data_get_int(peakHold, "widthPx");
		obs_data_release(peakHold);
	}
	if (s.peakHoldMs < 100)
		s.peakHoldMs = 100;
	if (s.peakHoldMs > 5000)
		s.peakHoldMs = 5000;
	if (s.peakHoldDecayDbPerSec < 1.0)
		s.peakHoldDecayDbPerSec = 1.0;
	if (s.peakHoldDecayDbPerSec > 60.0)
		s.peakHoldDecayDbPerSec = 60.0;
	if (s.peakHoldWidthPx < 1)
		s.peakHoldWidthPx = 1;
	if (s.peakHoldWidthPx > 4)
		s.peakHoldWidthPx = 4;

	obs_data_t *scale = obs_data_get_obj(data, "scale");
	if (scale) {
		if (obs_data_has_user_value(scale, "enabled"))
			s.scaleEnabled = obs_data_get_bool(scale, "enabled");
		if (obs_data_has_user_value(scale, "ticksDb"))
			s.scaleTicks = obs_data_get_string(scale, "ticksDb");
		if (obs_data_has_user_value(scale, "showLabels"))
			s.scaleShowLabels = obs_data_get_bool(scale, "showLabels");
		if (obs_data_has_user_value(scale, "fontFamily"))
			s.fontFamily = obs_data_get_string(scale, "fontFamily");
		if (obs_data_has_user_value(scale, "color"))
			s.scaleColor = (uint32_t)obs_data_get_int(scale, "color");
		if (obs_data_has_user_value(scale, "side"))
			s.scaleSide = vu_meter_scale_side_from_str(obs_data_get_string(scale, "side"));
		obs_data_release(scale);
	}
	if (s.fontFamily.size() > 128)
		s.fontFamily.resize(128);
	return s;
}

/* ========== OverlaySettings ========== */

obs_data_t *OverlaySettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();

	obs_data_t *visibility = obs_data_create();
	obs_data_set_bool(visibility, "enabled", enabled);
	obs_data_set_obj(data, "visibility", visibility);
	obs_data_release(visibility);

	obs_data_t *image = obs_data_create();
	obs_data_set_string(image, "path", imagePath.c_str());
	obs_data_set_obj(data, "image", image);
	obs_data_release(image);

	obs_data_t *placement = obs_data_create();
	obs_data_set_string(placement, "fit", overlay_fit_mode_to_str(fitMode));
	obs_data_set_string(placement, "anchor", overlay_anchor_mode_to_str(anchorMode));
	obs_data_set_obj(data, "placement", placement);
	obs_data_release(placement);

	obs_data_t *style = obs_data_create();
	obs_data_set_double(style, "opacity", opacity);
	obs_data_set_obj(data, "style", style);
	obs_data_release(style);

	return data;
}

OverlaySettings OverlaySettings::from_obs_data(obs_data_t *data)
{
	OverlaySettings s;
	if (!data)
		return s;

	obs_data_t *visibility = obs_data_get_obj(data, "visibility");
	if (visibility) {
		s.enabled = obs_data_get_bool(visibility, "enabled");
		obs_data_release(visibility);
	}

	obs_data_t *image = obs_data_get_obj(data, "image");
	if (image) {
		s.imagePath = obs_data_get_string(image, "path");
		obs_data_release(image);
	}
	/* Phase 3 / M6.6 H.5 hardening: clamp path length. */
	if (s.imagePath.size() > 4096)
		s.imagePath.resize(4096);

	obs_data_t *style = obs_data_get_obj(data, "style");
	if (style) {
		s.opacity = obs_data_get_double(style, "opacity");
		if (!obs_data_has_user_value(style, "opacity"))
			s.opacity = 1.0;
		obs_data_release(style);
	}
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;

	obs_data_t *placement = obs_data_get_obj(data, "placement");
	if (placement) {
		s.fitMode = overlay_fit_mode_from_str(obs_data_get_string(placement, "fit"));
		if (obs_data_has_user_value(placement, "anchor"))
			s.anchorMode = overlay_anchor_mode_from_str(obs_data_get_string(placement, "anchor"));
		obs_data_release(placement);
	}
	return s;
}

/* ========== HighlightSettings ========== */

obs_data_t *HighlightSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();

	obs_data_t *visibility = obs_data_create();
	obs_data_set_bool(visibility, "enabled", enabled);
	obs_data_set_obj(data, "visibility", visibility);
	obs_data_release(visibility);

	obs_data_t *colors = obs_data_create();
	obs_data_set_int(colors, "pgm", pgmColor);
	obs_data_set_int(colors, "prvw", prvwColor);
	obs_data_set_obj(data, "colors", colors);
	obs_data_release(colors);

	obs_data_t *nestedSceneStyle = obs_data_create();
	obs_data_set_bool(nestedSceneStyle, "dashed", nestedDashed);
	obs_data_set_int(nestedSceneStyle, "dashLengthPx", dashLengthPx);
	obs_data_set_int(nestedSceneStyle, "dashGapPx", dashGapPx);
	obs_data_set_obj(data, "nestedSceneStyle", nestedSceneStyle);
	obs_data_release(nestedSceneStyle);

	obs_data_t *border = obs_data_create();
	obs_data_set_int(border, "minThicknessPx", minThicknessPx);
	obs_data_set_obj(data, "border", border);
	obs_data_release(border);

	return data;
}

HighlightSettings HighlightSettings::from_obs_data(obs_data_t *data)
{
	HighlightSettings s;
	if (!data)
		return s;

	obs_data_t *visibility = obs_data_get_obj(data, "visibility");
	if (visibility) {
		if (obs_data_has_user_value(visibility, "enabled"))
			s.enabled = obs_data_get_bool(visibility, "enabled");
		obs_data_release(visibility);
	}

	obs_data_t *colors = obs_data_get_obj(data, "colors");
	if (colors) {
		if (obs_data_has_user_value(colors, "pgm"))
			s.pgmColor = (uint32_t)obs_data_get_int(colors, "pgm");
		if (obs_data_has_user_value(colors, "prvw"))
			s.prvwColor = (uint32_t)obs_data_get_int(colors, "prvw");
		obs_data_release(colors);
	}

	obs_data_t *nestedSceneStyle = obs_data_get_obj(data, "nestedSceneStyle");
	if (nestedSceneStyle) {
		if (obs_data_has_user_value(nestedSceneStyle, "dashed"))
			s.nestedDashed = obs_data_get_bool(nestedSceneStyle, "dashed");
		if (obs_data_has_user_value(nestedSceneStyle, "dashLengthPx"))
			s.dashLengthPx = (int)obs_data_get_int(nestedSceneStyle, "dashLengthPx");
		if (obs_data_has_user_value(nestedSceneStyle, "dashGapPx"))
			s.dashGapPx = (int)obs_data_get_int(nestedSceneStyle, "dashGapPx");
		obs_data_release(nestedSceneStyle);
	}

	obs_data_t *border = obs_data_get_obj(data, "border");
	if (border) {
		if (obs_data_has_user_value(border, "minThicknessPx"))
			s.minThicknessPx = (int)obs_data_get_int(border, "minThicknessPx");
		obs_data_release(border);
	}
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
