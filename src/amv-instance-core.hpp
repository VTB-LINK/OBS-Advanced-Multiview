/*
OBS Advanced Multiview - per-instance render/state core (issue #10)

One AmvInstanceCore exists per AMV instance (keyed by uuid). It owns the
SHARED, stream-pulling, content-rendering state that must exist exactly once
per instance no matter how many projector windows are open:

  - the cell sources (incl. private ffmpeg/ndi/spout/vlc sources that pull
    streams — created ONCE here, never per-window, so two windows of one
    instance do not double-pull a stream),
  - per-frame snapshots (PGM/PRVW scene trees, active audio track),
  - the audio volmeters, the label/status/image render resources,
  - the external output (Spout/NDI) manager (issue #11).

`MultiviewWindow` is a thin VIEW (QWidget + OBSDisplay + its own LayoutEngine /
viewport) that holds a pointer to its core and renders by calling draw_cells()
with the cells it computed for its own size. N views attach to one core. A core
with zero views but output enabled is the issue-#11 "headless host" generalized.

All shared state is guarded by source_mutex_ (recursive). Render-path methods
run on the single OBS graphics thread (all view display callbacks + the output
driver are serialized there); UI-thread mutations (refresh_* from config edits)
contend via source_mutex_. The view list is owned by plugin-main, not here.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "config-manager.hpp"
#include "layout-engine.hpp"
#include "multiview-output.hpp"

#include <obs.hpp>

#include <QObject>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

/* Inherits QObject so deferred work can be posted with QTimer::singleShot(ms,
 * core, functor): the functor runs on the core's (UI) thread and is auto-
 * cancelled if the core is destroyed first — the lifetime safety the old
 * MultiviewWindow (a QObject) relied on. No signals/slots of its own, so no
 * Q_OBJECT macro is needed. */
class AmvInstanceCore : public QObject {
public:
	AmvInstanceCore(ConfigManager *config, const std::string &uuid);
	~AmvInstanceCore() override;

	AmvInstanceCore(const AmvInstanceCore &) = delete;
	AmvInstanceCore &operator=(const AmvInstanceCore &) = delete;

	const std::string &uuid() const { return uuid_; }

	/* Layout/config accessors a VIEW reads to feed its own LayoutEngine. */
	const LayoutData &layout() const { return layout_; }
	int gutter_px() const { return gutter_px_; }
	double canvas_aspect() const { return canvas_aspect_; }

	/* Rebuild layout + sources from config (call after layout changes). Sets
	 * layout_/gutter_px_ then refresh_sources() + refresh_visual_settings().
	 * The VIEW must invalidate its own viewport cache after calling this. */
	void refresh_layout();

	/* Rebuild cell sources (heavyweight: tears down + rebuilds all per-cell
	 * label/bg/overlay/VU resources). refresh_sources_lazy() is the light path
	 * for the source-list signal bridge (only revisits weak_refs in place). */
	void refresh_sources();
	void refresh_sources_lazy();

	/* Single-cell incremental refresh. Returns true when the single-cell path
	 * was taken; false when the caller should fall back to refresh_sources(). */
	bool refresh_cell(int row, int col);

	/* Drop any cell bound to `source` (source_remove signal handler, any
	 * thread); ClearCell cells get queued for assignment mutation on the UI
	 * thread via apply_clear_cell_for_rowcols(). */
	void on_source_being_removed(obs_source_t *source);
	void apply_clear_cell_for_rowcols(const std::vector<std::pair<int, int>> &rowCols);

	/* Re-bind a MissingInternal cell when a matching source is (re)created. */
	void on_source_just_created(obs_source_t *source);

	/* Recompute effective visual settings for all cells. */
	void refresh_visual_settings();

	/* Re-resolve Lost Signal config without rebuilding resources;
	 * force_reconnect_cell powers the Reconnect Now menu. */
	void refresh_signal_settings();
	bool force_reconnect_cell(int cellIndex);

	/* External output (issue #11). apply_output_settings() (re)builds the
	 * output manager from the instance's persisted InstanceOutputSettings. */
	void apply_output_settings();
	bool has_output() const { return output_ != nullptr; }

	/* Advance once-per-frame shared state. Idempotent within a frame via a
	 * frame token so the first of N views (or the output driver) ticks and the
	 * rest no-op. */
	void tick_once_per_frame();

