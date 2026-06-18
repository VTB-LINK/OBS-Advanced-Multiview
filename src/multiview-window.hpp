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
#include "multiview-output.hpp"

#include <obs.hpp>

#include <QWidget>
#include <QWindow>
#include <QPointF>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class MultiviewWindow : public QWidget {
	Q_OBJECT

public:
	/* startVisible=false builds a hidden, display-less "render host" used to
	 * drive external output without a visible projector window (issue #11
	 * Phase 2). Such a host keeps all cell state live and is driven by the
	 * global obs_add_main_render_callback. */
	MultiviewWindow(ConfigManager *config, const std::string &uuid, QWidget *parent = nullptr,
			bool startVisible = true);
	~MultiviewWindow() override;

	std::string instance_uuid() const { return uuid_; }

	/* Rebuild layout from config (call after layout changes) */
	void refresh_layout();

	/* Update window title from config */
	void refresh_title();

	/* Rebuild cell sources (call after cell assignment changes).
	 *
	 * `refresh_sources()` is the heavyweight path: it tears down all
	 * per-cell vectors (label/bg/overlay text sources, VU meters) and
	 * rebuilds them. Use it for layout-shape changes and cell-assignment
	 * edits.
	 *
	 * `refresh_sources_lazy()` is the lightweight path used by the
	 * source-list signal bridge (source_create / remove / destroy /
	 * rename). It only revisits per-cell `CellSource` weak_refs and
	 * runtime state in place, so unrelated cells keep their label/VU
	 * resources and their MISSING SOURCE / FALLBACK overlays do not
	 * flicker for the few frames it would take a full rebuild. If the
	 * cell *count* itself changed (layout/grid edit), the lazy path
	 * bails and the caller is expected to schedule a full refresh. */
	void refresh_sources();
	void refresh_sources_lazy();

	/* Phase 3 / M6.1+ task 9.1.A: single-cell incremental refresh.
	 *
	 * `refresh_cell(row, col)` does the same thing refresh_sources()
	 * does — re-resolve assignment, re-bind weak ref / re-create
	 * external private source, refresh effective settings — but ONLY
	 * for the one cell at (row, col). Other cells in the same window
	 * keep their runtime untouched.
	 *
	 * Constraints:
	 *   - cell_count must NOT have changed (layout / Edit Grid still
	 *     goes through refresh_sources() because parallel-vector
	 *     resizing is unsafe to interleave with single-slot in-place
	 *     edits). The function bails and returns false if it detects
	 *     a count mismatch; the caller is expected to fall back to
	 *     refresh_sources() in that case.
	 *   - External private-source release / create must run OUTSIDE
	 *     source_mutex_ (lock-order rule from docs/ROADMAP.md §6).
	 *   - Volmeter rebuild uses the existing throttle flag —
	 *     check_active_track_change() coalesces it on the next render
	 *     frame, no per-cell volmeter API.
	 *
	 * Returns true when the single-cell path was taken; false when the
	 * caller should fall back to refresh_sources(). */
	bool refresh_cell(int row, int col);

	/* Phase 3 / M5.4 hardening: invoked synchronously from the OBS
	 * `source_remove` signal handler with the source pointer extracted via
	 * calldata. We immediately drop any cell whose cached strong/weak ref
	 * resolves to this source: state → MissingInternal, weak_ref cleared,
	 * `showing` decremented in pair with the original inc_showing. This
	 * shortens the race window where another frame could still call
	 * obs_source_video_render on a source whose sceneitems are about to be
	 * pruned by OBS — pruning fires `item_remove` signals into other
	 * plugins, and at least one of them (streamdeck-plugin-obs) has been
	 * observed to crash when the pruning happens mid-render.
	 *
	 * Safe to call from any thread: only touches `cell_sources_` under
	 * `source_mutex_` and uses obs_source_dec_showing on a source that
	 * libobs guarantees stays alive for the duration of the signal. */
	void on_source_being_removed(obs_source_t *source);

	/* Phase 3 / M5.1 ClearCell: invoked from on_source_being_removed when a
	 * cell with InternalMissingBehavior::ClearCell just lost its source.
	 * Mutates the instance assignment list and persists, mirroring what the
	 * right-click "Clear Cell" menu does — but driven by the runtime, not a
	 * user click. The list of (row, col) pairs is collected synchronously
	 * under source_mutex_ and then queued onto the Qt main thread because
	 * source_remove can fire on any thread and config persistence must
	 * happen on the UI thread. */
	void apply_clear_cell_for_rowcols(const std::vector<std::pair<int, int>> &rowCols);

	/* Phase 3 / M5.4 hardening: invoked synchronously from the OBS
	 * `source_create` signal handler. If the new source's name matches a
	 * cell that is currently in MissingInternal (e.g. user just clicked
	 * Edit → Undo Delete on a scene we were bound to), we re-bind right
	 * away rather than waiting for the 50ms debounce on the lazy refresh.
	 * Cells whose state is already Active (or any non-Missing variant)
	 * are not touched — re-binding a cell whose original source still
	 * resolves would change render identity unexpectedly. */
	void on_source_just_created(obs_source_t *source);

	/* Recompute effective visual settings for all cells */
	void refresh_visual_settings();

	/* Phase 3 / M5: re-resolve cell signal state.
	 *
	 * `refresh_signal_settings()` is the no-op-cheap counterpart of
	 * `refresh_visual_settings()` for Lost Signal config changes — it does
	 * not rebuild label/bg/overlay/VU sources but does kick the runtime so
	 * a freshly persisted strategy (image path, fallback assignment, etc.)
	 * applies on the very next frame.
	 *
	 * `force_reconnect_cell()` powers the `Reconnect Now` menu (M5.3): it
	 * forces a name re-resolve for internal sources and is M6-ready for
	 * external private-source rebuild. Returns true when a reconnect was
	 * actually attempted; returns false if the cooldown is still active or
	 * the cell has nothing to reconnect (Empty / out of range). */
	void refresh_signal_settings();
	bool force_reconnect_cell(int cellIndex);

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* Spout/NDI output (issue #11). apply_output_settings() (re)builds the
	 * output manager from the instance's persisted InstanceOutputSettings —
	 * call after the config changes (settings dialog, right-click toggle,
	 * notify). set_spout_output_enabled() is the right-click quick toggle:
	 * it flips spout.enabled in config, saves, then applies. The sender name
	 * follows the instance name. */
	void apply_output_settings();
	void set_spout_output_enabled(bool enabled);
	bool spout_output_enabled() const;

	/* Phase 2 headless driver hooks (issue #11). Called by the global
	 * obs_add_main_render_callback on the graphics thread:
	 *   has_output()        — does this host currently emit any output?
	 *   is_headless()       — true when there is no visible window/display,
	 *                         so the global driver must tick this host itself.
	 *   tick_frame()        — advance once-per-frame state (re-resolve, scene
	 *                         change, highlight trees, audio track). A visible
	 *                         window ticks via its own display render(); a
	 *                         headless host is ticked by the global driver.
	 *   render_output_only()— the offscreen output pass (no display).
	 *   enter_headless()/exit_headless() — switch a host between
	 *                         visible+display and hidden+display-less while
	 *                         keeping cell state + output_ alive. */
	bool has_output() const { return output_ != nullptr; }
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
	void update_source_refs();
	void update_source_refs_lazy();
	void release_source_refs();

	/* Phase 3 / M5: recompute the cached effective LostSignalSettings for
	 * every cell from Global + per-cell Override. Caller must already hold
	 * source_mutex_. The `cells` argument is the layout snapshot used by
	 * the caller — pass the same vector cell_sources_ was sized against
	 * so per-cell row/col lookups stay aligned with cell_sources_ indices. */
	void recompute_effective_lost_locked(const std::vector<CellRect> &cells);

	static void render_callback(void *data, uint32_t cx, uint32_t cy);
	void render(uint32_t cx, uint32_t cy);

	/* Draw the full multiview composition (gutter, cells, overlays, VU,
	 * highlights) into the current render target, mapped to the given
	 * viewport rect. Split out of render() so the same pipeline can paint
	 * both the on-screen display and an offscreen target for Spout/NDI
	 * output (issue #11). Acquires source_mutex_ (recursive). */
	void draw_grid(int vpX, int vpY, int vpW, int vpH);

	/* Output pipeline (issue #11). Display always renders natively; the
	 * output is a separate offscreen pass driven from the persisted
	 * InstanceOutputSettings. output_ is created only while output is enabled
	 * (so an instance with no output costs nothing); the manager renders one
	 * texrender per unique resolution and dispatches to each enabled backend. */
	std::unique_ptr<MultiviewOutputManager> output_;
	InstanceOutputSettings output_settings_; /* cached copy, updated by apply_output_settings() */
	std::string instance_display_name() const;

	/* Context menu */
	int cell_index_at_widget_pos(const QPointF &position);
	void show_context_menu(const QPoint &pos, int cellIndex);
	void on_add_source(int cellIndex);
	void on_change_source(int cellIndex);
	void on_clear_cell(int cellIndex);
	OBSSourceAutoRelease resolve_scene_cell_source_for_switch(int cellIndex);

	/* Left-click scene switching: when the resolved (instance ⇐ global)
	 * SceneClickSwitchSettings.enabled is true and the clicked cell is a
	 * scene assignment, route the scene to Preview (Studio Mode on) or
	 * Program (Studio Mode off). Non-scene cells are silently ignored.
	 * Mirrors the OBS built-in multiview projector left-click. */
	void handle_scene_click_switch(int cellIndex);
	void handle_scene_program_switch(int cellIndex);

	/* Phase 3 / M6.1+ task 9.1.C: Edit Source for external-provider
	 * cells. Opens a provider-specific form populated from the cell's
	 * current SignalConfig; on Save writes the new config back into
	 * the assignment and runs refresh_cell so other cells stay live. */
	void on_edit_source(int cellIndex);
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

	/* Phase 3 / M5: cell signal runtime state.
	 *
	 * Wraps the existing PGM/PRVW/Scene/Source flow so future work (Signal
	 * Lost overlay, Reconnect Now, fallback, M6 external providers) can
	 * branch on a single state value instead of scattering string-type
	 * checks throughout render(). M5.1a only populates the state; the
	 * Phase 2 visual behavior is intentionally preserved.
	 *
	 * State semantics (see [docs/phase-3-signal-lost-and-external-sources-design.md] §6):
	 *   Empty           : cell has no assignment.
	 *   Resolving       : reserved for async resolution paths (M6).
	 *   Active          : cell currently has a usable source for render.
	 *   MissingInternal : assignment exists but obs_get_source_by_name() fails.
	 *   Connecting      : reserved for external providers (M6).
	 *   Lost            : reserved for external providers (M6).
	 *   RetryScheduled  : reserved for backoff timer integration (M5.3+).
	 *   FallbackActive  : reserved for fallback engagement (M5.4).
	 *   Paused          : user-paused media playback (M6.6); not an error,
	 *                     just suppresses SIGNAL LOST overlay and shows
	 *                     a softer "PAUSED" hint.
	 *   Error           : provider/source unavailable; non-recoverable. */
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

	/* Sources per cell (indexed same as engine_.cells()) */
	struct CellSource {
		std::string type;       /* "pgm", "prvw", "scene", "source", "" */
		std::string name;       /* source name for lazy re-resolution */
		OBSWeakSource weak_ref; /* cached for scene/source only */
		bool showing = false;
		bool prvw_fallback = false; /* PRVW fell back to PGM */
		bool audio_only = false;    /* direct OBS source has audio output but no video output */

		/* Phase 3 / M5 runtime state. Read on render thread, mutated under
		 * source_mutex_ on UI thread (refresh_sources) and render thread
		 * (lazy re-resolve / per-frame health check). recursive_mutex
		 * already protects vector-level access, so plain field reads are
		 * safe within the same lock scope. */
		SignalRuntimeState state = SignalRuntimeState::Empty;
		uint64_t last_active_ns = 0;    /* set when render produced a non-empty frame */
		uint64_t last_reconnect_ns = 0; /* set on manual / scheduled rebuild attempt */
		int retry_attempt = 0;          /* backoff counter, reset on Active */

		/* Phase 3 / M5.1: ClearCell decision is made on the source_remove
		 * signal handler (any thread) but the actual assignment mutation +
		 * config save runs on the Qt main thread via QTimer::singleShot.
		 * In between, the render thread would normally show MISSING SOURCE
		 * for the few frames it takes the timer to run. This flag bridges
		 * that gap: render treats the cell as Empty (gutter colour, no
		 * overlay) until the queued mutation calls refresh_sources() and
		 * rebuilds cell_sources_ — which naturally drops the flag. */
		bool pending_clear = false;

		/* Phase 3 / M5: cached effective Lost Signal settings (Global +
		 * per-cell Override merged once per source-refresh, not per
		 * frame). Render uses this to decide what to draw when the
		 * primary source is missing — fallback to PGM/PRVW/Scene/Source
		 * is implemented in M5.4; placeholder/signal-lost image rendering
		 * is wired separately when that bg-image-style loader lands. */
		LostSignalSettings effective_lost;

		/* Phase 3 / M6: external provider runtime fields.
		 *
		 * For internal cells (`type` ∈ {pgm, prvw, scene, source, ""}) all
		 * of these stay default-constructed and the existing M5 paths
		 * (weak_ref + lazy re-resolve) handle rendering exactly as before.
		 *
		 * External cells own a `private_source` strong ref backed by an
		 * OBS source type the host plugin contributes (`ffmpeg_source` /
		 * `ndi_source` / `spout_capture` / `vlc_source`). The provider
		 * registry creates / updates / releases this through one helper
		 * (release_cell_runtime_locked) so layout shrink, ClearCell,
		 * source change, provider recreate, window close and OBS exit
		 * use the same teardown path. */
		SignalProviderType provider_type = SignalProviderType::Unknown;
		OBSSource private_source;    /* external private OBS source */
		uint64_t last_health_ns = 0; /* monotonic time of last health observation */
		uint32_t last_dimensions_w = 0;
		uint32_t last_dimensions_h = 0;
		std::string last_error_reason; /* one-shot human-readable reason */
		uint64_t next_retry_ns = 0;    /* monotonic time of next allowed reconnect attempt */

		/* Phase 3 / M6.6: snapshot of the supervisor's last health
		 * verdict, kept separate from the public `state` field above.
		 * `state` is the *display* state and may be overridden to
		 * FallbackActive when the user configured RetryWithFallback
		 * and the cell is currently painting a fallback source. The
		 * fallback substitution decision must consult the supervisor's
		 * raw verdict (not the display state) to avoid a feedback
		 * loop where FallbackActive itself triggers another fallback
		 * substitution next frame, hiding the supervisor's eventual
		 * Active recovery. */
		SignalRuntimeState last_health_state = SignalRuntimeState::Empty;

		/* Phase 3 / M6.6: sticky "fallback engaged" latch. Set true the
		 * first frame the supervisor verdict goes unhealthy
		 * (Lost / Error / RetryScheduled). Cleared only when the
		 * supervisor next returns Active. Used to keep the user-
		 * configured fallback (PGM / Scene / Source / static image)
		 * painted continuously across recreate cycles, avoiding the
		 * "fallback flashes off for one frame each retry" effect when
		 * the freshly-recreated source momentarily reports Connecting
		 * before the next probe escalates back to Lost. The user's
		 * stated requirement: only stop showing the fallback once the
		 * cell is truly back online. */
		bool fallback_latched = false;

		/* Phase 3 / M6 step 10: external health supervisor bookkeeping.
		 *
		 * The supervisor needs three additional per-cell timestamps to
		 * drive the Connecting / Lost / Error state machine across all
		 * external providers (FFmpeg / NDI / Spout / VLC / ...). These
		 * are intentionally provider-agnostic — supervisor reads them
		 * via tick_external_cell_health() regardless of the underlying
		 * source type. NDI / Spout will populate them through the same
		 * generic supervisor without needing per-provider runtime
		 * state. */
		uint64_t source_created_ns = 0;   /* when current private_source was created */
		uint64_t connecting_since_ns = 0; /* start of current Opening/Connecting phase */
		uint64_t lost_since_ns = 0;       /* start of current Lost phase */
		int media_restart_attempts = 0; /* obs_source_media_restart attempts during Opening since last Active */
		/* Phase 3 hardening tail: Lost-branch restart-then-recreate.
		 * Tracked separately from media_restart_attempts because the
		 * Opening counter has different escalation semantics (it caps
		 * at kMaxMediaRestartAttempts then promotes Opening -> Lost),
		 * while this counter caps the cheap retries within a single
		 * Lost window before falling back to full source recreate.
		 * Reset to 0 on Active and on cell install (refresh_cell /
		 * update_source_refs); reset to 0 after a recreate is queued
		 * so the next Lost window starts again with the cheap path. */
		int lost_restart_attempts = 0;

		/* Phase 3 / M6.1 perf diag: counters used by the optional
		 * 5-second perf-stats log line. We track render-thread
		 * activity to correlate user-visible stutter with what the
		 * supervisor sees:
		 *   render_calls      -> times this cell ran obs_source_video_render
		 *   render_skipped_*  -> reasons we elected to NOT render this frame
		 *   last_perf_log_ns  -> 5s rate-limit timer */
		uint64_t render_calls = 0;
		uint64_t render_skipped_no_src = 0;
		uint64_t render_skipped_zero_dim = 0;
		uint64_t last_perf_log_ns = 0;

		/* Phase 3 / M6.6 fill diag: one-shot log per (cell, source-dim,
		 * cell-dim) combination so we can diagnose pillarbox /
		 * letterbox surprises without spamming the OBS log. The hash
		 * mixes srcW / srcH / cellW / cellH so resizing the window
		 * or the source switching resolution emits exactly one fresh
		 * line. */
		uint64_t fill_log_hash = 0;
	};
	std::vector<CellSource> cell_sources_;
	std::recursive_mutex source_mutex_;

	/* Phase 3 / M6 step 10: external-cell health supervisor.
	 *
	 * Declared here (after CellSource / SignalRuntimeState are defined)
	 * because both are nested types and the signature needs them in
	 * scope. Called from render() once per cell per second (throttled
	 * via cs.last_health_ns) for every cell whose provider is external.
	 *
	 * Fully provider-agnostic: consumes the provider's HealthReport via
	 * ISignalProvider::probe_health() and drives transitions between
	 * Connecting / Active / Lost / Error (plus media_restart and
	 * full-recreate actions). NDI / Spout / future providers do not
	 * need their own supervisor code \u2014 they implement probe_health()
	 * + supports_media_restart() + benefits_from_recreate() and this
	 * function handles the rest.
	 *
	 * Returns the SignalRuntimeState the caller should write into
	 * cs.state. Side-effects: obs_source_media_restart is dispatched
	 * inline (safe under source_mutex_); full recreate is queued onto
	 * the Qt main thread via QTimer because refresh_cell() must not run
	 * with source_mutex_ held.
	 *
	 * Caller holds source_mutex_. cellRow/cellCol come from the layout
	 * engine snapshot the caller already has, so the queued refresh_cell
	 * call does not need to re-walk the layout. */
	SignalRuntimeState tick_external_cell_health(int cellIndex, int cellRow, int cellCol, uint64_t now_ns);

	/* Phase 3 hardening tail: max cheap-restart attempts per Lost window
	 * (shared by the supervisor in multiview-window-health.cpp and the
	 * manual Reconnect Now path in multiview-window.cpp). Same value
	 * means manual clicks escalate to full recreate on the same N as
	 * the automatic supervisor. */
	static constexpr int kMaxLostRestartAttempts = 3;

	/* Canvas aspect ratio for fixed-ratio viewport */
	double canvas_aspect_ = 16.0 / 9.0;

	bool is_always_on_top_ = false;
	std::atomic<bool> ready_{false};
	/* Issue #11 Phase 2: true when this is a hidden, display-less render host
	 * (output runs without a visible window). Read on the graphics thread by
	 * the global output driver, written on the UI thread. */
	std::atomic<bool> headless_{false};

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

	/* Phase 3 / M5: shared status overlay text sources.
	 *
	 * One private text source per status text variant ("Missing Source",
	 * "Signal Lost", etc.). They're created lazily on first use and
	 * shared across all cells of this window — sized once at high font
	 * resolution and scaled DOWN to each cell, mirroring the label
	 * source pattern to avoid blurry upscaling.
	 *
	 * `release_status_text_sources()` is invoked from the same teardown
	 * paths as label_sources_ so the source lifetime is bounded by the
	 * MultiviewWindow. */
	struct StatusTextEntry {
		OBSSource source;   /* private text_gdiplus / text_ft2_source_v2 */
		uint32_t width = 0; /* cached after first non-zero query */
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
	StatusTextEntry status_signal_lost_;      /* M6 */
	StatusTextEntry status_reconnecting_;     /* M5.3 / M6 */
	StatusTextEntry status_fallback_;         /* M5.4 */
	StatusTextEntry status_provider_missing_; /* M6.2 host-plugin missing */
	StatusTextEntry status_paused_;           /* M6.6 user-paused media */
	StatusTextEntry status_audio_only_;       /* direct OBS audio input/output capture */
	void ensure_status_text_source(StatusTextEntry &entry, const char *text, const std::string &fontFamily);
	void release_status_text_sources();
	StatusOverlayKind status_overlay_kind_for_state(SignalRuntimeState state, const std::string &cellType,
							SignalProviderType providerType) const;
	void render_status_overlay(int cellIndex, int cellX, int cellY, int cellW, int cellH);

	/* dB scale label text sources (cached per unique dB value) */
	struct ScaleLabelEntry {
		int dbTenths = 0; /* dB * 10, used as key */
		std::string fontFamily;
		OBSSource source;
		uint32_t width = 0;
		uint32_t height = 0;
	};
	std::vector<ScaleLabelEntry> scale_label_cache_;
	void rebuild_scale_label_sources();
	void release_scale_label_sources();

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

	/* Phase 3 / M5.4: Lost-Signal images (one per cell, single slot).
	 *
	 * Each cell uses at most one of these images at a time:
	 *   - placeholder image  (InternalMissingBehavior::PlaceholderImage)
	 *   - fallback static image (LostSignalSettings.fallbackType == "image")
	 *   - signal lost image  (M6: ExternalLostBehavior::SignalLostImage)
	 *
	 * The "wanted path" is computed per cell from `cs.state` and
	 * `cs.effective_lost`, so a single slot can switch which strategy is
	 * displayed when the runtime state flips (e.g. internal missing →
	 * recovers → external lost). The structure mirrors BgImage exactly so
	 * the four-stage load pattern (snapshot intentions → disk IO outside
	 * lock → graphics ops → install under lock) can be reused verbatim. */
	struct LostSignalImage {
		gs_texture_t *texture = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string path; /* for change detection; "" means no image */
	};
	std::vector<LostSignalImage> lost_signal_images_;
	void rebuild_lost_signal_images();
	void release_lost_signal_images();
	void render_lost_signal_image(int cellIndex, int contentX, int contentY, int contentW, int contentH);
	std::string compute_wanted_lost_image_path(int cellIndex);

	/* Phase 3 / M6.6: log helper. Returns "[<inst-name>(<uuid8>)] " so
	 * obs_log lines can be matched back to the originating multiview
	 * instance even when several windows are open. Resolved each call so
	 * instance renames take effect immediately. Cheap: only invoked from
	 * log paths that already throttle (perf 5s, fill once-per-tuple,
	 * health on transitions). */
	std::string log_prefix() const;

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
	void render_safe_area(int cellIndex, int cellX, int cellY, int cellW, int cellH, int vrX, int vrY, int vrW,
			      int vrH);

	/* VU Meter per-cell audio metering */
	struct SingleVolmeter {
		obs_volmeter_t *volmeter = nullptr;
		std::string name;
		float magnitude[MAX_AUDIO_CHANNELS];
		float peak[MAX_AUDIO_CHANNELS];
		int channels = 0;
		uint64_t last_callback_ns = 0; /* timestamp of last callback */

		/* Weak ref kept for signal disconnect + late muted re-query.
		 * Audio source itself outlives this struct (we hold a +1 ref while
		 * attached), so source pointer in callbacks is always valid. */
		OBSWeakSource source_weak;
		/* user_muted reflects UI mute (Mixer mute button). Updated by
		 * "mute" signal callback on the source's signal handler.
		 * Read on render thread, written from arbitrary OBS threads → atomic. */
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
		float displayMagnitude = -200.0f; /* smoothed RMS magnitude in dB */
		float displayPeak = -200.0f;      /* smoothed display value in dB */
		uint64_t last_render_ns = 0;      /* for ballistics time delta */
		float holdPeak = -200.0f;         /* peak hold value in dB */
		uint64_t holdSetAtNs = 0;         /* timestamp when holdPeak was last set */
		float channelDisplayMagnitude[MAX_AUDIO_CHANNELS];
		float channelDisplayPeak[MAX_AUDIO_CHANNELS];
		uint64_t channelLastRenderNs[MAX_AUDIO_CHANNELS];
		float channelHoldPeak[MAX_AUDIO_CHANNELS];
		uint64_t channelHoldSetAtNs[MAX_AUDIO_CHANNELS];
	};
	std::vector<CellVolmeter *> cell_volmeters_;
	/* Active mixer track bit (1 << (track_index - 1)). Recomputed in
	 * rebuild_volmeters() based on VuMeterSettings.trackMode. */
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

	/* Scene change detection for PGM/PRVW volmeter rebuild */
	OBSWeakSource last_pgm_scene_;
	OBSWeakSource last_prvw_scene_;
	bool has_pgm_cell_ = false;
	bool has_prvw_cell_ = false;
	void check_scene_change_for_volmeters();
	/* AutoFollow polling: checks streaming output mixer mask change between
	 * frames (no event fires on Settings → Output → Streaming Track change). */
	uint64_t last_track_poll_ns_ = 0;
	void check_active_track_change();

	/* Phase 3 / M5.4 hardening: throttle volmeter rebuilds to at most one
	 * per VOLMETER_REBUILD_MIN_INTERVAL_NS. Without this, a user that
	 * repeatedly deletes + restores a nested scene with audio sources
	 * triggers source_create / source_remove signal storms; each one
	 * schedules a refresh_sources_lazy which sets volmeters_rebuild_requested_,
	 * causing release+attach of volmeters at near every-frame rate. The
	 * resulting churn against OBS audio thread + graphics thread has been
	 * observed to deadlock OBS in extreme cases. The throttle keeps
	 * pending rebuilds coalesced into at most ~4 Hz; if rebuild is
	 * suppressed, the request flag stays set so the next allowed frame
	 * still rebuilds. */
	uint64_t last_volmeter_rebuild_ns_ = 0;
	/* Snapshot of the source pointers attached during the last rebuild,
	 * sorted for O(N) set comparison. Used by check_active_track_change()
	 * to detect any change in the currently-visible audio source set:
	 *   - a source's audio_mixers gained the active track bit (e.g. user
	 *     ticks Track 1 on Mic in Advanced Audio Properties)
	 *   - a scene was added/removed inside the watched scene (sceneitem
	 *     add/remove signals are not subscribed per-source)
	 *   - a previously-attached source disappeared
	 * Stored as void* identity (no deref); released-then-recreated sources
	 * are safe because new pointers won't match old ones. */
	std::vector<void *> last_active_sources_;
	void collect_active_source_pointers(std::vector<void *> &out, uint32_t track_bit);
	/* Queued rebuild request from non-Qt thread (e.g. audio_mixers signal). */
	std::atomic<bool> volmeters_rebuild_requested_{false};

	/* Phase 3 / M6.6 hardening: coalesce rebuild_lost_signal_images posts.
	 * The supervisor runs on the render thread and observes per-cell health
	 * transitions. Several external cells failing simultaneously (e.g. a
	 * shared NDI source going down) would otherwise post one
	 * QTimer::singleShot rebuild per cell per frame, flooding the Qt queue.
	 * We exchange this flag to true at post time and reset to false at the
	 * top of the queued lambda; if a second post arrives before the first
	 * lambda runs, exchange returns true and the redundant post is skipped. */
	std::atomic<bool> lost_images_rebuild_pending_{false};

	/* ---------- PGM / PRVW cell highlight borders (OBS-native style) ----------
	 *
	 * Each frame we snapshot the active PGM / PRVW source trees (recursively,
	 * via obs_source_enum_active_tree) into pointer sets. A cell is then
	 * classified as one of:
	 *   PgmDirect  — cell source == current PGM scene
	 *   PrvwDirect — cell source == current PRVW scene (Studio Mode only)
	 *   PgmNested  — cell source is reachable inside the PGM scene tree
	 *   PrvwNested — cell source is reachable inside the PRVW scene tree
	 *   None       — no relation, no border drawn
	 *
	 * Direct outranks nested; PGM outranks PRVW. Borders use solid fill rects
	 * for direct matches and dashed segments (GS_LINES vertex buffer) for
	 * nested matches. Color is window-scoped (Global+Instance only, no per-cell
	 * override). When Studio Mode is off, prvw_tree_set_ is left empty so the
	 * window automatically produces no green borders. */
	enum class HighlightKind { None, PgmDirect, PrvwDirect, PgmNested, PrvwNested };
	std::unordered_set<obs_source_t *> pgm_tree_set_;
	std::unordered_set<obs_source_t *> prvw_tree_set_;
	void refresh_highlight_tree_sets();
	HighlightKind compute_cell_highlight(int cellIndex);
	void render_cell_highlight(const CellRect &cell, int vpX, int vpY, HighlightKind kind,
				   const HighlightSettings &hs);
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
