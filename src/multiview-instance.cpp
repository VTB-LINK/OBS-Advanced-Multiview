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
#include <obs.h>
#include <obs-data.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <plugin-support.h>

#include <cstdio>

#include <cstring>

/* ========== Enum serialization helpers ========== */

#include "multiview-instance-serialize.hpp"

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

	obs_data_t *os = outputSettings.to_obs_data();
	obs_data_set_obj(data, "outputSettings", os);
	obs_data_release(os);

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

	/* External output (issue #11). Absent key -> all-default (disabled). */
	obs_data_t *os = obs_data_get_obj(data, "outputSettings");
	inst.outputSettings = InstanceOutputSettings::from_obs_data(os);
	if (os)
		obs_data_release(os);

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

	/* Issue #10: NDI readback double-buffer toggle (default on). */
	obs_data_set_bool(data, "ndiOutputDoubleBuffer", ndiOutputDoubleBuffer);

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

	/* Issue #10: NDI readback double-buffer. Default ON when the key is absent
	 * (new config / upgrade) — protects the main program output by default. */
	if (obs_data_has_user_value(data, "ndiOutputDoubleBuffer"))
		gs.ndiOutputDoubleBuffer = obs_data_get_bool(data, "ndiOutputDoubleBuffer");
	else
		gs.ndiOutputDoubleBuffer = true;

	return gs;
}
