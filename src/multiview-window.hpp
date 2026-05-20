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

#include "config-manager.hpp"
#include "layout-engine.hpp"

#include <obs.hpp>

#include <QWidget>
#include <QWindow>

#include <atomic>
#include <mutex>
#include <vector>

class MultiviewWindow : public QWidget {
	Q_OBJECT

public:
	MultiviewWindow(ConfigManager *config, const std::string &uuid, QWidget *parent = nullptr);
	~MultiviewWindow() override;

	std::string instance_uuid() const { return uuid_; }

	/* Rebuild layout from config (call after layout changes) */
	void refresh_layout();

	/* Update window title from config */
	void refresh_title();

	/* Rebuild cell sources (call after cell assignment changes) */
	void refresh_sources();

	/* Recompute effective visual settings for all cells */
	void refresh_visual_settings();

	QPaintEngine *paintEngine() const override { return nullptr; }

signals:
	void window_closed(const std::string &uuid);

protected:
	bool event(QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;

private:
	void create_display();
	void destroy_display();
	void update_source_refs();
	void release_source_refs();

	static void render_callback(void *data, uint32_t cx, uint32_t cy);
	void render(uint32_t cx, uint32_t cy);

	/* Context menu */
	void show_context_menu(const QPoint &pos, int cellIndex);
	void on_add_source(int cellIndex);
	void on_change_source(int cellIndex);
	void on_clear_cell(int cellIndex);
	void on_save_assignments();
	void on_edit_grid();
	void on_global_settings();
	void on_toggle_fullscreen();
	void on_toggle_always_on_top();

	ConfigManager *config_;
	std::string uuid_;

	/* OBS display */
	OBSDisplay display_;
	bool display_created_ = false;

	/* Layout */
	LayoutEngine engine_;
	LayoutData layout_;
	int gutter_px_ = 0;

	/* Sources per cell (indexed same as engine_.cells()) */
	struct CellSource {
		std::string type;       /* "pgm", "prvw", "scene", "source", "" */
		std::string name;       /* source name for lazy re-resolution */
		OBSWeakSource weak_ref; /* cached for scene/source only */
		bool showing = false;
		bool prvw_fallback = false; /* PRVW fell back to PGM */
	};
	std::vector<CellSource> cell_sources_;
	std::mutex source_mutex_;

	/* Canvas aspect ratio for fixed-ratio viewport */
	double canvas_aspect_ = 16.0 / 9.0;

	bool is_always_on_top_ = false;
	std::atomic<bool> ready_{false};

	/* Cached viewport size to avoid recomputing layout every frame */
	int cached_vpW_ = 0;
	int cached_vpH_ = 0;

	/* Frame counter for throttled lazy re-resolution of dead sources */
	int re_resolve_counter_ = 0;

	/* Cached effective visual settings per cell (same indexing as cell_sources_) */
	std::vector<EffectiveCellVisualSettings> effective_visuals_;
};

/* Global functions (defined in plugin-main) */
void open_multiview_window(const std::string &uuid);
void close_multiview_window(const std::string &uuid);
void open_manager_dialog();
void notify_multiview_layout_changed(const std::string &uuid = "");
void notify_multiview_name_changed(const std::string &uuid);
void notify_multiview_visual_settings_changed(const std::string &uuid = "");