	/* Paint the multiview composition for a caller-computed cell layout into the
	 * current render target's viewport. Acquires source_mutex_. (The per-cell
	 * half of the old MultiviewWindow::draw_grid.) `diag` enables the per-cell
	 * detailed-log diagnostics; only the display pass sets it, so the output
	 * pass (different cell sizes) doesn't thrash the once-per-tuple [fill] log. */
	void draw_cells(const std::vector<CellRect> &cells, int vpX, int vpY, int vpW, int vpH, bool diag = true);

	/* Offscreen output pass (no display); uses the core's own output engine. */
	void render_output_only();

	/* "[<inst-name>(<uuid8>)] " log prefix, resolved each call. */
	std::string log_prefix() const;

	/* Instance display name (for the output sender name). */
	std::string instance_display_name() const;

	/* ---- cell runtime state (Phase 3 / M5), public so the view's context
	 * menu can introspect a cell without reaching into the core internals. */
	enum class SignalRuntimeState {
		Empty,
		Resolving,
		Active,
		MissingInternal,
		Connecting,
		Lost,
		RetryScheduled,
		FallbackActive,
		Paused,
		Error,
	};

	/* Thread-safe snapshot of one cell's runtime, for the context menu. The
	 * private_source is a strong ref so it stays valid while used outside the
	 * lock (VLC/FFmpeg media controls). `valid` is false for out-of-range. */
	struct CellRuntimeSnapshot {
		bool valid = false;
		std::string type;
		SignalProviderType provider_type = SignalProviderType::Unknown;
		SignalRuntimeState state = SignalRuntimeState::Empty;
		OBSSource private_source;
		OBSWeakSource weak_ref;
	};
	CellRuntimeSnapshot snapshot_cell(int cellIndex);

private:
	/* Sources per cell (indexed same as the layout cells). */
	struct CellSource {
		std::string type;       /* "pgm", "prvw", "scene", "source", "" */
		std::string name;       /* source name for lazy re-resolution */
		OBSWeakSource weak_ref; /* cached for scene/source only */
		bool showing = false;
		bool prvw_fallback = false; /* PRVW fell back to PGM */
		bool audio_only = false;    /* direct OBS source has audio but no video output */

		SignalRuntimeState state = SignalRuntimeState::Empty;
		uint64_t last_active_ns = 0;
		uint64_t last_reconnect_ns = 0;
		int retry_attempt = 0;

		bool pending_clear = false;

		LostSignalSettings effective_lost;

		/* Phase 3 / M6 external provider runtime fields. */
		SignalProviderType provider_type = SignalProviderType::Unknown;
		OBSSource private_source;
		uint64_t last_health_ns = 0;
		uint32_t last_dimensions_w = 0;
		uint32_t last_dimensions_h = 0;
		std::string last_error_reason;
		uint64_t next_retry_ns = 0;

		SignalRuntimeState last_health_state = SignalRuntimeState::Empty;
		bool fallback_latched = false;

		/* Issue #10: per-source first-frame load timeout in ms (0 = use the
		 * built-in default). Cached from signalConfig.providerSettings on
		 * (re)resolve; the health supervisor's OPENING phase uses it to widen
		 * the first-frame wait for slow network media. */
		int first_frame_timeout_ms = 0;

		uint64_t source_created_ns = 0;
		uint64_t connecting_since_ns = 0;
		uint64_t lost_since_ns = 0;
		int media_restart_attempts = 0;
		int lost_restart_attempts = 0;

		uint64_t render_calls = 0;
		uint64_t render_skipped_no_src = 0;
		uint64_t render_skipped_zero_dim = 0;
		uint64_t last_perf_log_ns = 0;
		/* Playback position (ms) sampled at the last perf interval, for the
		 * media-progress stall check. -1 = not a timed media (live/network). */
		int64_t last_media_time_ms = -1;

		uint64_t fill_log_hash = 0;
	};

	/* Reference viewport for sizing the SHARED text/image render resources.
	 * The core has no single window size (N views of different sizes share it),
	 * so resources are built at the OBS canvas base resolution and scaled into
	 * each cell at draw time. Replaces the old per-window cached_vpW_/H_. */
	int ref_vp_width() const;
	int ref_vp_height() const;

	void update_source_refs();
	void update_source_refs_lazy();
	void release_source_refs();
	void recompute_effective_lost_locked(const std::vector<CellRect> &cells);
	SignalRuntimeState tick_external_cell_health(int cellIndex, int cellRow, int cellCol, uint64_t now_ns);
	static constexpr int kMaxLostRestartAttempts = 3;

