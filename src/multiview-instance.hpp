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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <obs-data.h>

/* ========== Visual Settings Enums ========== */

enum class InheritanceMode { Inherit, Override };

enum class LabelDisplayMode { None, Overlay, Below };

enum class LabelPosition { Top, Bottom };

enum class FontScaleMode { Fixed, ScaleWithCell };

enum class ImageFitMode { Fit, Stretch };

enum class SafeAreaPreset { EBU_R95 };

enum class VuMeterPosition { Left, Right, Bottom };

enum class VuMeterStyle { Bar };

enum class OverlayFitMode { Fit, Stretch };

enum class OverlayAnchorMode { Cell, Signal };

enum class BackgroundFillMode { FillSignalOnly, FillEntireCell };

/* ========== Visual Settings Group Structs ========== */

struct BackgroundSettings {
	bool colorEnabled = false;
	uint32_t color = 0xFF000000; /* ARGB black */
	BackgroundFillMode fillMode = BackgroundFillMode::FillSignalOnly;
	bool labelRegionFill = false; /* Below mode: fill label row with bgColor */
	bool imageEnabled = false;
	std::string imagePath;
	ImageFitMode imageFitMode = ImageFitMode::Fit;

	obs_data_t *to_obs_data() const;
	static BackgroundSettings from_obs_data(obs_data_t *data);
};

struct LabelSettings {
	LabelDisplayMode displayMode = LabelDisplayMode::Overlay;
	LabelPosition position = LabelPosition::Bottom;
	std::string fontFamily; /* empty = system default */
	int fontSize = 14;
	FontScaleMode fontScaleMode = FontScaleMode::ScaleWithCell;
	int minFontSize = 8;
	int maxFontSize = 48;
	uint32_t textColor = 0xFFFFFFFF; /* ARGB white */
	double backgroundOpacity = 0.2;
	bool backgroundRounded = false;
	int margin = 4;

	obs_data_t *to_obs_data() const;
	static LabelSettings from_obs_data(obs_data_t *data);
};

struct SafeAreaSettings {
	bool enabled = false;
	SafeAreaPreset preset = SafeAreaPreset::EBU_R95;
	uint32_t color = 0xFFD0D0D0; /* ARGB light grey - matches OBS native OUTLINE_COLOR */
	double opacity = 1.0;

	obs_data_t *to_obs_data() const;
	static SafeAreaSettings from_obs_data(obs_data_t *data);
};

struct VuMeterSettings {
	bool enabled = false;
	VuMeterPosition position = VuMeterPosition::Right;
	double opacity = 0.8;
	int width = 8;
	VuMeterStyle style = VuMeterStyle::Bar;

	obs_data_t *to_obs_data() const;
	static VuMeterSettings from_obs_data(obs_data_t *data);
};

struct OverlaySettings {
	bool enabled = false;
	std::string imagePath;
	double opacity = 1.0;
	OverlayFitMode fitMode = OverlayFitMode::Fit;
	OverlayAnchorMode anchorMode = OverlayAnchorMode::Signal;

	obs_data_t *to_obs_data() const;
	static OverlaySettings from_obs_data(obs_data_t *data);
};

/* ========== Visual Settings Containers ========== */

struct GlobalVisualSettings {
	BackgroundSettings background;
	LabelSettings label;
	SafeAreaSettings safeArea;
	VuMeterSettings vuMeter;
	OverlaySettings overlay;

	obs_data_t *to_obs_data() const;
	static GlobalVisualSettings from_obs_data(obs_data_t *data);
};

struct InstanceVisualSettings {
	InheritanceMode backgroundMode = InheritanceMode::Inherit;
	BackgroundSettings background;
	InheritanceMode labelMode = InheritanceMode::Inherit;
	LabelSettings label;
	InheritanceMode safeAreaMode = InheritanceMode::Inherit;
	SafeAreaSettings safeArea;
	InheritanceMode vuMeterMode = InheritanceMode::Inherit;
	VuMeterSettings vuMeter;
	InheritanceMode overlayMode = InheritanceMode::Inherit;
	OverlaySettings overlay;

	obs_data_t *to_obs_data() const;
	static InstanceVisualSettings from_obs_data(obs_data_t *data);
};

struct CellVisualSettings {
	int row = -1;
	int col = -1;
	InheritanceMode backgroundMode = InheritanceMode::Inherit;
	BackgroundSettings background;
	InheritanceMode labelMode = InheritanceMode::Inherit;
	LabelSettings label;
	InheritanceMode safeAreaMode = InheritanceMode::Inherit;
	SafeAreaSettings safeArea;
	InheritanceMode vuMeterMode = InheritanceMode::Inherit;
	VuMeterSettings vuMeter;
	InheritanceMode overlayMode = InheritanceMode::Inherit;
	OverlaySettings overlay;

	obs_data_t *to_obs_data() const;
	static CellVisualSettings from_obs_data(obs_data_t *data);
};

struct EffectiveCellVisualSettings {
	BackgroundSettings background;
	LabelSettings label;
	SafeAreaSettings safeArea;
	VuMeterSettings vuMeter;
	OverlaySettings overlay;
};

/* Resolve effective visual settings via group-level inheritance chain:
 * cell (if override) -> instance (if override) -> global */
EffectiveCellVisualSettings resolve_effective_visual_settings(const GlobalVisualSettings &global,
							      const InstanceVisualSettings &instance,
							      const CellVisualSettings *cell);

/* ========== Core Data Structs ========== */

struct CellAssignment {
	int row = -1;     // grid row position (-1 = legacy/unset)
	int col = -1;     // grid col position (-1 = legacy/unset)
	std::string type; // "pgm", "prvw", "scene", "source", ""
	std::string name; // scene/source name, empty for pgm/prvw/empty

	obs_data_t *to_obs_data() const;
	static CellAssignment from_obs_data(obs_data_t *data);
};

struct SpanRegion {
	int row;
	int col;
	int rowSpan;
	int colSpan;

	obs_data_t *to_obs_data() const;
	static SpanRegion from_obs_data(obs_data_t *data);
};

struct LayoutData {
	int rows = 4;
	int columns = 4;
	int gutterPx = 4;
	std::vector<SpanRegion> spans;

	obs_data_t *to_obs_data() const;
	static LayoutData from_obs_data(obs_data_t *data);
};

struct MultiviewInstance {
	std::string uuid;
	std::string name;
	std::string folder; /* UI-only grouping tag, empty = root */
	LayoutData layout;
	std::vector<CellAssignment> cellAssignments;
	InstanceVisualSettings visualSettings;
	std::vector<CellVisualSettings> cellVisualSettings;

	bool useGlobalGutter = true;
	bool layoutDirty = false;
	bool signalDirty = false;

	int effective_gutter(int globalGutter) const;

	/* Find cell visual settings for given coordinate, or nullptr */
	const CellVisualSettings *find_cell_visual(int row, int col) const;

	obs_data_t *to_obs_data() const;
	static MultiviewInstance from_obs_data(obs_data_t *data);

	static MultiviewInstance create_new(const std::string &name);
	MultiviewInstance clone_instance(const std::string &newName) const;
};

struct LayoutPreset {
	std::string uuid;
	std::string name;
	LayoutData layout;

	obs_data_t *to_obs_data() const;
	static LayoutPreset from_obs_data(obs_data_t *data);
};

struct GlobalSettings {
	int defaultGutterPx = 4;
	bool reResolveInheritObs = true;
	double reResolveCustomFps = 30.0;
	GlobalVisualSettings visualSettings;

	obs_data_t *to_obs_data() const;
	static GlobalSettings from_obs_data(obs_data_t *data);
};
