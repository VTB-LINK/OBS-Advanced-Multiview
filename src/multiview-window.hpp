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
 *
 * Issue #10: N views attach to one core. The core (sources / volmeters / output
 * / render logic) is owned by plugin-main's g_cores registry; the view holds a
 * NON-owning pointer to it. Closing a view never tears the core down — that
 * happens in plugin-main only when the last view is gone and no output remains.
 *
 * Several public methods are thin forwarders to the core so the context-menu
 * call sites keep working; because the core is shared, one call updates every
 * view of the instance. */
class MultiviewWindow : public QWidget {
	Q_OBJECT

public:
	MultiviewWindow(ConfigManager *config, AmvInstanceCore *core, QWidget *parent = nullptr);
	~MultiviewWindow() override;

	std::string instance_uuid() const { return uuid_; }

	/* Window numbering (issue #10): plugin-main assigns the 1-based position of
	 * this view in the instance's view list and re-assigns after any open/close
	 * so the titles stay gap-free. set_window_number() refreshes the title. */
	void set_window_number(int number);
	void refresh_title();

	/* Force this view's layout engine to recompute at its own size next frame
	 * (after a shared layout/visual change fanned out from plugin-main). */
	void invalidate_layout();

	/* ---- forwarders to the shared core (context-menu call sites) ---- */
	void refresh_sources();
	bool refresh_cell(int row, int col);
	void refresh_visual_settings();
	bool force_reconnect_cell(int cellIndex);

	QPaintEngine *paintEngine() const override { return nullptr; }

signals:
	void window_closed(MultiviewWindow *view, const std::string &uuid);

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

	/* The shared per-instance state + render logic. NON-owning: the core lives
	 * in plugin-main's g_cores and outlives / outdies this view independently
	 * (N views share one core). Never reset or delete it here. */
	AmvInstanceCore *core_ = nullptr;

	/* 1-based position of this view in its instance's view list (title only). */
	int window_number_ = 1;

	/* OBS display (per-window). */
	OBSDisplay display_;
	bool display_created_ = false;

	/* Per-window layout: the engine is recomputed at THIS window's size. */
	LayoutEngine engine_;
	int cached_vpW_ = 0;
	int cached_vpH_ = 0;

	bool is_always_on_top_ = false;
	std::atomic<bool> ready_{false};
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
