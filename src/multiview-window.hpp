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
#include <string>
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
	std::recursive_mutex source_mutex_;

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

	/* Label text sources (one per cell, may be null if label disabled) */
	struct LabelSource {
		OBSSource source; /* text_ft2_source_v2 private source */
		std::string text; /* current text for change detection */
		uint32_t color = 0xFFFFFFFF;
		int fontSize = 0;
		std::string fontFamily;
	};
	std::vector<LabelSource> label_sources_;
	void rebuild_label_sources();
	void render_label(int cellIndex, const CellRect &cell, int vpX, int vpY);

	/* Background image textures (one per cell) */
	struct BgImage {
		gs_texture_t *texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string path; /* for change detection */
	};
	std::vector<BgImage> bg_images_;
	void rebuild_bg_images();
	void release_bg_images();

	/* Foreground overlay textures (one per cell) */
	struct OverlayImage {
		gs_texture_t *texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string path; /* for change detection */
	};
	std::vector<OverlayImage> overlay_images_;
	void rebuild_overlay_images();
	void release_overlay_images();

	/* Safe area vertex buffers (normalized 0-1 coords, shared across cells) */
	gs_vertbuffer_t *safe_action_vb_ = nullptr;       /* Action Safe 3.5% */
	gs_vertbuffer_t *safe_graphics_vb_ = nullptr;     /* Graphics Safe 5.0% */
	gs_vertbuffer_t *safe_4x3_vb_ = nullptr;          /* 4:3 safe 16.25% */
	gs_vertbuffer_t *safe_center_left_vb_ = nullptr;  /* center left horizontal */
	gs_vertbuffer_t *safe_center_top_vb_ = nullptr;   /* center top vertical */
	gs_vertbuffer_t *safe_center_right_vb_ = nullptr; /* center right horizontal */
	bool safe_area_vb_init_ = false;
	void init_safe_area_vbs();
	void release_safe_area_vbs();
	void render_safe_area(int cellIndex, int vrX, int vrY, int vrW, int vrH);

	/* VU Meter per-cell audio metering */
	struct SingleVolmeter {
		obs_volmeter_t *volmeter = nullptr;
		std::string name;
		float magnitude[MAX_AUDIO_CHANNELS];
		float peak[MAX_AUDIO_CHANNELS];
		int channels = 0;
		uint64_t last_callback_ns = 0; /* timestamp of last callback */
	};
	struct CellVolmeter {
		std::vector<SingleVolmeter> meters;
		uint64_t last_update_ts = 0;
		float displayPeak = -200.0f; /* smoothed display value in dB */
		uint64_t last_render_ns = 0; /* for ballistics time delta */
	};
	std::vector<CellVolmeter *> cell_volmeters_;
	void rebuild_volmeters();
	void release_volmeters();
	void render_vu_meter(int cellIndex, const CellRect &cell, int vpX, int vpY, int sigX, int sigY, int sigW,
			     int sigH);
	static void volmeter_callback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				      const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);

	/* Scene change detection for PGM/PRVW volmeter rebuild */
	OBSWeakSource last_pgm_scene_;
	OBSWeakSource last_prvw_scene_;
	bool has_pgm_cell_ = false;
	bool has_prvw_cell_ = false;
	void check_scene_change_for_volmeters();
};

/* Global functions (defined in plugin-main) */
void open_multiview_window(const std::string &uuid);
void close_multiview_window(const std::string &uuid);
void open_manager_dialog();
void notify_multiview_layout_changed(const std::string &uuid = "");
void notify_multiview_name_changed(const std::string &uuid);
void notify_multiview_visual_settings_changed(const std::string &uuid = "");
