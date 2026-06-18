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

#include "amv-frontend-cache.hpp"
#include "amv-instance-core.hpp"
#include "config-manager.hpp"
#include "manager-dialog.hpp"
#include "multiview-window.hpp"
#include "signal-provider.hpp"

#include <QGuiApplication>
#include <QMainWindow>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static ConfigManager *config_manager = nullptr;
static ManagerDialog *manager_dialog = nullptr;

/* ---- Issue #10: per-instance cores + their views ----
 *
 * g_cores owns exactly one AmvInstanceCore per AMV instance (keyed by uuid).
 * The core holds the shared, stream-pulling, content-rendering state, so a
 * single m3u8/NDI/Spout source is pulled ONCE no matter how many projector
 * windows are open. g_views lists the open projector windows (views) for each
 * instance; a core with zero views but output enabled is the issue-#11
 * headless host generalized.
 *
 * Both maps are mutated only on the Qt/UI thread (open/close, config edits).
 * The graphics thread never reads them — it iterates g_output_hosts, which is
 * rebuilt under obs_enter_graphics. */
static std::map<std::string, std::unique_ptr<AmvInstanceCore>> g_cores;
static std::map<std::string, std::vector<MultiviewWindow *>> g_views;

/* Create the core for `uuid` if absent (applying persisted layout + output);
 * returns the (now-existing) core, or nullptr if there is no such instance. */
static AmvInstanceCore *ensure_core(const std::string &uuid)
{
	auto it = g_cores.find(uuid);
	if (it != g_cores.end())
		return it->second.get();

	auto core = std::make_unique<AmvInstanceCore>(config_manager, uuid);
	core->refresh_layout();
	/* Resume any persisted external output (issue #11) for this instance. */
	core->apply_output_settings();
	AmvInstanceCore *raw = core.get();
	g_cores[uuid] = std::move(core);
	return raw;
}

/* Re-number an instance's views 1..N (list order) and refresh their titles, so
 * the "| Window n" suffix stays gap-free after any open/close. */
static void refresh_view_numbers(const std::string &uuid)
{
	auto it = g_views.find(uuid);
	if (it == g_views.end())
		return;
	int n = 1;
	for (auto *v : it->second) {
		if (v)
			v->set_window_number(n++);
	}
}

/* ---- Issue #11 / issue #10: global external-output driver ----
 *
 * External output runs even with no visible projector window. A single
 * obs_add_main_rendered_callback drives every output-enabled core each frame on
 * the graphics thread. The callback is registered ONLY while at least one core
 * emits output, so instances with no output cost nothing per frame.
 *
 * We use the *rendered* callback (fires after the main canvas texture is
 * composited and obs->...->texture_rendered is set), NOT the plain main render
 * callback (which fires BEFORE the scene is rendered). PGM cells call
 * obs_render_main_texture(), which early-returns black until texture_rendered
 * is true — so the plain callback produced a black PGM in the output.
 *
 * g_output_hosts is the graphics-thread view of which cores to drive. It is
 * rebuilt from g_cores under obs_enter_graphics (which serializes against
 * on_main_rendered, since that runs inside the graphics frame), so the callback
 * never iterates a list that is changing under it, and a core removed from
 * g_cores is out of the driver before it is destroyed. */
static bool g_main_render_registered = false;
static std::vector<AmvInstanceCore *> g_output_hosts;

static void on_main_rendered(void *)
{
	for (auto *core : g_output_hosts) {
		if (!core || !core->has_output())
			continue;
		/* Idempotent within a frame via the core's tick token: if a visible
		 * view already ticked this core this frame, this no-ops; a headless
		 * core (no view, no display callback) ticks here. */
		core->tick_once_per_frame();
		core->render_output_only();
	}
}

void multiview_refresh_output_driver()
{
	obs_enter_graphics();
	g_output_hosts.clear();
	for (auto &[id, core] : g_cores) {
		if (core && core->has_output())
			g_output_hosts.push_back(core.get());
	}
	const bool need = !g_output_hosts.empty();
	obs_leave_graphics();

	if (need && !g_main_render_registered) {
		obs_add_main_rendered_callback(on_main_rendered, nullptr);
		g_main_render_registered = true;
	} else if (!need && g_main_render_registered) {
		obs_remove_main_rendered_callback(on_main_rendered, nullptr);
		g_main_render_registered = false;
	}
}

