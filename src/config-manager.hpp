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

#include "multiview-instance.hpp"

#include <set>
#include <string>
#include <vector>

class ConfigManager {
public:
	static constexpr int CURRENT_CONFIG_VERSION = 3;

	ConfigManager();
	~ConfigManager();

	/* Load / save for the current scene collection */
	bool load();
	bool save();

	/* issue #14: result of a scene-collection switch, handed to the UI layer.
	 *
	 * When the user duplicates a scene collection, OBS creates a brand-new
	 * collection (identical scenes) and switches to it, with no distinct
	 * "duplicated" event. If the just-entered collection looks like a duplicate
	 * of the one we left (freshly created + our referenced sources all resolve +
	 * the old collection had instances + the new one has no AMV config yet),
	 * `duplicateCandidate` is set and the snapshot of the SOURCE collection's
	 * config is attached so the UI can offer to copy it over. */
	struct SceneCollectionChange {
		bool duplicateCandidate = false;
		std::string sourceCollection; /* collection switched away from */
		std::string newCollection;    /* collection switched to */
		int instanceCount = 0;
		std::vector<MultiviewInstance> sourceInstances;
		GlobalSettings sourceGlobal;
		std::vector<LayoutPreset> sourcePresets;
	};

	/* Called on OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED. Saves the old
	 * collection, switches, loads the new one, and returns the change info
	 * (including any duplicate-candidate snapshot). */
	SceneCollectionChange on_scene_collection_changed();

	/* Called on OBS_FRONTEND_EVENT_SCENE_COLLECTION_RENAMED (issue #14 sister
	 * requirement). If the previous collection had a config file, rename it to
	 * the new name and reload. Returns true if a rename actually happened (the
	 * UI uses this to cancel a duplicate prompt that a rename mis-triggered on
	 * the preceding CHANGED, since CHANGED fires before RENAMED). */
	bool on_scene_collection_renamed();

	/* Copy a source-collection snapshot into the CURRENT collection: global
	 * settings + layout presets verbatim, instances with freshly generated
	 * UUIDs (they are copies, not the same instances). Persists the result. */
	void seed_current_from_snapshot(const SceneCollectionChange &snapshot);

	/* Refresh the known-collection-name snapshot used for brand-new detection.
	 * Call after OBS finishes loading and after each collection change. */
	void refresh_known_collections();

	/* Instance CRUD */
	MultiviewInstance *add_instance(const std::string &name);
	bool rename_instance(const std::string &uuid, const std::string &newName);
	MultiviewInstance *clone_instance(const std::string &uuid, const std::string &newName);
	bool delete_instance(const std::string &uuid);
	MultiviewInstance *find_instance(const std::string &uuid);
	const std::vector<MultiviewInstance> &instances() const;
	std::vector<MultiviewInstance> &instances_mutable();

	/* Global settings */
	GlobalSettings &global_settings();
	const GlobalSettings &global_settings() const;

	/* Layout presets (reserved) */
	const std::vector<LayoutPreset> &layout_presets() const;

private:
	std::string get_config_file_path() const;
	std::string config_file_path_for(const std::string &collection) const;
	static std::string sanitize_filename(const std::string &name);
	static std::string get_current_scene_collection();

	bool save_to_file(const std::string &path);
	bool load_from_file(const std::string &path);

	/* True if the CURRENTLY ACTIVE OBS scene collection is content-compatible
	 * with `instances`: at least one referenced scene/source resolves by name
	 * (a duplicate shares all names; an empty new collection shares none). An
	 * all-external setup with no internal refs is treated as compatible. */
	static bool collection_shares_content(const std::vector<MultiviewInstance> &instances);

	std::string config_dir_;
	std::string current_collection_;
	std::string previous_collection_; /* collection active before the last change */
	std::set<std::string> known_collections_;

	GlobalSettings global_settings_;
	std::vector<MultiviewInstance> instances_;
	std::vector<LayoutPreset> layout_presets_;
};
