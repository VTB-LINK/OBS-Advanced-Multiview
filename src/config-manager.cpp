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

#include <QUuid>

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

std::string ConfigManager::config_file_path_for(const std::string &collection) const
{
	return config_dir_ + "settings-" + sanitize_filename(collection) + ".json";
}

std::string ConfigManager::get_config_file_path() const
{
	return config_file_path_for(current_collection_);
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

void ConfigManager::refresh_known_collections()
{
	known_collections_.clear();
	char **list = obs_frontend_get_scene_collections();
	if (!list)
		return;
	for (char **p = list; *p; p++)
		known_collections_.insert(*p);
	/* Single allocation (convert_string_list): one bfree frees the whole list. */
	bfree(list);
}

bool ConfigManager::collection_shares_content(const std::vector<MultiviewInstance> &instances)
{
	/* True if the just-entered collection shares content with these instances,
	 * i.e. it is a duplicate (or close enough that copying makes sense) rather
	 * than an unrelated / empty new collection.
	 *
	 * Requiring at least ONE referenced scene/source to resolve by name is
	 * deliberately lenient: a real duplicate shares all names, a brand-new empty
	 * collection (only the default "Scene") shares none, and a large/complex
	 * collection still qualifies even if a stray cell references something that
	 * no longer resolves. pgm/prvw/external cells carry no collection-scene name;
	 * an all-external setup (no internal refs to check) is treated as
	 * compatible. */
	bool has_internal_ref = false;
	for (const auto &inst : instances) {
		for (const auto &ca : inst.cellAssignments) {
			if (ca.type != "scene" && ca.type != "source")
				continue;
			if (ca.name.empty())
				continue;
			has_internal_ref = true;
			obs_source_t *s = obs_get_source_by_name(ca.name.c_str());
			if (s) {
				obs_source_release(s);
				return true;
			}
		}
	}
	return !has_internal_ref;
}

ConfigManager::SceneCollectionChange ConfigManager::on_scene_collection_changed()
{
	SceneCollectionChange change;

	std::string new_collection = get_current_scene_collection();
	if (new_collection == current_collection_)
		return change;

	obs_log(LOG_INFO, "scene collection changed: '%s' -> '%s'", current_collection_.c_str(),
		new_collection.c_str());

	/* Snapshot the collection we are leaving BEFORE we save/switch/load, so the
	 * UI can offer to copy it if this turns out to be a duplicate. */
	const std::string old_collection = current_collection_;
	std::vector<MultiviewInstance> old_instances = instances_;
	GlobalSettings old_global = global_settings_;
	std::vector<LayoutPreset> old_presets = layout_presets_;

	/* "Brand new" = the collection we are switching TO was not present as of the
	 * last snapshot (taken at load / after the previous change). A real
	 * duplicate always creates a new collection; switching to any pre-existing
	 * collection (even one with identical scene names) is therefore never
	 * treated as a duplicate. Checked BEFORE refreshing the snapshot. */
	const bool brand_new = known_collections_.count(new_collection) == 0;

	/* save current before switching */
	save();

	previous_collection_ = old_collection;
	current_collection_ = new_collection;
	const bool new_had_config = os_file_exists(get_config_file_path().c_str());
	load();

	change.sourceCollection = old_collection;
	change.newCollection = new_collection;

	const bool shares = collection_shares_content(old_instances);
	const bool candidate = brand_new && !new_had_config && !old_instances.empty() && shares;
	/* Always log the decision so a "no prompt appeared" report can be traced to
	 * the exact condition that failed. */
	obs_log(LOG_INFO,
		"[scene-collection] duplicate check '%s' -> '%s': brand_new=%d new_has_config=%d src_instances=%zu shares_content=%d => candidate=%d",
		old_collection.c_str(), new_collection.c_str(), (int)brand_new, (int)new_had_config,
		old_instances.size(), (int)shares, (int)candidate);

	if (candidate) {
		change.duplicateCandidate = true;
		change.instanceCount = (int)old_instances.size();
		change.sourceInstances = std::move(old_instances);
		change.sourceGlobal = old_global;
		change.sourcePresets = std::move(old_presets);
	}

	/* Refresh AFTER the brand-new check so the just-entered collection counts as
	 * known from the next change onward. */
	refresh_known_collections();
	return change;
}

bool ConfigManager::on_scene_collection_renamed()
{
	/* CHANGED fires before RENAMED in OBS's rename flow, so by now
	 * current_collection_ is already the new name and previous_collection_ holds
	 * the old name. Move the old config file to the new name (a rename keeps the
	 * same instances, so no UUID regeneration) and reload. */
	const std::string old_collection = previous_collection_;
	const std::string new_collection = current_collection_;
	if (old_collection.empty() || old_collection == new_collection)
		return false;

	const std::string old_path = config_file_path_for(old_collection);
	const std::string new_path = config_file_path_for(new_collection);
	if (!os_file_exists(old_path.c_str()))
		return false;
	if (os_file_exists(new_path.c_str())) {
		/* Target already exists (name reuse); don't clobber it. Keep whatever
		 * the CHANGED handler already loaded for the new name. */
		obs_log(LOG_WARNING, "scene collection rename: '%s' already has a config; not overwriting",
			new_collection.c_str());
		return false;
	}

	if (os_rename(old_path.c_str(), new_path.c_str()) != 0) {
		obs_log(LOG_WARNING, "scene collection rename: failed to move config '%s' -> '%s'", old_path.c_str(),
			new_path.c_str());
		return false;
	}

	obs_log(LOG_INFO, "scene collection renamed: moved config '%s' -> '%s'", old_collection.c_str(),
		new_collection.c_str());
	load(); /* reload the moved config under the new name */
	return true;
}

void ConfigManager::seed_current_from_snapshot(const SceneCollectionChange &snapshot)
{
	/* Copy the source collection's config into the CURRENT (new) collection:
	 * global settings + presets verbatim; instances with fresh UUIDs since they
	 * are copies, mirroring how OBS regenerates source UUIDs on duplicate. */
	global_settings_ = snapshot.sourceGlobal;
	layout_presets_ = snapshot.sourcePresets;
	instances_.clear();
	instances_.reserve(snapshot.sourceInstances.size());
	for (const auto &src : snapshot.sourceInstances) {
		MultiviewInstance copy = src;
		copy.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
		instances_.push_back(std::move(copy));
	}
	/* Mirror the persisted detailed-logs flag into the runtime atomic, matching
	 * load_from_file. */
	amv::set_detailed_logs_enabled(global_settings_.detailedLogs);
	save();
	obs_log(LOG_INFO, "seeded scene collection '%s' with %zu instance(s) copied from '%s'",
		current_collection_.c_str(), instances_.size(), snapshot.sourceCollection.c_str());
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
