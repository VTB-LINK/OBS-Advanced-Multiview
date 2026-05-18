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

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <plugin-support.h>

#include "manager-dialog.hpp"

#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static ManagerDialog *manager_dialog = nullptr;

static bool init_config_path()
{
	char *path = obs_module_config_path("");
	if (!path) {
		obs_log(LOG_ERROR, "failed to get config path");
		return false;
	}

	int ret = os_mkdirs(path);
	if (ret == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "failed to create config directory: %s",
			path);
		bfree(path);
		return false;
	}

	obs_log(LOG_INFO, "config path: %s", path);
	bfree(path);
	return true;
}

static void on_tools_menu_clicked(void *)
{
	if (manager_dialog) {
		manager_dialog->show();
		manager_dialog->raise();
		manager_dialog->activateWindow();
		return;
	}

	QMainWindow *main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());

	manager_dialog = new ManagerDialog(main_window);
	manager_dialog->show();
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (manager_dialog) {
			manager_dialog->close();
			delete manager_dialog;
			manager_dialog = nullptr;
		}
	}
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)",
		PLUGIN_VERSION);

	init_config_path();

	obs_frontend_add_tools_menu_item(
		obs_module_text("OBSAdvancedMultiview"),
		on_tools_menu_clicked, nullptr);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	if (manager_dialog) {
		delete manager_dialog;
		manager_dialog = nullptr;
	}

	obs_log(LOG_INFO, "plugin unloaded");
}
