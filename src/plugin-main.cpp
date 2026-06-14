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

#include "config-manager.hpp"
#include "manager-dialog.hpp"
#include "multiview-window.hpp"
#include "signal-provider.hpp"

#include <QGuiApplication>
#include <QMainWindow>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <atomic>
#include <map>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static ConfigManager *config_manager = nullptr;
static ManagerDialog *manager_dialog = nullptr;
static std::map<std::string, MultiviewWindow *> open_windows;

static bool init_config_path()
{
	char *path = obs_module_config_path("");
	if (!path) {
		obs_log(LOG_ERROR, "failed to get config path");
		return false;
	}

	int ret = os_mkdirs(path);
	if (ret == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "failed to create config directory: %s", path);
		bfree(path);
		return false;
	}

	obs_log(LOG_INFO, "config path: %s", path);
	bfree(path);
	return true;
}

static ManagerDialog *ensure_manager_dialog()
{
	if (manager_dialog)
		return manager_dialog;

	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	manager_dialog = new ManagerDialog(config_manager, main_window);
	return manager_dialog;
}

static void reset_manager_dialog_geometry(ManagerDialog *dialog)
{
	Qt::WindowStates state = dialog->windowState();
	state &= ~Qt::WindowMinimized;
	state &= ~Qt::WindowMaximized;
	state &= ~Qt::WindowFullScreen;
	dialog->setWindowState(state);

	QSize defaultSize = dialog->sizeHint().expandedTo(dialog->minimumSize());
	dialog->resize(defaultSize);

	QWidget *parent = dialog->parentWidget();
	QRect anchor = parent ? parent->geometry() : QRect();
	if (!anchor.isValid()) {
		QScreen *screen = dialog->screen();
		if (!screen)
			screen = QGuiApplication::primaryScreen();
		if (screen)
			anchor = screen->availableGeometry();
	}
	if (anchor.isValid()) {
		QPoint pos = anchor.center() - QPoint(defaultSize.width() / 2, defaultSize.height() / 2);
		dialog->move(pos);
	}
}

