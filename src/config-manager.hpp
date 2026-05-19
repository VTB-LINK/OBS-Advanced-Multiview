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

#include <string>
#include <vector>

class ConfigManager {
public:
	static constexpr int CURRENT_CONFIG_VERSION = 1;

	ConfigManager();
	~ConfigManager();

	/* Load / save for the current scene collection */
	bool load();
	bool save();

	/* Called when scene collection changes */
	void on_scene_collection_changed();

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
	static std::string sanitize_filename(const std::string &name);
	static std::string get_current_scene_collection();

	bool save_to_file(const std::string &path);
	bool load_from_file(const std::string &path);

	std::string config_dir_;
	std::string current_collection_;

	GlobalSettings global_settings_;
	std::vector<MultiviewInstance> instances_;
	std::vector<LayoutPreset> layout_presets_;
};