	ConfigManager *config_ = nullptr;
	std::string uuid_;

	LayoutData layout_;
	int gutter_px_ = 0;
	double canvas_aspect_ = 16.0 / 9.0;

	std::vector<CellSource> cell_sources_;
	/* Lock order (F5, see docs/obs-isolation-review.md): the ONLY legal nesting
	 * is OBS graphics-lock -> source_mutex_ (the render path: OBS holds the
	 * graphics lock around our draw callback, which then takes source_mutex_).
	 * NEVER call obs_enter_graphics() while holding source_mutex_ — teardown/
	 * rebuild uses the four-phase pattern (collect under lock, release, do GPU
	 * work, re-lock to install). Recursive so nested core calls are safe. */
	std::recursive_mutex source_mutex_;

	int re_resolve_counter_ = 0;

	std::vector<EffectiveCellVisualSettings> effective_visuals_;

	/* Label text sources (one per cell). */
	struct LabelSource {
		OBSSource source;
		std::string text;
		uint32_t color = 0xFFFFFFFF;
		int fontSize = 0;
		std::string fontFamily;
	};
	std::vector<LabelSource> label_sources_;
	void rebuild_label_sources();
	void render_label(int cellIndex, const CellRect &cell, int vpX, int vpY);

	/* Shared status overlay text sources (one per variant, scaled per cell). */
	struct StatusTextEntry {
		OBSSource source;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string fontFamily;
	};
	enum class StatusOverlayKind {
		None,
		MissingSource,
		MissingScene,
		SignalLost,
		Reconnecting,
		Fallback,
		ProviderMissing,
		Paused,
		AudioOnly,
	};
	StatusTextEntry status_missing_source_;
	StatusTextEntry status_missing_scene_;
	StatusTextEntry status_signal_lost_;
	StatusTextEntry status_reconnecting_;
	StatusTextEntry status_fallback_;
	StatusTextEntry status_provider_missing_;
	StatusTextEntry status_paused_;
	StatusTextEntry status_audio_only_;
	void ensure_status_text_source(StatusTextEntry &entry, const char *text, const std::string &fontFamily);
	void release_status_text_sources();
	StatusOverlayKind status_overlay_kind_for_state(SignalRuntimeState state, const std::string &cellType,
							SignalProviderType providerType) const;
	void render_status_overlay(int cellIndex, int cellX, int cellY, int cellW, int cellH);

	/* dB scale label text sources (cached per unique dB value). */
	struct ScaleLabelEntry {
		int dbTenths = 0;
		std::string fontFamily;
		OBSSource source;
		uint32_t width = 0;
		uint32_t height = 0;
	};
	std::vector<ScaleLabelEntry> scale_label_cache_;
	void rebuild_scale_label_sources();
	void release_scale_label_sources();

	/* Background image textures (one per cell). */
	struct BgImage {
		gs_texture_t *texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string path;
	};
	std::vector<BgImage> bg_images_;
	void rebuild_bg_images();
	void release_bg_images();

	/* Foreground overlay textures (one per cell). */
	struct OverlayImage {
		gs_texture_t *texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string path;
	};
	std::vector<OverlayImage> overlay_images_;
	void rebuild_overlay_images();
	void release_overlay_images();

	/* Lost-Signal images (one per cell, single slot). */
	struct LostSignalImage {
		gs_texture_t *texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string path;
	};
	std::vector<LostSignalImage> lost_signal_images_;
	void rebuild_lost_signal_images();
	void release_lost_signal_images();
	void render_lost_signal_image(int cellIndex, int contentX, int contentY, int contentW, int contentH);
	std::string compute_wanted_lost_image_path(int cellIndex);

	/* Safe area vertex buffers (normalized 0-1, shared across cells). */
	gs_vertbuffer_t *safe_action_vb_ = nullptr;
	gs_vertbuffer_t *safe_graphics_vb_ = nullptr;
	gs_vertbuffer_t *safe_4x3_vb_ = nullptr;
	gs_vertbuffer_t *safe_center_left_vb_ = nullptr;
	gs_vertbuffer_t *safe_center_top_vb_ = nullptr;
	gs_vertbuffer_t *safe_center_right_vb_ = nullptr;
	bool safe_area_vb_init_ = false;
	void init_safe_area_vbs();
	void release_safe_area_vbs();
	void render_safe_area(int cellIndex, int cellX, int cellY, int cellW, int cellH, int vrX, int vrY, int vrW,
			      int vrH);