static void present_manager_dialog(ManagerDialog *dialog, bool resetGeometry)
{
	if (resetGeometry)
		reset_manager_dialog_geometry(dialog);

	if (resetGeometry || dialog->isMinimized())
		dialog->showNormal();
	else
		dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

static void on_tools_menu_clicked(void *)
{
	present_manager_dialog(ensure_manager_dialog(), false);
}

void open_manager_dialog()
{
	present_manager_dialog(ensure_manager_dialog(), true);
}

void open_manager_dialog_for_instance(const std::string &uuid)
{
	ManagerDialog *dialog = ensure_manager_dialog();
	dialog->show_instances_tab_for_uuid(uuid);
	present_manager_dialog(dialog, true);
}

void open_manager_dialog_settings()
{
	ManagerDialog *dialog = ensure_manager_dialog();
	dialog->show_settings_tab();
	present_manager_dialog(dialog, true);
}

void open_multiview_window(const std::string &uuid)
{
	/* If already open, just bring to front */
	auto it = open_windows.find(uuid);
	if (it != open_windows.end() && it->second) {
		it->second->show();
		it->second->raise();
		it->second->activateWindow();
		return;
	}

	auto *window = new MultiviewWindow(config_manager, uuid, nullptr);

	QObject::connect(window, &MultiviewWindow::window_closed, [](const std::string &closedUuid) {
		auto it = open_windows.find(closedUuid);
		if (it != open_windows.end()) {
			it->second->deleteLater();
			open_windows.erase(it);
		}
	});

	open_windows[uuid] = window;
}

void notify_multiview_layout_changed(const std::string &uuid)
{
	if (uuid.empty()) {
		/* Refresh all open windows */
		for (auto &[id, window] : open_windows) {
			if (window)
				window->refresh_layout();
		}
	} else {
		auto it = open_windows.find(uuid);
		if (it != open_windows.end() && it->second)
			it->second->refresh_layout();
	}
}

void notify_multiview_name_changed(const std::string &uuid)
{
	auto it = open_windows.find(uuid);
	if (it != open_windows.end() && it->second)
		it->second->refresh_title();
}

void notify_multiview_visual_settings_changed(const std::string &uuid)
{
	if (uuid.empty()) {
		for (auto &[id, window] : open_windows) {
			if (window)
				window->refresh_visual_settings();
		}
	} else {
		auto it = open_windows.find(uuid);
		if (it != open_windows.end() && it->second)
			it->second->refresh_visual_settings();
	}
}

void notify_multiview_signal_settings_changed(const std::string &uuid)
{
	/* Phase 3 / M5.5: Lost Signal settings changes don't touch label/bg/
	 * overlay/VU — only the runtime resolver and any image loaders that
	 * read placeholder/signal-lost paths. refresh_signal_settings() is the
	 * narrow hook for that and avoids the heavier label/source rebuild that
	 * notify_multiview_visual_settings_changed() triggers. */
	if (uuid.empty()) {
		for (auto &[id, window] : open_windows) {
			if (window)
				window->refresh_signal_settings();
		}
	} else {
		auto it = open_windows.find(uuid);
		if (it != open_windows.end() && it->second)
			it->second->refresh_signal_settings();
	}
}

void close_multiview_window(const std::string &uuid)
{
	auto it = open_windows.find(uuid);
	if (it != open_windows.end() && it->second) {
		it->second->disconnect();
		it->second->close();
		delete it->second;
		open_windows.erase(it);
	}
}

static void close_all_multiview_windows()
{
	for (auto &[uuid, window] : open_windows) {
		if (window) {
			/* Disconnect signal to avoid modifying map during iteration */
			window->disconnect();
			window->close();
			delete window;
		}
	}
	open_windows.clear();
}

/* Phase 3 / M5: bridge OBS core source-list signals to all open MultiviewWindows.
 *
 * The render thread re-resolves cell sources every frame, but volmeters and the
 * `cell_sources_[i].state` snapshot are only refreshed when refresh_sources()
 * is called. Without these signals a deleted scene/source kept its volmeter
 * attached until the user happened to switch scene; an undo'd source needed up
 * to one second (the active-source poll) before VU re-attached.
 *
 * Signals fire on arbitrary OBS threads, so we MUST NOT touch any QWidget
 * directly here. We coalesce a request flag and post a lambda onto the Qt
 * main thread via QTimer::singleShot — idiomatic Qt cross-thread dispatch
 * already used by other OBS plugins (e.g. obs-websocket, DistroAV). */
static std::atomic<bool> source_list_dirty_{false};

static void on_qt_refresh_sources_all()
{
	if (!source_list_dirty_.exchange(false, std::memory_order_acquire))
		return;
	for (auto &[id, window] : open_windows) {
		if (window)
			window->refresh_sources_lazy();
	}
}

static void schedule_refresh_sources_all()
{
	/* Only schedule once per coalesce window. Another signal can fire while
	 * the timer is pending; the next refresh will pick up the cumulative
	 * change because refresh_sources_lazy() is idempotent.
	 *
	 * NOTE: we deliberately use the lazy variant here (and NOT the heavy
	 * refresh_sources() that release/rebuilds label_sources_ / bg_images_
	 * / overlay_images_ / volmeters_). The heavy path leaves cell_sources_
	 * empty for several frames, which makes any cell's MISSING SOURCE /
	 * FALLBACK overlay flicker every time an unrelated source elsewhere
	 * is added or destroyed. The lazy path only updates the cells whose
	 * assignment actually changed and lets the render-thread lazy resolve
	 * detect destroyed sources via OBSGetStrongRef() returning null. */
	if (source_list_dirty_.exchange(true, std::memory_order_acq_rel))
		return;

	QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QObject *ctx = main_window ? static_cast<QObject *>(main_window) : nullptr;
	/* 50 ms debounce — absorbs bursts from operations like "delete scene"
	 * which fire source_remove + source_destroy back-to-back, plus child
	 * sceneitem teardown. Smaller intervals risk hammering refresh_sources()
	 * from each child source destroy. */
	QTimer::singleShot(50, ctx, on_qt_refresh_sources_all);
}

static void on_obs_source_signal(void *, calldata_t *)
{
	schedule_refresh_sources_all();
}

/* Phase 3 / M5.4 hardening: precise source_remove handler.
 *
 * The generic on_obs_source_signal path schedules a 50ms-debounced lazy
 * refresh — perfect for source_create / source_destroy / source_rename, but
 * source_remove has a much narrower deadline. Between source_remove and
 * source_destroy OBS may keep the source alive (and reachable via our weak
 * refs) for an arbitrary number of frames if the undo stack still references
 * it, while internally pruning sceneitems. During that window:
 *   - obs_source_video_render() can recurse into scene_video_render →
 *     update_transforms_and_prune_sources, which fires sceneitem signals
 *     into other plugins; streamdeck-plugin-obs has been observed to crash
 *     here when the host sceneitem was removed mid-render.
 *   - volmeters left attached to the doomed source's audio chain end up
 *     wrestling with the audio thread when OBS finally destroys it.
 *
 * Calling MultiviewWindow::on_source_being_removed() synchronously here
 * drops the binding before anything else has a chance to render through it.
 * We still kick a lazy refresh afterwards so any cell whose effective Lost
 * Signal fallback now needs to switch picks up the new state on the next
 * 50ms tick. The signal fires on whichever thread invoked obs_source_remove
 * (often the Qt main thread for UI-driven removals), and on_source_being_
 * removed acquires source_mutex_ — short, bounded, deadlock-free. */
static void on_obs_source_remove_precise(void *, calldata_t *cd)
{
	obs_source_t *source = nullptr;
	calldata_get_ptr(cd, "source", &source);
	if (source) {
		for (auto &[id, window] : open_windows) {
			if (window)
				window->on_source_being_removed(source);
		}
	}
	schedule_refresh_sources_all();
}

/* Phase 3 / M5.4 hardening: precise source_create handler.
 *
 * Mirror of on_obs_source_remove_precise. Without this the user-visible
 * recovery path (Edit → Undo Delete, or any plugin/script re-creating a
 * source with the bound name) waited up to 50 ms for the debounced lazy
 * refresh to run. The lazy refresh is still useful for source_destroy /
 * source_rename storms, but for source_create alone we have a precise
 * source pointer in calldata and can rebind synchronously.
 *
 * Cells whose state is *not* MissingInternal are left alone — we never
 * forcibly rebind a cell whose existing binding still works, even if the
 * names happen to collide (OBS allows duplicate display names through
 * separate scene collections / nested groups; identity is the source
 * pointer, not the name). */
static void on_obs_source_create_precise(void *, calldata_t *cd)
{
	obs_source_t *source = nullptr;
	calldata_get_ptr(cd, "source", &source);
	if (source) {
		for (auto &[id, window] : open_windows) {
			if (window)
				window->on_source_just_created(source);
		}
	}
	/* Still schedule the debounced refresh as a safety net for any state
	 * the precise path doesn't cover (e.g. a cell that just transitioned
	 * to MissingInternal in this very frame and hasn't been observed by
	 * the render loop yet). The lazy refresh is idempotent. */
	schedule_refresh_sources_all();
}

static void register_source_list_signals()
{
	signal_handler_t *sh = obs_get_signal_handler();
	if (!sh)
		return;
	signal_handler_connect(sh, "source_create", on_obs_source_create_precise, nullptr);
	signal_handler_connect(sh, "source_remove", on_obs_source_remove_precise, nullptr);
	signal_handler_connect(sh, "source_destroy", on_obs_source_signal, nullptr);
	signal_handler_connect(sh, "source_rename", on_obs_source_signal, nullptr);
}

static void unregister_source_list_signals()
{
	signal_handler_t *sh = obs_get_signal_handler();
	if (!sh)
		return;
	signal_handler_disconnect(sh, "source_create", on_obs_source_create_precise, nullptr);
	signal_handler_disconnect(sh, "source_remove", on_obs_source_remove_precise, nullptr);
	signal_handler_disconnect(sh, "source_destroy", on_obs_source_signal, nullptr);
	signal_handler_disconnect(sh, "source_rename", on_obs_source_signal, nullptr);
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		if (config_manager) {
			config_manager->on_scene_collection_changed();
			if (manager_dialog)
				manager_dialog->refresh_instance_list();
		}
	}

	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (config_manager)
			config_manager->save();

		close_all_multiview_windows();

		if (manager_dialog) {
			manager_dialog->close();
			delete manager_dialog;
			manager_dialog = nullptr;
		}
	}
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

	init_config_path();

	config_manager = new ConfigManager();
	config_manager->load();

	/* Phase 3 / M6: bring up the signal provider registry before any UI
	 * surface that might query it (manager dialog, source picker, future
	 * external provider implementations). External providers register
	 * themselves from their own translation units; the registry stays
	 * usable with only the internal adapters until those land. */
	signal_provider_registry_init();

	obs_frontend_add_tools_menu_item(obs_module_text("OBSAdvancedMultiview"), on_tools_menu_clicked, nullptr);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	register_source_list_signals();

	return true;
}

void obs_module_unload(void)
{
	unregister_source_list_signals();
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	close_all_multiview_windows();

	if (manager_dialog) {
		delete manager_dialog;
		manager_dialog = nullptr;
	}

	if (config_manager) {
		delete config_manager;
		config_manager = nullptr;
	}

	signal_provider_registry_shutdown();

	obs_log(LOG_INFO, "plugin unloaded");
}
