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

#include "amv-instance-core.hpp"
#include "config-manager.hpp"
#include "layout-engine.hpp"

#include <obs.hpp>

#include <QWidget>
#include <QWindow>
#include <QPointF>

#include <atomic>
#include <memory>
#include <string>

/* A projector VIEW onto one AMV instance. Holds a per-window OBS display +
 * LayoutEngine and renders the shared AmvInstanceCore at this window's size.
 * Issue #10 will allow N views per core; for now each window owns its core 1:1
 * (the core ownership moves to plugin-main in the multi-window phase).
 *
 * Most of the public methods are thin forwarders to the core so the existing
 * plugin-main / manager / context-menu call sites keep working unchanged during
 * the refactor. */
class MultiviewWindow : public QWidget {
	Q_OBJECT

public:
	MultiviewWindow(ConfigManager *config, const std::string &uuid, QWidget *parent = nullptr,
			bool startVisible = true);
	~MultiviewWindow() override;

	std::string instance_uuid() const { return uuid_; }

	/* Update window title from config (view-specific). */
	void refresh_title();

	/* ---- forwarders to the shared core ---- */
	void refresh_layout();
	void refresh_sources();
	void refresh_sources_lazy();
	bool refresh_cell(int row, int col);
	void on_source_being_removed(obs_source_t *source);
	void on_source_just_created(obs_source_t *source);
	void refresh_visual_settings();
	void refresh_signal_settings();
	bool force_reconnect_cell(int cellIndex);
	void apply_output_settings();

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* Phase 2 headless driver hooks (issue #11). */
	bool has_output() const { return core_ && core_->has_output(); }
	bool is_headless() const { return headless_.load(std::memory_order_relaxed); }
	void tick_frame();
	void render_output_only();
	void enter_headless();
	void exit_headless();

signals:
	void window_closed(const std::string &uuid);

protected:
	bool event(QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
	void create_display();
	void destroy_display();

	static void render_callback(void *data, uint32_t cx, uint32_t cy);
	void render(uint32_t cx, uint32_t cy);

	/* Context menu (view-side UI; mutators call into the core). */
	int cell_index_at_widget_pos(const QPointF &position);
	void show_context_menu(const QPoint &pos, int cellIndex);
	void on_add_source(int cellIndex);
	void on_change_source(int cellIndex);
	void on_clear_cell(int cellIndex);
	OBSSourceAutoRelease resolve_scene_cell_source_for_switch(int cellIndex);
	void handle_scene_click_switch(int cellIndex);
	void handle_scene_program_switch(int cellIndex);
	void on_edit_source(int cellIndex);
	void on_save_assignments();
	void on_edit_grid();
	void on_global_settings();
	void on_toggle_fullscreen();
	void on_toggle_always_on_top();

	ConfigManager *config_;
	std::string uuid_;

	/* The shared per-instance state + render logic. Owned 1:1 by the window for
	 * now (issue #10 Phase 1); ownership moves to plugin-main when N views per
	 * instance land. */
	std::unique_ptr<AmvInstanceCore> core_;

	/* OBS display (per-window). */
	OBSDisplay display_;
	bool display_created_ = false;

	/* Per-window layout: the engine is recomputed at THIS window's size. */
	LayoutEngine engine_;
	int cached_vpW_ = 0;
	int cached_vpH_ = 0;

	bool is_always_on_top_ = false;
	std::atomic<bool> ready_{false};
	/* Hidden, display-less render host (output without a visible projector). */
	std::atomic<bool> headless_{false};
};

/* Global functions (defined in plugin-main) */
void open_multiview_window(const std::string &uuid);
void close_multiview_window(const std::string &uuid);
void open_manager_dialog();
void open_manager_dialog_for_instance(const std::string &uuid);
void open_manager_dialog_settings();
void notify_multiview_layout_changed(const std::string &uuid = "");
void notify_multiview_name_changed(const std::string &uuid);
void notify_multiview_visual_settings_changed(const std::string &uuid = "");
void notify_multiview_signal_settings_changed(const std::string &uuid = "");
void notify_multiview_output_settings_changed(const std::string &uuid = "");
/* Re-evaluate the global external-output render driver (register/unregister the
 * main render callback, rebuild the host list). Call after any change to a
 * window's output_ state (issue #11 Phase 2). */
void multiview_refresh_output_driver();