	/* VU Meter per-cell audio metering. */
	struct SingleVolmeter {
		obs_volmeter_t *volmeter = nullptr;
		std::string name;
		float magnitude[MAX_AUDIO_CHANNELS];
		float peak[MAX_AUDIO_CHANNELS];
		int channels = 0;
		uint64_t last_callback_ns = 0;

		OBSWeakSource source_weak;
		std::atomic<bool> user_muted{false};

		SingleVolmeter() = default;
		SingleVolmeter(const SingleVolmeter &) = delete;
		SingleVolmeter &operator=(const SingleVolmeter &) = delete;
	};
	struct CellVolmeter {
		CellVolmeter()
		{
			for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
				channelDisplayMagnitude[i] = -200.0f;
				channelDisplayPeak[i] = -200.0f;
				channelLastRenderNs[i] = 0;
				channelHoldPeak[i] = -200.0f;
				channelHoldSetAtNs[i] = 0;
			}
		}

		std::vector<std::unique_ptr<SingleVolmeter>> meters;
		uint64_t last_update_ts = 0;
		float displayMagnitude = -200.0f;
		float displayPeak = -200.0f;
		uint64_t last_render_ns = 0;
		float holdPeak = -200.0f;
		uint64_t holdSetAtNs = 0;
		float channelDisplayMagnitude[MAX_AUDIO_CHANNELS];
		float channelDisplayPeak[MAX_AUDIO_CHANNELS];
		uint64_t channelLastRenderNs[MAX_AUDIO_CHANNELS];
		float channelHoldPeak[MAX_AUDIO_CHANNELS];
		uint64_t channelHoldSetAtNs[MAX_AUDIO_CHANNELS];
	};
	std::vector<CellVolmeter *> cell_volmeters_;
	uint32_t current_track_bit_ = 0x1;
	uint32_t compute_active_track_bit();
	void rebuild_volmeters();
	void release_volmeters();
	void render_vu_meter(int cellIndex, const CellRect &cell, int vpX, int vpY, int sigX, int sigY, int sigW,
			     int sigH);
	static void volmeter_callback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				      const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);
	static void source_mute_callback(void *data, calldata_t *cd);
	static void source_audio_mixers_callback(void *data, calldata_t *cd);

	/* Scene change detection for PGM/PRVW volmeter rebuild. */
	OBSWeakSource last_pgm_scene_;
	OBSWeakSource last_prvw_scene_;
	bool has_pgm_cell_ = false;
	bool has_prvw_cell_ = false;
	void check_scene_change_for_volmeters();
	uint64_t last_track_poll_ns_ = 0;
	void check_active_track_change();
	uint64_t last_volmeter_rebuild_ns_ = 0;
	std::vector<void *> last_active_sources_;
	void collect_active_source_pointers(std::vector<void *> &out, uint32_t track_bit);
	std::atomic<bool> volmeters_rebuild_requested_{false};
	std::atomic<bool> lost_images_rebuild_pending_{false};

	/* PGM / PRVW cell highlight borders. */
	enum class HighlightKind { None, PgmDirect, PrvwDirect, PgmNested, PrvwNested };
	std::unordered_set<obs_source_t *> pgm_tree_set_;
	std::unordered_set<obs_source_t *> prvw_tree_set_;
	void refresh_highlight_tree_sets();
	HighlightKind compute_cell_highlight(int cellIndex);
	void render_cell_highlight(const CellRect &cell, int vpX, int vpY, HighlightKind kind,
				   const HighlightSettings &hs);

	/* External output (issue #11). */
	std::unique_ptr<MultiviewOutputManager> output_;
	InstanceOutputSettings output_settings_;
	/* Issue #10: cached global NDI readback double-buffer flag. Written on the
	 * main thread in apply_output_settings(), read on the graphics thread in
	 * render_output_only() — atomic so the graphics thread never reads
	 * main-thread-mutated GlobalSettings directly (mirrors the F2 discipline). */
	std::atomic<bool> output_ndi_double_buffer_{true};
	LayoutEngine output_engine_;
	int output_cached_vpW_ = 0;
	int output_cached_vpH_ = 0;

	/* once-per-frame tick de-dup token (graphics thread, under source_mutex_). */
	uint64_t last_tick_token_ = 0;
};