static void on_window_closed(MultiviewWindow *view, const std::string &uuid)
{
	/* The view's closeEvent already removed its display callback; here we drop
	 * it from the instance's view list, re-number the survivors, and tear the
	 * core down only if nothing references it anymore (no views, no output). */
	auto vit = g_views.find(uuid);
	if (vit != g_views.end()) {
		auto &vec = vit->second;
		vec.erase(std::remove(vec.begin(), vec.end(), view), vec.end());
		if (view)
			view->deleteLater();
		if (vec.empty())
			g_views.erase(vit);
		else
			refresh_view_numbers(uuid);
	}

	const bool hasViews = g_views.count(uuid) != 0;
	auto cit = g_cores.find(uuid);
	if (cit != g_cores.end() && !hasViews && !cit->second->has_output()) {
		/* Last view gone and no output: remove from the driver first (under
		 * the graphics lock, so no in-flight frame holds it), then destroy
		 * the core (releases sources on this main thread). The just-closed
		 * view is pending deleteLater and never dereferences core_ again. */
		std::unique_ptr<AmvInstanceCore> dying = std::move(cit->second);
		g_cores.erase(cit);
		multiview_refresh_output_driver();
		dying.reset();
	} else {
		multiview_refresh_output_driver();
	}
}

/* Reconcile an instance's headless output host with its persisted settings:
 * ensure a core exists (even with no window) while output is enabled, and tear
 * a core with no views down once output is disabled. */
static void reconcile_output_host(const std::string &uuid)
{
	MultiviewInstance *inst = config_manager ? config_manager->find_instance(uuid) : nullptr;
	const bool want = inst && inst->outputSettings.any_enabled();
	const bool hasViews = g_views.count(uuid) != 0;
	auto cit = g_cores.find(uuid);

	if (want) {
		AmvInstanceCore *core = ensure_core(uuid);
		if (core)
			core->apply_output_settings();
	} else if (cit != g_cores.end()) {
		cit->second->apply_output_settings(); /* tears output_ down */
		if (!hasViews) {
			/* No views and no output left — destroy the core (driver
			 * rebuilt first so no in-flight frame holds it). */
			std::unique_ptr<AmvInstanceCore> dying = std::move(cit->second);
			g_cores.erase(cit);
			multiview_refresh_output_driver();
			return;
		}
	}
	multiview_refresh_output_driver();
}

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
	/* Issue #10: every open creates a NEW projector window onto the instance's
	 * shared core (with N windows already open, "focus the existing one" is
	 * ambiguous). The core is created on first open and reused thereafter, so
	 * stream-pulling sources are never duplicated across windows. */
	AmvInstanceCore *core = ensure_core(uuid);
	if (!core)
		return;

	auto *window = new MultiviewWindow(config_manager, core, nullptr);
	QObject::connect(window, &MultiviewWindow::window_closed, on_window_closed);
	g_views[uuid].push_back(window);
	refresh_view_numbers(uuid);

	/* If this instance has output enabled (persisted), make sure the global
	 * driver picks the core up. */
	multiview_refresh_output_driver();
}

void notify_multiview_layout_changed(const std::string &uuid)
{
	/* Layout change rebuilds the shared core's cells ONCE, then invalidates
	 * every view's cached viewport so each recomputes at its own size. */
	auto apply = [](const std::string &id, AmvInstanceCore *core) {
		core->refresh_layout();
		auto vit = g_views.find(id);
		if (vit != g_views.end())
			for (auto *v : vit->second)
				if (v)
					v->invalidate_layout();
	};
	if (uuid.empty()) {
		for (auto &[id, core] : g_cores)
			if (core)
				apply(id, core.get());
	} else {
		auto it = g_cores.find(uuid);
		if (it != g_cores.end() && it->second)
			apply(uuid, it->second.get());
	}
}

void notify_multiview_name_changed(const std::string &uuid)
{
	/* Fan the new name out to every view's title (each keeps its window n). */
	auto vit = g_views.find(uuid);
	if (vit != g_views.end())
		for (auto *v : vit->second)
			if (v)
				v->refresh_title();
}

void notify_multiview_visual_settings_changed(const std::string &uuid)
{
	/* Visual settings rebuild shared resources on the core; cell rects are
	 * unchanged, so views need no cache invalidation (they redraw next frame).
	 * One call per core (not per view) avoids N redundant rebuilds. */
	if (uuid.empty()) {
		for (auto &[id, core] : g_cores)
			if (core)
				core->refresh_visual_settings();
	} else {
		auto it = g_cores.find(uuid);
		if (it != g_cores.end() && it->second)
			it->second->refresh_visual_settings();
	}
}

void notify_multiview_signal_settings_changed(const std::string &uuid)
{
	/* Phase 3 / M5.5: Lost Signal settings changes don't touch label/bg/
	 * overlay/VU — only the runtime resolver and any image loaders that
	 * read placeholder/signal-lost paths. refresh_signal_settings() is the
	 * narrow hook for that and avoids the heavier label/source rebuild that
	 * notify_multiview_visual_settings_changed() triggers. Once per core. */
	if (uuid.empty()) {
		for (auto &[id, core] : g_cores)
			if (core)
				core->refresh_signal_settings();
	} else {
		auto it = g_cores.find(uuid);
		if (it != g_cores.end() && it->second)
			it->second->refresh_signal_settings();
	}
}

