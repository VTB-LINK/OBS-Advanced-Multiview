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
	return VuMeterPosition::Right;
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

/* ========== BackgroundSettings ========== */

obs_data_t *BackgroundSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "colorEnabled", colorEnabled);
	obs_data_set_int(data, "color", (long long)color);
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
	s.colorEnabled = obs_data_get_bool(data, "colorEnabled");
	s.color = (uint32_t)obs_data_get_int(data, "color");
	if (!obs_data_has_user_value(data, "color"))
		s.color = 0xFF000000;
	s.imageEnabled = obs_data_get_bool(data, "imageEnabled");
	s.imagePath = obs_data_get_string(data, "imagePath");
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
	s.fontSize = (int)obs_data_get_int(data, "fontSize");
	if (s.fontSize < 1)
		s.fontSize = 14;
	s.fontScaleMode = font_scale_mode_from_str(obs_data_get_string(data, "fontScaleMode"));
	s.minFontSize = (int)obs_data_get_int(data, "minFontSize");
	if (s.minFontSize < 1)
		s.minFontSize = 8;
	s.maxFontSize = (int)obs_data_get_int(data, "maxFontSize");
	if (s.maxFontSize < s.minFontSize)
		s.maxFontSize = 48;
	s.textColor = (uint32_t)obs_data_get_int(data, "textColor");
	if (!obs_data_has_user_value(data, "textColor"))
		s.textColor = 0xFFFFFFFF;
	s.backgroundOpacity = obs_data_get_double(data, "backgroundOpacity");
	if (!obs_data_has_user_value(data, "backgroundOpacity"))
		s.backgroundOpacity = 0.6;
	if (s.backgroundOpacity < 0.0)
		s.backgroundOpacity = 0.0;
	if (s.backgroundOpacity > 1.0)
		s.backgroundOpacity = 1.0;
	s.backgroundRounded = obs_data_get_bool(data, "backgroundRounded");
	s.margin = (int)obs_data_get_int(data, "margin");
	if (!obs_data_has_user_value(data, "margin"))
		s.margin = 4;
	return s;
}

/* ========== SafeAreaSettings ========== */

obs_data_t *SafeAreaSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_string(data, "preset", safe_area_preset_to_str(preset));
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
	s.color = (uint32_t)obs_data_get_int(data, "color");
	if (!obs_data_has_user_value(data, "color"))
		s.color = 0xFFFFFFFF;
	s.opacity = obs_data_get_double(data, "opacity");
	if (!obs_data_has_user_value(data, "opacity"))
		s.opacity = 0.5;
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
	return data;
}

VuMeterSettings VuMeterSettings::from_obs_data(obs_data_t *data)
{
	VuMeterSettings s;
	if (!data)
		return s;
	s.enabled = obs_data_get_bool(data, "enabled");
	s.position = vu_meter_position_from_str(obs_data_get_string(data, "position"));
	s.opacity = obs_data_get_double(data, "opacity");
	if (!obs_data_has_user_value(data, "opacity"))
		s.opacity = 0.8;
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;
	s.width = (int)obs_data_get_int(data, "width");
	if (s.width < 1)
		s.width = 8;
	if (s.width > 64)
		s.width = 64;
	s.style = vu_meter_style_from_str(obs_data_get_string(data, "style"));
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
	s.opacity = obs_data_get_double(data, "opacity");
	if (!obs_data_has_user_value(data, "opacity"))
		s.opacity = 1.0;
	if (s.opacity < 0.0)
		s.opacity = 0.0;
	if (s.opacity > 1.0)
		s.opacity = 1.0;
	s.fitMode = overlay_fit_mode_from_str(obs_data_get_string(data, "fitMode"));
	s.anchorMode = overlay_anchor_mode_from_str(obs_data_get_string(data, "anchorMode"));
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

	return eff;
}

/* ---------- CellAssignment ---------- */

obs_data_t *CellAssignment::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "row", row);
	obs_data_set_int(data, "col", col);
	obs_data_set_string(data, "type", type.c_str());
	obs_data_set_string(data, "name", name.c_str());
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

	obs_data_t *layoutData = layout.to_obs_data();
	obs_data_set_obj(data, "layout", layoutData);
	obs_data_release(layoutData);

	obs_data_array_t *arr = obs_data_array_create();
	for (auto &ca : cellAssignments) {
		if (ca.type.empty())
			continue; /* Don't save empty assignments */
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
			/* Skip empty assignments */
			if (ca.type.empty())
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

	return data;
}

GlobalSettings GlobalSettings::from_obs_data(obs_data_t *data)
{
	GlobalSettings gs;
	gs.defaultGutterPx = (int)obs_data_get_int(data, "defaultGutterPx");
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

	return gs;
}
