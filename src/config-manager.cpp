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

#include "config-manager.hpp"

#include "amv-logging.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstdio>

/* ---- helpers ---- */

std::string ConfigManager::sanitize_filename(const std::string &name)
{
	std::string result;
	result.reserve(name.size());
	for (char c : name) {
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
		    c == '|')
			result += '_';
		else
			result += c;
	}
	/* trim leading/trailing underscores and spaces */
	size_t start = result.find_first_not_of("_ ");
	size_t end = result.find_last_not_of("_ ");
	if (start == std::string::npos)
		return "default";
	return result.substr(start, end - start + 1);
}

std::string ConfigManager::get_current_scene_collection()
{
	char *name = obs_frontend_get_current_scene_collection();
	std::string result = name ? name : "default";
	bfree(name);
	return result;
}

/* ---- construction ---- */

ConfigManager::ConfigManager()
{
	char *path = obs_module_config_path("");
	if (path) {
		config_dir_ = path;
		bfree(path);
	}
	current_collection_ = get_current_scene_collection();
}

ConfigManager::~ConfigManager() = default;

/* ---- file path ---- */

std::string ConfigManager::get_config_file_path() const
{
	std::string safe = sanitize_filename(current_collection_);
	return config_dir_ + "settings-" + safe + ".json";
}

/* ---- load ---- */

bool ConfigManager::load()
{
	if (config_dir_.empty()) {
		obs_log(LOG_ERROR, "config_dir_ is empty, cannot load config");
		return false;
	}
	current_collection_ = get_current_scene_collection();
	std::string path = get_config_file_path();
	return load_from_file(path);
}

bool ConfigManager::load_from_file(const std::string &path)
{
	if (!os_file_exists(path.c_str())) {
		obs_log(LOG_INFO, "no config file found, using defaults: %s", path.c_str());
		instances_.clear();
		layout_presets_.clear();
		global_settings_ = GlobalSettings();
		return true;
	}

	obs_data_t *data = obs_data_create_from_json_file(path.c_str());
	if (!data) {
		obs_log(LOG_ERROR, "failed to parse config file: %s", path.c_str());
		return false;
	}

	/* configVersion: tracks structural changes for future migration.
	 * v1 = Phase 1 baseline. v2 = Phase 2 (visualSettings across
	 * global / instance / cell). v3 = Phase 3 / M5 (adds GlobalSettings.lostSignal
	 * and per-cell cellLostSignalSettings overrides). Missing fields on older
	 * configs are safe-fallback via per-struct from_obs_data() defaults; we
	 * only log the upgrade so user / support can trace it. */
	int version = (int)obs_data_get_int(data, "configVersion");
	if (version > 0 && version < CURRENT_CONFIG_VERSION) {
		obs_log(LOG_INFO, "upgrading config from v%d to v%d", version, CURRENT_CONFIG_VERSION);
	} else if (version > CURRENT_CONFIG_VERSION) {
		obs_log(LOG_WARNING, "config v%d is newer than supported v%d; some fields may be ignored or reset",
			version, CURRENT_CONFIG_VERSION);
	}

	/* global settings */
	obs_data_t *gs = obs_data_get_obj(data, "globalSettings");
	if (gs) {
		global_settings_ = GlobalSettings::from_obs_data(gs);
		obs_data_release(gs);
	} else {
		global_settings_ = GlobalSettings();
	}

	/* Phase 3 hardening tail: mirror persisted Detailed logs flag into
	 * the process-wide atomic so static-context provider code can read
	 * it without touching ConfigManager. */
	amv::set_detailed_logs_enabled(global_settings_.detailedLogs);

	/* instances */
	instances_.clear();
	obs_data_array_t *arr = obs_data_get_array(data, "instances");
	if (arr) {
		size_t count = obs_data_array_count(arr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			instances_.push_back(MultiviewInstance::from_obs_data(item));
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	/* layout presets (reserved) */
	layout_presets_.clear();
	obs_data_array_t *parr = obs_data_get_array(data, "layoutPresets");
	if (parr) {
		size_t count = obs_data_array_count(parr);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(parr, i);
			layout_presets_.push_back(LayoutPreset::from_obs_data(item));
			obs_data_release(item);
		}
		obs_data_array_release(parr);
	}

	obs_data_release(data);

	obs_log(LOG_INFO, "loaded %zu instance(s) from %s", instances_.size(), path.c_str());
	return true;
}

/* ---- save (atomic: tmp + rename) ---- */

bool ConfigManager::save()
{
	if (config_dir_.empty()) {
		obs_log(LOG_ERROR, "config_dir_ is empty, cannot save config");
		return false;
	}
	std::string path = get_config_file_path();
	return save_to_file(path);
}

bool ConfigManager::save_to_file(const std::string &path)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "configVersion", CURRENT_CONFIG_VERSION);

	/* global settings */
	obs_data_t *gs = global_settings_.to_obs_data();
	obs_data_set_obj(data, "globalSettings", gs);
	obs_data_release(gs);

	/* instances */
	obs_data_array_t *arr = obs_data_array_create();
	for (auto &inst : instances_) {
		obs_data_t *item = inst.to_obs_data();
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "instances", arr);
	obs_data_array_release(arr);

	/* layout presets */
	obs_data_array_t *parr = obs_data_array_create();
	for (auto &lp : layout_presets_) {
		obs_data_t *item = lp.to_obs_data();
		obs_data_array_push_back(parr, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "layoutPresets", parr);
	obs_data_array_release(parr);

	/* atomic save: write to tmp then rename */
	bool ok = obs_data_save_json_safe(data, path.c_str(), ".tmp", ".bak");
	obs_data_release(data);

	if (!ok) {
		obs_log(LOG_ERROR, "failed to save config to %s", path.c_str());
		return false;
	}

	obs_log(LOG_INFO, "saved %zu instance(s) to %s", instances_.size(), path.c_str());
	return true;
}