void notify_multiview_output_settings_changed(const std::string &uuid)
{
	/* Issue #11: external-output config changed. Ensure a core exists for each
	 * instance that now has output enabled (creating a headless core if there
	 * is no open window), and tear down view-less cores whose output was
	 * disabled. reconcile_output_host() refreshes the global driver itself. */
	if (uuid.empty()) {
		/* Reconcile every config instance AND every existing core: the union
		 * covers instances that should gain a headless core and stale cores
		 * whose instance no longer has output (or was removed, e.g. after a
		 * scene-collection switch). Iterate a snapshot set — reconcile may
		 * insert into / erase from g_cores. */
		std::set<std::string> uuids;
		if (config_manager) {
			for (auto &inst : config_manager->instances())
				uuids.insert(inst.uuid);
		}
		for (auto &[id, core] : g_cores)
			uuids.insert(id);
		for (const auto &u : uuids)
			reconcile_output_host(u);
	} else {
		reconcile_output_host(uuid);
	}
}

void close_multiview_window(const std::string &uuid)
{
	/* Instance deletion: close ALL views of the instance and destroy the core.
	 * UAF-safe order: (1) disconnect view signals so deleting them doesn't
	 * re-enter on_window_closed; (2) move the core out of g_cores and rebuild
	 * the driver under the graphics lock, so no in-flight frame holds it;
	 * (3) delete the views (each removes its display callback) while the core
	 * is still alive; (4) destroy the core last (releases its sources). */
	auto vit = g_views.find(uuid);
	auto cit = g_cores.find(uuid);

	if (vit != g_views.end())
		for (auto *v : vit->second)
			if (v)
				v->disconnect();

	std::unique_ptr<AmvInstanceCore> dying;
	if (cit != g_cores.end()) {
		dying = std::move(cit->second);
		g_cores.erase(cit);
	}
	multiview_refresh_output_driver();

	if (vit != g_views.end()) {
		for (auto *v : vit->second) {
			if (v) {
				v->close();
				delete v;
			}
		}
		g_views.erase(vit);
	}

	dying.reset();
}

static void close_all_multiview_windows()
{
	/* Issue #11: stop the global output driver before destroying anything so no
	 * on_main_rendered fires against freed cores. */
	if (g_main_render_registered) {
		obs_remove_main_rendered_callback(on_main_rendered, nullptr);
		g_main_render_registered = false;
	}
	g_output_hosts.clear();

	/* Delete all views first (each removes its display callback), then destroy
	 * all cores (releases their sources). */
	for (auto &[uuid, views] : g_views) {
		for (auto *v : views) {
			if (v) {
				v->disconnect();
				v->close();
				delete v;
			}
		}
	}
	g_views.clear();
	g_cores.clear();
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
	for (auto &[id, core] : g_cores) {
		if (core)
			core->refresh_sources_lazy();
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
		for (auto &[id, core] : g_cores) {
			if (core)
				core->on_source_being_removed(source);
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
		for (auto &[id, core] : g_cores) {
			if (core)
				core->on_source_just_created(source);
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
	/* Issue #10 isolation F2: keep the frontend cache (program/preview scene +
	 * streaming output) current from the MAIN thread. The render/graphics
	 * thread reads this cache instead of calling obs_frontend_* directly, which
	 * would race OBS's main-thread scene switch (potential UAF). Frontend events
	 * are coarse (scene/preview/studio/streaming/collection/load) and infrequent,
	 * so refreshing on every event is cheap and guarantees freshness. */
	amv_frontend::refresh();

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		if (config_manager) {
			config_manager->on_scene_collection_changed();
			if (manager_dialog)
				manager_dialog->refresh_instance_list();

			/* Issue #10: the config reloaded for the new collection. Any
			 * open window whose instance no longer exists is now an orphan —
			 * close its views and destroy its core (close_multiview_window
			 * handles the UAF-safe teardown). Collect first; the close
			 * mutates g_views/g_cores. */
			std::vector<std::string> orphans;
			for (auto &[id, views] : g_views) {
				if (!config_manager->find_instance(id))
					orphans.push_back(id);
			}
			for (const auto &id : orphans)
				close_multiview_window(id);

			/* Spin up headless cores for instances with output enabled and
			 * tear down view-less cores whose instance is gone/disabled. */
			notify_multiview_output_settings_changed();
		}
	}

	/* Issue #11 Phase 2: once OBS finishes loading (scene collection + our
	 * config are in memory), start headless output for any instance that has
	 * it persisted-enabled, WITHOUT needing the user to open its window. */
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		notify_multiview_output_settings_changed();
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

	/* Release the frontend cache's cached scene/output refs (F2). Done after all
	 * windows/cores are gone, so no render pass can read it concurrently. */
	amv_frontend::shutdown();

	obs_log(LOG_INFO, "plugin unloaded");
}