/* ---- scene collection change ---- */

void ConfigManager::on_scene_collection_changed()
{
	std::string new_collection = get_current_scene_collection();
	if (new_collection == current_collection_)
		return;

	obs_log(LOG_INFO, "scene collection changed: '%s' -> '%s'", current_collection_.c_str(),
		new_collection.c_str());

	/* save current before switching */
	save();

	current_collection_ = new_collection;
	load();
}

/* ---- instance CRUD ---- */

MultiviewInstance *ConfigManager::add_instance(const std::string &name)
{
	instances_.push_back(MultiviewInstance::create_new(name));
	return &instances_.back();
}

bool ConfigManager::rename_instance(const std::string &uuid, const std::string &newName)
{
	MultiviewInstance *inst = find_instance(uuid);
	if (!inst)
		return false;
	inst->name = newName;
	return true;
}

MultiviewInstance *ConfigManager::clone_instance(const std::string &uuid, const std::string &newName)
{
	MultiviewInstance *src = find_instance(uuid);
	if (!src)
		return nullptr;
	instances_.push_back(src->clone_instance(newName));
	return &instances_.back();
}

bool ConfigManager::delete_instance(const std::string &uuid)
{
	auto it = std::remove_if(instances_.begin(), instances_.end(),
				 [&](const MultiviewInstance &inst) { return inst.uuid == uuid; });
	if (it == instances_.end())
		return false;
	instances_.erase(it, instances_.end());
	return true;
}

MultiviewInstance *ConfigManager::find_instance(const std::string &uuid)
{
	for (auto &inst : instances_) {
		if (inst.uuid == uuid)
			return &inst;
	}
	return nullptr;
}

const std::vector<MultiviewInstance> &ConfigManager::instances() const
{
	return instances_;
}

std::vector<MultiviewInstance> &ConfigManager::instances_mutable()
{
	return instances_;
}

/* ---- global settings ---- */

GlobalSettings &ConfigManager::global_settings()
{
	return global_settings_;
}

const GlobalSettings &ConfigManager::global_settings() const
{
	return global_settings_;
}

/* ---- layout presets ---- */

const std::vector<LayoutPreset> &ConfigManager::layout_presets() const
{
	return layout_presets_;
}
