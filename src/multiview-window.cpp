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

#include "multiview-window.hpp"
#include "cell-display-settings-dialog.hpp"
#include "edit-source-dialog.hpp"
#include "signal-lost-settings-dialog.hpp"
#include "signal-provider.hpp"
#include "source-picker.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QAction>
#include <QCloseEvent>
#include <QMenu>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

/* ---- helpers (same as OBS internal) ---- */

static inline void startRegion(int vX, int vY, int vCX, int vCY, float oL, float oR, float oT, float oB)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vX, vY, vCX, vCY);
	gs_ortho(oL, oR, oT, oB, -100.0f, 100.0f);
}

static inline void endRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}

/* ---- PGM/PRVW tree collection helpers ----
 *
 * obs_source_enum_active_tree walks all active (rendered) sources reachable
 * from a root scene, including nested scenes, source filters' inputs, and
 * any other compositional dependencies. It performs cycle detection
 * internally, so a self-referencing scene group will not infinite-recurse.
 *
 * We only keep raw pointer identity for set-membership testing — we never
 * dereference these pointers. The parent scene holds strong references to
 * everything in the tree for the duration of the frame, so the pointers
 * remain unique and valid for the lookup window. */
static void collect_tree_cb(obs_source_t * /*parent*/, obs_source_t *child, void *param)
{
	auto *out = static_cast<std::unordered_set<obs_source_t *> *>(param);
	if (child)
		out->insert(child);
}

static void collect_tree_sources(obs_source_t *root, std::unordered_set<obs_source_t *> &out)
{
	if (!root)
		return;
	/* Include the root itself so a cell whose source IS the PGM/PRVW scene
	 * gets classified correctly by the same set membership lookup. */
	out.insert(root);
	obs_source_enum_active_tree(root, collect_tree_cb, &out);
}

static inline QSize GetPixelSize(QWidget *widget)
{
	return widget->size() * widget->devicePixelRatioF();
}

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y,
					float &scale)
{
	double windowAspect = (double)windowCX / (double)windowCY;
	double baseAspect = (double)baseCX / (double)baseCY;
	int newCX, newCY;

	if (windowAspect > baseAspect) {
		scale = (float)windowCY / (float)baseCY;
		newCX = (int)((double)windowCY * baseAspect);
		newCY = windowCY;
	} else {
		scale = (float)windowCX / (float)baseCX;
		newCX = windowCX;
		newCY = (int)((float)windowCX / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

/* ---- MultiviewWindow implementation ---- */

MultiviewWindow::MultiviewWindow(ConfigManager *config, const std::string &uuid, QWidget *parent)
	: QWidget(parent, Qt::Window),
	  config_(config),
	  uuid_(uuid)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_DeleteOnClose, false);
	setAttribute(Qt::WA_QuitOnClose, false);

	setMinimumSize(320, 180);
	resize(960, 540);

	/* Window title */
	refresh_title();

	/* Escape to close */
	QAction *escAction = new QAction(this);
	escAction->setShortcut(Qt::Key_Escape);
	addAction(escAction);
	connect(escAction, &QAction::triggered, this, &QWidget::close);

	/* Create display when window becomes visible */
	connect(windowHandle(), &QWindow::visibleChanged, this, [this](bool visible) {
		if (visible && !display_created_)
			create_display();
	});

	/* Get canvas aspect ratio from OBS base resolution */
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		canvas_aspect_ = (double)ovi.base_width / (double)ovi.base_height;
	}

	refresh_layout();

	ready_ = true;
	show();
	activateWindow();
}

MultiviewWindow::~MultiviewWindow()
{
	ready_ = false;
	release_source_refs();
	destroy_display();
}

void MultiviewWindow::create_display()
{
	if (display_created_)
		return;
	if (!windowHandle())
		return;

	QSize size = GetPixelSize(this);

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#ifdef _WIN32
	info.window.hwnd = (HWND)windowHandle()->winId();
#elif __APPLE__
	info.window.view = (id)windowHandle()->winId();
#else
	info.window.id = windowHandle()->winId();
	info.window.display = obs_get_nix_platform_display();
#endif

	display_ = obs_display_create(&info, 0xFF000000);
	if (!display_)
		return;

	obs_display_add_draw_callback(display_, render_callback, this);
	display_created_ = true;
}

void MultiviewWindow::destroy_display()
{
	if (!display_created_)
		return;

	obs_display_remove_draw_callback(display_, render_callback, this);
	display_ = nullptr;
	display_created_ = false;
}

void MultiviewWindow::refresh_title()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (inst)
		setWindowTitle(QStringLiteral("Advanced Multiview - %1").arg(QString::fromStdString(inst->name)));
}

void MultiviewWindow::refresh_layout()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	layout_ = inst->layout;
	gutter_px_ = inst->effective_gutter(config_->global_settings().defaultGutterPx);

	/* Update layout with effective gutter */
	layout_.gutterPx = gutter_px_;

	/* Invalidate cached viewport to force recompute in next render frame */
	cached_vpW_ = 0;
	cached_vpH_ = 0;

	/* Re-resolve source refs: grid shape may have changed, affecting
	 * which cells exist and their indices */
	refresh_sources();

	/* Recompute effective visual settings for the new cell layout */
	refresh_visual_settings();
}

void MultiviewWindow::refresh_sources()
{
	release_source_refs();
	update_source_refs();
	rebuild_label_sources();
	rebuild_volmeters();
	rebuild_lost_signal_images();
}

bool MultiviewWindow::refresh_cell(int row, int col)
{
	/* Phase 3 / M6.1+ task 9.1.A: re-resolve a single cell without
	 * disturbing the other cells' runtime. Critical for windows that
	 * host long-segment HLS streams: rebuilding a 10-second-segment
	 * ffmpeg_source means the user sees seconds of black before
	 * playback resumes; doing that on every other cell's edit was the
	 * top usability gripe of the M6.1 first slice.
	 *
	 * Lock-order recap:
	 *   - cell_sources_ is touched only under source_mutex_ (read on
	 *     render thread, written here on UI thread).
	 *   - obs_source_create_private / dec_active must run OUTSIDE
	 *     source_mutex_ because both can fire host-plugin callbacks
	 *     into other threads which may try to take our mutex.
	 *
	 * The function therefore splits into four phases (collect under
	 * lock → tear down outside lock → build outside lock → install
	 * under lock), mirroring update_source_refs / release_source_refs
	 * but scoped to one cell. */

	/* Old refs we need to release outside the lock (for the cell we're
	 * replacing). For external private sources we keep the OBSSource
	 * so RAII drops it after the dec pair runs. */
	OBSSource old_external;
	OBSWeakSource old_internal_weak;
	bool old_internal_was_showing = false;

	/* New external intent (if the assignment after refresh is external). */
	bool new_is_external = false;
	SignalProviderType new_provider = SignalProviderType::Unknown;
	std::string new_desired_name;
	SignalConfig new_cfg_copy;

	int cellIdx = -1;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (!inst)
			return false;

		/* Recompute layout to know cell count + index for (row, col). */
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(800, 600);
		tmpEngine.compute();

		const auto &cells = tmpEngine.cells();
		if (cells.size() != cell_sources_.size()) {
			/* Layout / grid change in flight — parallel vectors are
			 * about to be resized. Bail out so the caller falls back
			 * to refresh_sources() which handles the resize correctly. */
			return false;
		}

		for (size_t i = 0; i < cells.size(); i++) {
			if (cells[i].gridRow == row && cells[i].gridCol == col) {
				cellIdx = (int)i;
				break;
			}
		}
		if (cellIdx < 0)
			return false; /* (row, col) not in current layout */

		auto &cs = cell_sources_[cellIdx];

		/* Phase 1 (under lock): capture old refs into locals so we can
		 * release them outside the lock. dec_showing on the internal
		 * weak ref is cheap and side-effect-free, so we keep it inside
		 * the lock (matching update_source_refs()'s old behavior on
		 * full refresh). The expensive piece is the external private
		 * source, which we move out to drop later. */
		if (cs.showing) {
			old_internal_weak = cs.weak_ref;
			old_internal_was_showing = true;
		}
		cs.weak_ref = nullptr;
		cs.showing = false;

		if (cs.private_source) {
			old_external = std::move(cs.private_source);
			cs.private_source = nullptr;
		}

		/* Reset runtime fields to the same defaults update_source_refs
		 * uses for a fresh cell, then re-resolve the assignment. */
		cs.type.clear();
		cs.name.clear();
		cs.prvw_fallback = false;
		cs.state = SignalRuntimeState::Empty;
		cs.last_active_ns = 0;
		cs.last_reconnect_ns = 0;
		cs.retry_attempt = 0;
		cs.provider_type = SignalProviderType::Unknown;
		cs.provider_settings_hash = 0;
		cs.last_error_reason.clear();

		/* Look up the (possibly new) assignment for this cell. */
		const CellAssignment *ca = nullptr;
		for (auto &a : inst->cellAssignments) {
			if (a.row == row && a.col == col) {
				ca = &a;
				break;
			}
		}

		std::string short_uuid = uuid_.size() > 8 ? uuid_.substr(0, 8) : uuid_;

		if (ca && ca->signalConfig.is_external()) {
			cs.provider_type = ca->signalConfig.provider;
			cs.name = ca->signalConfig.displayName;
			cs.state = SignalRuntimeState::Connecting;

			char namebuf[256];
			snprintf(namebuf, sizeof(namebuf), "OBS Advanced Multiview/%s/%d,%d/%s", short_uuid.c_str(),
				 row, col, signal_provider_to_string(ca->signalConfig.provider));

			new_is_external = true;
			new_provider = ca->signalConfig.provider;
			new_desired_name = namebuf;
			new_cfg_copy = ca->signalConfig; /* SignalConfig deep-copy */
		} else if (ca && !ca->type.empty()) {
			cs.type = ca->type;
			cs.name = ca->name;
			if (ca->type == "pgm" || ca->type == "prvw") {
				cs.state = SignalRuntimeState::Active;
			} else {
				obs_source_t *src = obs_get_source_by_name(ca->name.c_str());
				if (src && obs_source_removed(src)) {
					obs_source_release(src);
					cs.state = SignalRuntimeState::MissingInternal;
				} else if (src) {
					cs.weak_ref = OBSGetWeakRef(src);
					obs_source_inc_showing(src);
					cs.showing = true;
					cs.state = SignalRuntimeState::Active;
					obs_source_release(src);
				} else {
					cs.state = SignalRuntimeState::MissingInternal;
				}
			}
		}

		/* Refresh effective Lost Signal cache for this cell. Cheap; we
		 * recompute the whole vector to keep behavior identical to the
		 * full refresh path's recompute_effective_lost_locked. */
		recompute_effective_lost_locked(cells);
	}

	/* Phase 2 (outside lock): tear down old internal ref and old
	 * external private source. dec_showing on a weak-ref strong
	 * resolution can fire scene callbacks; dec_active on a private
	 * ffmpeg_source stops its mp_media worker thread — both must not
	 * run while source_mutex_ is held. */
	if (old_internal_was_showing) {
		OBSSourceAutoRelease oldSrc = OBSGetStrongRef(old_internal_weak);
		if (oldSrc)
			obs_source_dec_showing(oldSrc);
	}
	if (old_external) {
		obs_source_dec_showing(old_external);
		obs_source_dec_active(old_external);
		old_external = nullptr; /* RAII drops the strong ref */
	}

	/* Phase 3 (outside lock): build the new external private source if
	 * the assignment we resolved above is external. */
	OBSSource new_external;
	std::string create_failure_reason;
	if (new_is_external) {
		auto &reg = SignalProviderRegistry::instance();
		const auto *provider = reg.find(new_provider);
		if (!provider) {
			create_failure_reason = "provider not registered";
			obs_log(LOG_WARNING,
				"[multiview-window] refresh_cell: provider not registered for cell (%d,%d) type=%d",
				row, col, (int)new_provider);
		} else {
			new_external = provider->create_private_source(new_desired_name, new_cfg_copy);
			if (new_external) {
				/* Activate the private source so async providers (FFmpeg,
				 * NDI, etc.) start producing frames. inc_active is enough
				 * — it already bumps show_refs internally via MAIN_VIEW
				 * activate, so a separate inc_showing would double-count. */
				obs_source_inc_active(new_external);

				/* Phase 3 / M6.1 perf: apply the provider's
				 * unbuffered preference uniformly here so refresh_cell
				 * matches the full-refresh path. */
				if (provider->prefers_unbuffered_async(new_cfg_copy))
					obs_source_set_async_unbuffered(new_external, true);
			} else {
				create_failure_reason = provider->unavailable_reason();
				if (create_failure_reason.empty())
					create_failure_reason = "provider failed to create source";
			}
		}
	}

	/* Phase 4 (under lock): install. */
	bool need_volmeter_rebuild = false;
	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);
		if (cellIdx < (int)cell_sources_.size()) {
			auto &cs = cell_sources_[cellIdx];
			if (new_is_external) {
				cs.private_source = new_external;
				if (new_external) {
					/* Phase 3 / M6 step 10: stamp the new
					 * source's birth time so the supervisor's
					 * age_ns is accurate from tick 1. Clear
					 * the recovery bookkeeping so a fresh
					 * Opening countdown starts clean. */
					cs.source_created_ns = os_gettime_ns();
					cs.connecting_since_ns = 0;
					cs.lost_since_ns = 0;
					cs.media_restart_attempts = 0;
					cs.next_retry_ns = 0;
					cs.last_health_ns = 0;
					/* Phase 3 / M6.6 fix: do NOT optimistically
					 * set cs.state to Active here. The new source
					 * hasn't been probed yet \u2014 ffmpeg/vlc may take
					 * 100ms-3s to open the URL (or, in the retry-
					 * after-Lost case, may immediately fail again).
					 * Setting Active here causes a one-frame flash:
					 * compute_wanted_lost_image_path would treat
					 * Active as "draw the source video, no fallback
					 * image", but the source has no frames to draw,
					 * resulting in a black hole until the supervisor
					 * tick (~1s) flips back to Lost. Use Connecting
					 * instead so the lost-image renderer continues
					 * painting the user's fallback image until the\n\t\t\t\t\t * source actually produces frames. */
					cs.state = SignalRuntimeState::Connecting;
					cs.last_active_ns = cs.source_created_ns;
					cs.last_error_reason.clear();
				} else {
					cs.state = SignalRuntimeState::Error;
					cs.last_error_reason = create_failure_reason;
				}
			}
			need_volmeter_rebuild = true;
		} else if (new_external) {
			/* Layout shrank between create and install — unwind the
			 * inc_active and let RAII drop the strong ref. */
			obs_source_dec_active(new_external);
		}
	}

	/* Path-keyed bg/overlay/lost-signal/label rebuilds are no-ops when
	 * the per-cell wanted path or text didn't change. They're cheap
	 * enough to run unconditionally per single-cell edit; the win we
	 * really care about (not interrupting OTHER cells' external private
	 * sources) is already secured by the targeted teardown above. */
	rebuild_label_sources();
	rebuild_lost_signal_images();

	/* Volmeter rebuild flag — coalesced by the next render frame's
	 * check_active_track_change so we never run a heavy rebuild here. */
	if (need_volmeter_rebuild)
		volmeters_rebuild_requested_.store(true, std::memory_order_release);

	return true;
}

/* Phase 3 / M5: in-place CellSource refresh used by the source-list signal
 * bridge (plugin-main connects source_create / source_remove / source_destroy
 * / source_rename and debounces them through this path).
 *
 * Unlike refresh_sources(), this function:
 *   - never clears label_sources_ / bg_images_ / overlay_images_ / volmeters_,
 *     so unrelated cells keep rendering their MISSING SOURCE / FALLBACK
 *     overlays without a multi-frame gap;
 *   - only revisits cells whose assignment (type+name) is unchanged for state
 *     evaluation, and rebinds weak refs only when the assignment itself
 *     changed (rare on signal events);
 *   - bails out when the cell count differs from the live vector — this only
 *     happens on a layout/grid edit, which always goes through the heavy
 *     refresh_sources() path anyway.
 *
 * The signal bridge cares about two cases: a source the user just added/
 * undid coming back (we re-bind here, state flips to Active on next frame),
 * and a source being destroyed (we leave the stale weak ref in place — the
 * render-thread lazy resolve already detects it returning null and switches
 * the cell to MissingInternal, which makes the overlay appear without
 * touching any other cell). */
void MultiviewWindow::refresh_sources_lazy()
{
	update_source_refs_lazy();
	/* Phase 3 / M5.4: a lazy refresh can change cs.state (e.g. a source
	 * the user just undid is now Active again). Picking up the matching
	 * placeholder / fallback image needs to happen on the UI thread, so
	 * piggyback on this entry point. The rebuild itself is path-keyed —
	 * if nothing changed, it's a no-op. */
	rebuild_lost_signal_images();
}

void MultiviewWindow::on_source_being_removed(obs_source_t *source)
{
	if (!source)
		return;

	bool any_match = false;
	std::vector<std::pair<int, int>> clear_rowcols;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		/* Build a (cell index -> row,col) lookup once per call so we can
		 * map a matched cell back to its grid coordinates without a
		 * second LayoutEngine pass per cell. */
		std::vector<std::pair<int, int>> idx_to_rowcol;
		{
			LayoutEngine tmpEngine;
			tmpEngine.set_layout(layout_);
			tmpEngine.set_viewport(800, 600);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			idx_to_rowcol.reserve(cells.size());
			for (const auto &cr : cells)
				idx_to_rowcol.emplace_back(cr.gridRow, cr.gridCol);
		}

		for (size_t i = 0; i < cell_sources_.size(); i++) {
			auto &cs = cell_sources_[i];
			if (cs.type.empty() || cs.type == "pgm" || cs.type == "prvw")
				continue;
			if (!cs.weak_ref)
				continue;
			OBSSourceAutoRelease bound = OBSGetStrongRef(cs.weak_ref);
			if (bound != source)
				continue;

			/* Phase 3 / M5.4 hardening: deliberately do NOT call
			 * obs_source_dec_showing() here, even though we paired the cell's
			 * inc_showing with one. Two reasons:
			 *
			 *  1. dec_showing fires the "hide" signal synchronously into every
			 *     plugin that subscribed (streamdeck, obs-websocket, scripts,
			 *     etc). Those handlers run *during* source_remove, while the
			 *     source is marked removed but not yet destroyed. Several
			 *     third-party plugins have been observed to corrupt their own
			 *     state when they receive hide while OBS is mid-removal,
			 *     manifesting as obs_source_release / obs_scene_release access
			 *     violations later when OBS undo/redo or RemoveSelectedScene
			 *     paths release the source's control block.
			 *
			 *  2. The source is about to be destroyed (or held alive solely by
			 *     OBS's undo data, which doesn't render). When destroy actually
			 *     runs, libobs frees the show_refs counter alongside the
			 *     source — there is no leak to clean up. Until then the source
			 *     is unreachable through any surface a user can interact with.
			 *
			 * We still drop the weak ref + reset state so subsequent renders
			 * never call obs_source_video_render() through this binding. */
			cs.showing = false;
			cs.weak_ref = nullptr;
			cs.state = SignalRuntimeState::MissingInternal;
			cs.last_active_ns = 0;
			any_match = true;

			/* Phase 3 / M5.1 ClearCell: if the user explicitly chose
			 * ClearCell as the InternalMissingBehavior for this cell,
			 * collect its (row, col) so the Qt main thread can drop
			 * the assignment + persist after we leave this signal
			 * handler. We deliberately don't mutate cellAssignments
			 * here: source_remove can fire on non-UI threads, and
			 * config save / refresh_sources must run on the UI thread
			 * (Qt widgets, Frontend API, file IO). */
			if (cs.effective_lost.internalMissingBehavior == InternalMissingBehavior::ClearCell &&
			    i < idx_to_rowcol.size()) {
				clear_rowcols.push_back(idx_to_rowcol[i]);
				/* Mark the cell as awaiting clear so the render
				 * thread paints it as Empty (gutter colour, no
				 * MISSING SOURCE flash) for the 1-2 frames before
				 * apply_clear_cell_for_rowcols runs and rebuilds
				 * cell_sources_. */
				cs.pending_clear = true;
			}
		}

		/* Only kick a volmeter rebuild when this window actually held the
		 * source. Otherwise we'd needlessly churn meters on every unrelated
		 * source_remove fired across the whole OBS session. */
		if (any_match)
			volmeters_rebuild_requested_.store(true, std::memory_order_release);
	}

	/* Phase 3 / M5.4: bring up placeholder / static-image fallback texture
	 * for the cells that just transitioned to MissingInternal. The rebuild
	 * is path-keyed, so cells whose configured image is empty are no-ops.
	 * Must run OUTSIDE source_mutex_ — rebuild calls obs_enter_graphics()
	 * and the render thread takes (graphics, source_mutex_) in that order. */
	if (any_match)
		rebuild_lost_signal_images();

	/* Phase 3 / M5.1 ClearCell: queue persistence onto the UI thread.
	 * QueuedConnection (via QTimer::singleShot 0ms targeting `this`) means
	 * the lambda runs on the thread that owns `this` (Qt main thread for
	 * any QWidget), regardless of which thread fired source_remove. */
	if (!clear_rowcols.empty()) {
		auto rowcols_copy = clear_rowcols;
		QTimer::singleShot(0, this, [this, rowcols_copy]() { apply_clear_cell_for_rowcols(rowcols_copy); });
	}
}

void MultiviewWindow::on_source_just_created(obs_source_t *source)
{
	if (!source)
		return;

	/* Don't rebind through a source that's already on its way out — same
	 * caution we apply on every other bind path. Shouldn't happen for
	 * source_create, but cheap defence. */
	if (obs_source_removed(source))
		return;

	const char *cname = obs_source_get_name(source);
	if (!cname || !*cname)
		return;
	const std::string newName(cname);

	bool any_match = false;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		for (auto &cs : cell_sources_) {
			if (cs.type.empty() || cs.type == "pgm" || cs.type == "prvw")
				continue;
			if (cs.state != SignalRuntimeState::MissingInternal)
				continue;
			if (cs.name != newName)
				continue;

			/* Sync re-bind. Pair the inc_showing here so the source's
			 * show_refs counter matches the binding lifetime — opposite of
			 * source_remove where we deliberately don't dec_showing. Here
			 * the source is alive and stable, so inc_showing is safe and
			 * keeps audio meters reachable. */
			cs.weak_ref = OBSGetWeakRef(source);
			obs_source_inc_showing(source);
			cs.showing = true;
			cs.state = SignalRuntimeState::Active;
			cs.last_active_ns = os_gettime_ns();
			cs.retry_attempt = 0;
			any_match = true;
		}

		if (any_match)
			volmeters_rebuild_requested_.store(true, std::memory_order_release);
	}

	/* Phase 3 / M5.4: cell transitioned out of MissingInternal — release
	 * the placeholder / static-image texture so the next render falls
	 * straight through to the real source video. Must run OUTSIDE
	 * source_mutex_ to honour the (graphics, source_mutex_) lock order. */
	if (any_match)
		rebuild_lost_signal_images();
}

void MultiviewWindow::update_source_refs_lazy()
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(800, 600);
	tmpEngine.compute();

	const auto &cells = tmpEngine.cells();
	size_t cellCount = cells.size();

	/* Cell count mismatch ⇒ a layout edit is in flight. Lazy refresh
	 * cannot keep the parallel vectors in sync; bail and let the next
	 * full refresh_sources() (always issued on layout changes) handle
	 * it. The signal bridge will fire again on the next OBS event so we
	 * don't lose anything by skipping this round. */
	if (cell_sources_.size() != cellCount)
		return;

	bool any_state_change = false;

	for (size_t i = 0; i < cellCount; i++) {
		int r = cells[i].gridRow;
		int c = cells[i].gridCol;
		const CellAssignment *ca = nullptr;
		for (auto &a : inst->cellAssignments) {
			if (a.row == r && a.col == c) {
				ca = &a;
				break;
			}
		}

		std::string newType = ca ? ca->type : std::string();
		std::string newName = ca ? ca->name : std::string();

		auto &cs = cell_sources_[i];
		if (cs.type == newType && cs.name == newName) {
			/* Assignment unchanged. State recovery (e.g. a Source
			 * the user just undid coming back) happens on the
			 * render thread via the existing lazy re-resolve path
			 * — we rebind here too so audio metering doesn't have
			 * to wait for the next render frame. */
			if (!newType.empty() && newType != "pgm" && newType != "prvw") {
				/* Phase 3 / M5.4 hardening: if the cached strong ref
				 * still resolves but obs_source_removed() reports the
				 * source is marked-removed, treat it as gone. We must
				 * NOT keep rendering through it (a third-party plugin's
				 * sceneitem signal handler can crash mid-prune) and we
				 * must NOT keep the volmeter attached to a doomed
				 * source. Drop the binding so the next frame picks up
				 * the configured fallback (or MISSING SOURCE). */
				if (cs.weak_ref) {
					OBSSourceAutoRelease cur = OBSGetStrongRef(cs.weak_ref);
					if (cur && obs_source_removed(cur)) {
						if (cs.showing) {
							obs_source_dec_showing(cur);
							cs.showing = false;
						}
						cs.weak_ref = nullptr;
						cs.state = SignalRuntimeState::MissingInternal;
						any_state_change = true;
					}
				}
				if (cs.state == SignalRuntimeState::MissingInternal) {
					obs_source_t *src = obs_get_source_by_name(newName.c_str());
					if (src && obs_source_removed(src)) {
						/* Resolved by name but already on its way out.
						 * Don't bind — release and stay missing. */
						obs_source_release(src);
					} else if (src) {
						cs.weak_ref = OBSGetWeakRef(src);
						if (!cs.showing) {
							obs_source_inc_showing(src);
							cs.showing = true;
						}
						cs.state = SignalRuntimeState::Active;
						obs_source_release(src);
						any_state_change = true;
					}
				}
			}
			continue;
		}

		/* Assignment changed (most likely a rename event affecting
		 * this cell's bound source). Drop the old binding cleanly,
		 * reset runtime state, then re-bind to the new name. */
		if (cs.showing) {
			OBSSourceAutoRelease oldSrc = OBSGetStrongRef(cs.weak_ref);
			if (oldSrc)
				obs_source_dec_showing(oldSrc);
			cs.showing = false;
		}
		cs.weak_ref = nullptr;
		cs.type = newType;
		cs.name = newName;
		cs.prvw_fallback = false;
		cs.state = SignalRuntimeState::Empty;
		cs.last_active_ns = 0;
		cs.last_reconnect_ns = 0;
		cs.retry_attempt = 0;

		if (newType.empty()) {
			any_state_change = true;
			continue;
		}
		if (newType == "pgm" || newType == "prvw") {
			cs.state = SignalRuntimeState::Active;
			any_state_change = true;
			continue;
		}

		obs_source_t *src = obs_get_source_by_name(newName.c_str());
		if (src) {
			cs.weak_ref = OBSGetWeakRef(src);
			obs_source_inc_showing(src);
			cs.showing = true;
			cs.state = SignalRuntimeState::Active;
			obs_source_release(src);
		} else {
			cs.state = SignalRuntimeState::MissingInternal;
		}
		any_state_change = true;
	}

	/* A rename / re-bind may have changed which audio sources are reachable
	 * from this multiview window — kick the volmeter rebuild so the next
	 * frame on the render thread re-attaches meters without waiting for the
	 * 1Hz active-source poll. */
	if (any_state_change)
		volmeters_rebuild_requested_.store(true, std::memory_order_release);

	/* Effective lost-signal settings depend on (row,col) which can't change
	 * during a lazy refresh, but per-cell Override entries themselves may
	 * have been edited concurrently via the dialog. Refresh the cache so we
	 * never render against a stale fallback / placeholder choice. */
	recompute_effective_lost_locked(cells);
}
void MultiviewWindow::refresh_visual_settings()
{
	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (!inst) {
			effective_visuals_.clear();
			return;
		}

		const GlobalVisualSettings &globalVS = config_->global_settings().visualSettings;
		const InstanceVisualSettings &instVS = inst->visualSettings;

		/* Recompute layout to know cell positions */
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();

		const auto &cells = tmpEngine.cells();
		effective_visuals_.resize(cells.size());

		for (size_t i = 0; i < cells.size(); i++) {
			int r = cells[i].gridRow;
			int c = cells[i].gridCol;
			const CellVisualSettings *cellVS = inst->find_cell_visual(r, c);
			effective_visuals_[i] = resolve_effective_visual_settings(globalVS, instVS, cellVS);
		}

		rebuild_label_sources();
	}

	/* Image rebuild involves obs_enter_graphics() which must be called
	 * without holding source_mutex_ to prevent ABBA deadlock with the
	 * render thread (render thread: graphics lock -> source_mutex_). */
	rebuild_bg_images();
	rebuild_overlay_images();
	rebuild_lost_signal_images();
	rebuild_scale_label_sources();
}

void MultiviewWindow::refresh_signal_settings()
{
	/* Phase 3 / M5.4 / M5.5: Lost Signal settings just changed (Global edit
	 * via Settings tab, or per-cell Override via the cell context menu).
	 *
	 * The runtime state itself is recomputed every frame inside render()
	 * from `cell_sources_[i].state`; we only need to refresh the cached
	 * effective LostSignalSettings so the very next frame picks up the new
	 * fallback / placeholder choice without waiting for the user to edit a
	 * cell assignment. Texture-based placeholder / signal-lost-image loaders
	 * (the bg-image-style four-stage pipeline) will hook in here as well
	 * when that work lands. */
	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(800, 600);
		tmpEngine.compute();
		recompute_effective_lost_locked(tmpEngine.cells());
	}

	/* Phase 3 / M5.4: lost-signal images depend on
	 * placeholderImagePath / fallbackName which the user just edited.
	 * The rebuild is path-keyed so cells with unchanged paths are no-ops.
	 * Must run OUTSIDE source_mutex_ — rebuild calls obs_enter_graphics()
	 * and the render thread takes (graphics, source_mutex_) in order. */
	rebuild_lost_signal_images();
}

bool MultiviewWindow::force_reconnect_cell(int cellIndex)
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return false;
	auto &cs = cell_sources_[cellIndex];
	if (cs.type.empty() && cs.provider_type == SignalProviderType::Unknown)
		return false;

	/* Resolve cooldown from effective Lost Signal settings so users who
	 * raised the cooldown deliberately (e.g. flaky NDI sources) keep their
	 * preferred pacing. Falls back to the documented 1s default if the
	 * instance / global config disappears mid-call. */
	int cooldownMs = 1000;
	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			LayoutEngine tmpEngine;
			tmpEngine.set_layout(inst->layout);
			tmpEngine.set_viewport(100, 100);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			if (cellIndex < (int)cells.size()) {
				int r = cells[cellIndex].gridRow;
				int c = cells[cellIndex].gridCol;
				const CellLostSignalSettings *cls = inst->find_cell_lost_signal(r, c);
				LostSignalSettings eff =
					resolve_effective_lost_signal(config_->global_settings().lostSignal, cls);
				cooldownMs = eff.manualReconnectCooldownMs;
			}
		}
	}

	const uint64_t now = os_gettime_ns();
	if (cs.last_reconnect_ns != 0 && cooldownMs > 0) {
		uint64_t elapsedMs = (now - cs.last_reconnect_ns) / 1000000ull;
		if ((int)elapsedMs < cooldownMs) {
			obs_log(LOG_DEBUG, "reconnect cooldown active for cell %d (%llu/%d ms)", cellIndex,
				(unsigned long long)elapsedMs, cooldownMs);
			return false;
		}
	}
	cs.last_reconnect_ns = now;
	cs.retry_attempt++;

	/* Phase 3 / M6.1: external provider cell. The cheap reconnect path is
	 * obs_source_media_restart, which for ffmpeg_source re-opens the URL
	 * via mp_media_set_active(false) -> set_active(true) on its worker
	 * thread. No need to recreate the private source. Full recreate
	 * (release_source_refs + refresh_sources) is reserved for step 10's
	 * health supervisor when restart alone has failed N times.
	 *
	 * Held strong ref via OBSSource (private_source) survives the call;
	 * obs_source_media_restart is safe to call on any source type that
	 * registered the media callbacks (ffmpeg_source / vlc_source). For
	 * provider types that don't expose media controls (ndi_source,
	 * spout_capture) it is a no-op in OBS, which still satisfies the
	 * "manual attempt was made" semantics for the cooldown / UI. */
	if (cs.provider_type != SignalProviderType::Unknown && !signal_provider_is_internal(cs.provider_type)) {
		if (cs.private_source) {
			obs_source_media_restart(cs.private_source);
			cs.state = SignalRuntimeState::Connecting;
			obs_log(LOG_INFO, "reconnect cell %d: media_restart on external provider '%s'", cellIndex,
				signal_provider_to_string(cs.provider_type));
		} else {
			cs.state = SignalRuntimeState::Error;
			obs_log(LOG_INFO, "reconnect cell %d: external provider has no private source", cellIndex);
		}
		return true;
	}

	/* PGM/PRVW are resolved fresh per-frame; nothing to rebuild but we
	 * still record the attempt so the cooldown applies symmetrically. */
	if (cs.type == "pgm" || cs.type == "prvw") {
		cs.state = SignalRuntimeState::Active;
		return true;
	}

	/* Internal scene/source: drop the cached weak ref so the very next
	 * frame falls into the existing lazy re-resolve path with a forced
	 * lookup (re_resolve_counter_ == 0 isn't required because we go
	 * straight through obs_get_source_by_name). */
	if (cs.showing) {
		OBSSourceAutoRelease oldSrc = OBSGetStrongRef(cs.weak_ref);
		if (oldSrc)
			obs_source_dec_showing(oldSrc);
		cs.showing = false;
	}
	cs.weak_ref = nullptr;

	if (!cs.name.empty()) {
		obs_source_t *resolved = obs_get_source_by_name(cs.name.c_str());
		if (resolved && obs_source_removed(resolved)) {
			/* Phase 3 / M5.4 hardening: name resolves to a marked-removed
			 * source (deletion in flight). Don't bind — let the cell stay
			 * MissingInternal so fallback / overlay paths render instead. */
			obs_source_release(resolved);
			resolved = nullptr;
		}
		if (resolved) {
			cs.weak_ref = OBSGetWeakRef(resolved);
			obs_source_inc_showing(resolved);
			cs.showing = true;
			cs.state = SignalRuntimeState::Active;
			obs_source_release(resolved);
			obs_log(LOG_INFO, "reconnect cell %d: resolved '%s'", cellIndex, cs.name.c_str());
			return true;
		}
	}
	cs.state = SignalRuntimeState::MissingInternal;
	obs_log(LOG_INFO, "reconnect cell %d: '%s' still missing", cellIndex, cs.name.c_str());
	return true;
}

void MultiviewWindow::update_source_refs()
{
	/* Phase 3 / M6 step 9: per-cell external provider intents collected
	 * inside the locked section, materialized OUTSIDE the lock so
	 * obs_source_create_private never runs while we hold source_mutex_
	 * (see plan.md §6 lock order). Each intent owns a deep copy of the
	 * provider settings so the SignalConfig in instance memory can be
	 * mutated by the UI thread without racing us. */
	struct ExternalIntent {
		size_t cell_idx = 0;
		SignalProviderType provider = SignalProviderType::Unknown;
		std::string desired_name;
		SignalConfig cfg_copy; /* deep copy of CellAssignment.signalConfig */
	};
	std::vector<ExternalIntent> externals;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (!inst)
			return;

		/* Recompute layout to know cell count and positions */
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(800, 600); /* dummy size for cell count */
		tmpEngine.compute();

		const auto &cells = tmpEngine.cells();
		size_t cellCount = cells.size();
		cell_sources_.resize(cellCount);

		/* Short instance UUID prefix for the deterministic private-source
		 * name. Eight hex characters is enough to disambiguate windows
		 * within one OBS session without making the source name unwieldy. */
		std::string short_uuid = uuid_.size() > 8 ? uuid_.substr(0, 8) : uuid_;

		for (size_t i = 0; i < cellCount; i++) {
			cell_sources_[i].type.clear();
			cell_sources_[i].name.clear();
			cell_sources_[i].weak_ref = nullptr;
			cell_sources_[i].showing = false;
			cell_sources_[i].prvw_fallback = false;
			cell_sources_[i].state = SignalRuntimeState::Empty;
			cell_sources_[i].last_active_ns = 0;
			cell_sources_[i].last_reconnect_ns = 0;
			cell_sources_[i].retry_attempt = 0;
			cell_sources_[i].provider_type = SignalProviderType::Unknown;

			/* Look up assignment by (gridRow, gridCol) */
			int r = cells[i].gridRow;
			int c = cells[i].gridCol;
			const CellAssignment *ca = nullptr;
			for (auto &a : inst->cellAssignments) {
				if (a.row == r && a.col == c) {
					ca = &a;
					break;
				}
			}
			if (!ca)
				continue;

			/* Phase 3 / M6 step 9: external-provider cell. Detected
			 * by a non-empty SignalConfig with an external provider
			 * type. Legacy type/name stay empty so the existing
			 * obs_get_source_by_name path never fires for this cell;
			 * the render-thread external branch picks up
			 * cs.private_source / cs.provider_type instead. */
			if (ca->signalConfig.is_external()) {
				cell_sources_[i].provider_type = ca->signalConfig.provider;
				cell_sources_[i].name = ca->signalConfig.displayName;
				cell_sources_[i].state = SignalRuntimeState::Connecting;

				char namebuf[256];
				snprintf(namebuf, sizeof(namebuf), "OBS Advanced Multiview/%s/%d,%d/%s",
					 short_uuid.c_str(), r, c,
					 signal_provider_to_string(ca->signalConfig.provider));

				ExternalIntent intent;
				intent.cell_idx = i;
				intent.provider = ca->signalConfig.provider;
				intent.desired_name = namebuf;
				intent.cfg_copy = ca->signalConfig; /* SignalConfig deep-copy */
				externals.push_back(std::move(intent));
				continue;
			}

			if (ca->type.empty())
				continue;

			cell_sources_[i].type = ca->type;
			cell_sources_[i].name = ca->name;

			/* PGM/PRVW are resolved per-frame in render(), no caching */
			if (ca->type == "pgm" || ca->type == "prvw") {
				cell_sources_[i].state = SignalRuntimeState::Active;
				continue;
			}

			/* Scene/Source: cache weak ref and inc_showing */
			obs_source_t *src = obs_get_source_by_name(ca->name.c_str());
			if (src && obs_source_removed(src)) {
				/* Phase 3 / M5.4 hardening: matched by name but already on
				 * the way out (deletion in flight). Don't bind — release
				 * and let the cell sit in MissingInternal so the fallback /
				 * overlay paths kick in. */
				obs_source_release(src);
				cell_sources_[i].state = SignalRuntimeState::MissingInternal;
				continue;
			}
			if (src) {
				cell_sources_[i].weak_ref = OBSGetWeakRef(src);
				obs_source_inc_showing(src);
				cell_sources_[i].showing = true;
				cell_sources_[i].state = SignalRuntimeState::Active;
				obs_source_release(src);
			} else {
				/* Phase 3 / M5: name resolves to nothing right now — mark as missing.
				 * The Phase 2 lazy re-resolve path in render() can still recover it
				 * (e.g. user undoes a deletion); we just record the state here. */
				cell_sources_[i].state = SignalRuntimeState::MissingInternal;
			}
		}

		/* Cache effective LostSignalSettings (Global + per-cell Override) so the
		 * render path can branch on fallback / placeholder strategy without
		 * touching the instance config every frame. */
		recompute_effective_lost_locked(cells);
	}

	/* Phase 3 / M6 step 9: materialize external private sources OUTSIDE
	 * source_mutex_. obs_source_create_private fires the source's create
	 * chain which can touch the OBS graphics/audio subsystems and, for
	 * URL-based ffmpeg_source, spawn an internal worker thread; we must
	 * not hold our recursive mutex while that runs. The install step at
	 * the bottom of the loop re-takes the lock briefly per cell. */
	if (!externals.empty()) {
		auto &reg = SignalProviderRegistry::instance();
		for (auto &it : externals) {
			const auto *provider = reg.find(it.provider);
			if (!provider) {
				obs_log(LOG_WARNING,
					"[multiview-window] external provider not registered for cell %zu (type=%d)",
					it.cell_idx, (int)it.provider);
				std::lock_guard<std::recursive_mutex> lock(source_mutex_);
				if (it.cell_idx < cell_sources_.size()) {
					cell_sources_[it.cell_idx].state = SignalRuntimeState::Error;
					cell_sources_[it.cell_idx].last_error_reason = "provider not registered";
				}
				continue;
			}

			OBSSource priv = provider->create_private_source(it.desired_name, it.cfg_copy);

			/* Phase 3 / M6 step 9: async OBS sources (ffmpeg_source,
			 * ndi_source, spout_capture, vlc_source) only start producing
			 * media when their `active` count goes above 0. Private sources
			 * are never added to a scene, so OBS never bumps active on our
			 * behalf — the source would just sit dormant and our cell would
			 * paint black forever.
			 *
			 * obs_source_inc_active(MAIN_VIEW) already does the show_refs
			 * increment internally (obs_source_activate calls
			 * enum_active_tree+show_tree before enum_active_tree+activate_
			 * tree), so we deliberately do NOT call obs_source_inc_showing
			 * separately — that would double-count show_refs and confuse
			 * frontend tools that watch the showing state. Paired with
			 * dec_active in release_source_refs(). */
			if (priv) {
				obs_source_inc_active(priv);

				/* Phase 3 / M6.1 perf: apply the provider's
				 * unbuffered preference uniformly here so all
				 * private external sources go through one decision
				 * point. See ISignalProvider::prefers_unbuffered_async
				 * for the policy table. */
				if (provider->prefers_unbuffered_async(it.cfg_copy))
					obs_source_set_async_unbuffered(priv, true);
			}

			std::lock_guard<std::recursive_mutex> lock(source_mutex_);
			if (it.cell_idx >= cell_sources_.size()) {
				/* Layout shrank between intent collection and install.
				 * Unwind the inc_active before the OBSSource RAII drops
				 * the strong ref so the source tears down cleanly. */
				if (priv) {
					obs_source_dec_active(priv);
				}
				continue;
			}
			auto &cs = cell_sources_[it.cell_idx];
			cs.private_source = priv;
			if (priv) {
				/* Phase 3 / M6 step 10: stamp the source's birth
				 * time so the supervisor's age_ns is accurate from
				 * tick 1 (its 5 s Opening grace is measured from
				 * source_created_ns).
				 *
				 * Phase 3 / M6.6 fix: do NOT optimistically set
				 * cs.state to Active here. The new source hasn't
				 * produced a frame yet; if the supervisor's first
				 * probe is delayed by up to 1s, the lost-image
				 * renderer would treat the cell as Active and stop
				 * painting the user's fallback image, producing a
				 * one-frame black flash on every recreate cycle.
				 * Connecting is the correct initial state \u2014 the
				 * supervisor flips it to Active when frames arrive. */
				cs.source_created_ns = os_gettime_ns();
				cs.connecting_since_ns = 0;
				cs.lost_since_ns = 0;
				cs.media_restart_attempts = 0;
				cs.state = SignalRuntimeState::Connecting;
				cs.last_active_ns = cs.source_created_ns;
				cs.last_health_ns = 0;
				cs.last_error_reason.clear();
			} else {
				cs.state = SignalRuntimeState::Error;
				cs.last_error_reason = provider->unavailable_reason();
				if (cs.last_error_reason.empty())
					cs.last_error_reason = "provider failed to create source";
			}
		}
	}
}

void MultiviewWindow::recompute_effective_lost_locked(const std::vector<CellRect> &cells)
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;
	if (cell_sources_.size() != cells.size())
		return;

	const LostSignalSettings &globalLost = config_->global_settings().lostSignal;
	for (size_t i = 0; i < cells.size(); i++) {
		int r = cells[i].gridRow;
		int c = cells[i].gridCol;
		const CellLostSignalSettings *cls = nullptr;
		for (auto &x : inst->cellLostSignalSettings) {
			if (x.row == r && x.col == c) {
				cls = &x;
				break;
			}
		}
		cell_sources_[i].effective_lost = resolve_effective_lost_signal(globalLost, cls);
	}
}

void MultiviewWindow::release_source_refs()
{
	/* Collect textures to destroy outside the mutex to avoid
	 * deadlock: render thread holds graphics lock then takes mutex,
	 * so we must never hold mutex while calling obs_enter_graphics(). */
	std::vector<gs_texture_t *> textures_to_destroy;

	/* Phase 3 / M6: external private sources must be released OUTSIDE
	 * source_mutex_ (see plan.md §6 lock order). Move them into a local
	 * vector under lock so the RAII destructors fire after we release. */
	std::vector<OBSSource> externals_to_release;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		for (auto &cs : cell_sources_) {
			if (cs.showing) {
				OBSSourceAutoRelease src = OBSGetStrongRef(cs.weak_ref);
				if (src)
					obs_source_dec_showing(src);
				cs.showing = false;
			}
			cs.weak_ref = nullptr;

			/* Phase 3 / M6: hand off any external private source the
			 * provider runtime may have created. Internal cells leave
			 * `private_source` empty so this is a no-op for them.
			 *
			 * The matching dec_active / dec_showing for the inc pair we
			 * did on create runs OUTSIDE source_mutex_ a few lines
			 * below (deactivate triggers ffmpeg_source's worker-thread
			 * stop / mp_media_set_active(false), which we must not run
			 * while holding our mutex). Move the OBSSource into the
			 * unlock-side vector so the RAII destructor and the dec_*
			 * calls happen in the same scope. */
			if (cs.private_source) {
				externals_to_release.push_back(std::move(cs.private_source));
				cs.private_source = nullptr;
			}
			cs.provider_type = SignalProviderType::Unknown;
			cs.provider_settings_hash = 0;
			cs.last_error_reason.clear();
		}
		cell_sources_.clear();

		/* Release label text sources */
		label_sources_.clear();

		/* Phase 3 / M5: drop shared status overlay text sources alongside
		 * label sources so the lifetime is identical and no text source
		 * outlives the runtime that referenced it. */
		release_status_text_sources();

		/* Collect bg/overlay textures and clear vectors under lock */
		for (auto &bgi : bg_images_) {
			if (bgi.texture)
				textures_to_destroy.push_back(bgi.texture);
			bgi.texture = nullptr;
			bgi.path.clear();
		}
		bg_images_.clear();

		for (auto &oi : overlay_images_) {
			if (oi.texture)
				textures_to_destroy.push_back(oi.texture);
			oi.texture = nullptr;
			oi.path.clear();
		}
		overlay_images_.clear();

		/* Phase 3 / M5.4: release lost-signal images (placeholder /
		 * fallback static) alongside bg/overlay so the texture lifetime
		 * is identical to the rest of the cell visuals. */
		for (auto &li : lost_signal_images_) {
			if (li.texture)
				textures_to_destroy.push_back(li.texture);
			li.texture = nullptr;
			li.path.clear();
		}
		lost_signal_images_.clear();
	}

	/* Destroy textures outside mutex, respecting lock order */
	if (!textures_to_destroy.empty()) {
		obs_enter_graphics();
		for (auto *tex : textures_to_destroy)
			gs_texture_destroy(tex);
		obs_leave_graphics();
	}

	/* Phase 3 / M6: drop external private source strong refs outside
	 * source_mutex_. The RAII destructors fire at scope exit and run the
	 * source destroy chain on this thread; running it without holding
	 * source_mutex_ keeps the render thread free to keep working and
	 * avoids re-entering the mutex through any signal callbacks the
	 * host plugin fires during source destroy.
	 *
	 * Pair-undo the inc_active / inc_showing we did on create. dec_active
	 * triggers ffmpeg_source_deactivate (stops mp_media worker thread)
	 * and the equivalent for ndi/spout/vlc; this is the heavy step that
	 * must not run inside source_mutex_. */
	for (auto &priv : externals_to_release) {
		if (priv) {
			/* Matched dec_active for the inc_active we did on create.
			 * dec_active triggers ffmpeg_source_deactivate (stops
			 * mp_media worker thread), which must run with source_mutex_
			 * already released. */
			obs_source_dec_active(priv);
		}
	}
	externals_to_release.clear();

	/* Release volmeters (no graphics context needed) */
	release_volmeters();

	/* Release scale label sources */
	release_scale_label_sources();

	/* Release safe area vertex buffers */
	obs_enter_graphics();
	release_safe_area_vbs();
	obs_leave_graphics();
}

/* ---- Rendering ---- */

void MultiviewWindow::render_callback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiviewWindow *>(data);
	if (!self->ready_)
		return;
	self->render(cx, cy);
}

void MultiviewWindow::render(uint32_t cx, uint32_t cy)
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	/* Throttle lazy re-resolution: attempt once per re-resolve interval.
	 * Interval is determined by OBS canvas FPS or custom setting. */
	{
		const GlobalSettings &gs = config_->global_settings();
		double intervalFps = gs.reResolveCustomFps;
		if (gs.reResolveInheritObs) {
			struct obs_video_info ovi;
			if (obs_get_video_info(&ovi))
				intervalFps = (double)ovi.fps_num / (double)ovi.fps_den;
		}
		int interval = (int)(intervalFps + 0.5);
		if (interval < 1)
			interval = 1;
		re_resolve_counter_ = (re_resolve_counter_ + 1) % interval;
	}

	/* Detect PGM/PRVW scene changes and rebuild volmeters if needed */
	if (has_pgm_cell_ || has_prvw_cell_)
		check_scene_change_for_volmeters();

	/* Snapshot PGM / PRVW scene trees once per frame for cell-highlight
	 * classification. We always do this (independent of has_pgm_cell_ /
	 * has_prvw_cell_) because a regular "scene" cell can be highlighted
	 * when its source happens to be the current PGM/PRVW scene or is
	 * nested inside one. Studio Mode OFF → prvw_tree_set_ stays empty
	 * → no green borders anywhere, automatically. */
	refresh_highlight_tree_sets();

	/* Handle deferred rebuild requested from non-render threads
	 * (audio_mixers signal) and poll for AutoFollow streaming-track changes
	 * (no event fires on Settings → Output → Streaming Audio Track edits). */
	check_active_track_change();

	/* Compute canvas-aspect-ratio viewport (centered, with black borders) */
	double windowAspect = (double)cx / (double)cy;
	int vpX = 0, vpY = 0;
	int vpW = (int)cx, vpH = (int)cy;

	if (windowAspect > canvas_aspect_) {
		/* Window wider than canvas → pillarbox (black on sides) */
		vpW = (int)((double)cy * canvas_aspect_);
		vpX = ((int)cx - vpW) / 2;
	} else if (windowAspect < canvas_aspect_) {
		/* Window taller than canvas → letterbox (black on top/bottom) */
		vpH = (int)((double)cx / canvas_aspect_);
		vpY = ((int)cy - vpH) / 2;
	}

	/* Recompute layout only when viewport size actually changed */
	if (vpW != cached_vpW_ || vpH != cached_vpH_) {
		engine_.set_layout(layout_);
		engine_.set_viewport(vpW, vpH);
		engine_.compute();
		cached_vpW_ = vpW;
		cached_vpH_ = vpH;
	}

	const auto &cells = engine_.cells();

	/* Draw each cell (offset by vpX, vpY for centering) */
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	/* Fill viewport with gutter color (same as OBS native outerColor) */
	startRegion(vpX, vpY, vpW, vpH, 0.0f, (float)vpW, 0.0f, (float)vpH);
	gs_effect_set_color(colorParam, 0xFF999999);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, vpW, vpH);
	endRegion();

	for (int i = 0; i < (int)cells.size(); i++) {
		const CellRect &cell = cells[i];
		int cellX = cell.x + vpX;
		int cellY = cell.y + vpY;

		/* Determine if FillSignalOnly mode (pillarbox/letterbox stays gutter color) */
		bool signalFillOnly = false;
		if (i < (int)effective_visuals_.size() &&
		    effective_visuals_[i].background.fillMode == BackgroundFillMode::FillSignalOnly) {
			signalFillOnly = true;
		}

		if (!signalFillOnly) {
			/* Fill entire cell with background color (default black) */
			uint32_t cellBg = 0xFF000000;
			if (i < (int)effective_visuals_.size() && effective_visuals_[i].background.colorEnabled)
				cellBg = effective_visuals_[i].background.color;
			startRegion(cellX, cellY, cell.w, cell.h, 0.0f, (float)cell.w, 0.0f, (float)cell.h);
			gs_effect_set_color(colorParam, cellBg);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, cell.w, cell.h);
			endRegion();
		}

		/* Get source for this cell */
		obs_source_t *src = nullptr;
		OBSSourceAutoRelease srcHolder;
		bool isPrvwFallback = false;
		bool isPgm = false;
		bool isFallback = false; /* Phase 3 / M5.4: rendering a Lost-Signal fallback */

		if (i < (int)cell_sources_.size()) {
			const auto &cs = cell_sources_[i];

			/* Phase 3 / M5.1 ClearCell: render the cell as Empty
			 * for the brief window between source_remove (which set
			 * pending_clear) and apply_clear_cell_for_rowcols
			 * running on the Qt main thread. Skip every visual that
			 * depends on the soon-to-be-cleared assignment: source
			 * resolve, fallback, lost-signal image, status overlay
			 * — they're all handled below by the same `src == null
			 * && cell empty` branch the layout already has. */
			if (cs.pending_clear) {
				/* fall through to the "no signal" branch with
				 * src == nullptr; existing code in that branch
				 * paints the cell background only. */
			} else if (cs.provider_type != SignalProviderType::Unknown &&
				   !signal_provider_is_internal(cs.provider_type)) {
				/* Phase 3 / M6 step 9: external-provider cell.
				 *
				 * The provider-owned private OBS source lives in
				 * cs.private_source for the cell's lifetime (created
				 * in update_source_refs(), released in
				 * release_source_refs()). We render it directly
				 * through the same letterbox / overlay pipeline as
				 * internal scene/source cells.
				 *
				 * obs_source_get_ref bumps the strong ref by +1 so
				 * the OBSSourceAutoRelease srcHolder can drop it at
				 * scope exit symmetrically with the other branches.
				 * Returns nullptr if the source is mid-destroy, in
				 * which case the cell just renders its background. */
				if (obs_source_t *raw = cs.private_source.Get()) {
					srcHolder = obs_source_get_ref(raw);
					src = srcHolder;
				}
			} else if (cs.type == "pgm") {
				/* PGM: we use obs_render_main_texture() for
				 * composited output (includes transitions).
				 * Still get current scene to verify non-null. */
				srcHolder = obs_frontend_get_current_scene();
				src = srcHolder;
				isPgm = true;
			} else if (cs.type == "prvw") {
				/* Resolve PRVW fresh each frame */
				srcHolder = obs_frontend_get_current_preview_scene();
				if (!srcHolder) {
					/* No Studio Mode → fallback to PGM */
					srcHolder = obs_frontend_get_current_scene();
					isPrvwFallback = (srcHolder != nullptr);
				}
				src = srcHolder;
			} else if (!cs.type.empty()) {
				/* scene/source: use cached ref, or lazy re-resolve */
				if (cs.weak_ref)
					srcHolder = OBSGetStrongRef(cs.weak_ref);
				if (!srcHolder && !cs.name.empty() && re_resolve_counter_ == 0) {
					/* Source may have been re-added (undo) - throttled */
					obs_source_t *resolved = obs_get_source_by_name(cs.name.c_str());
					if (resolved) {
						cell_sources_[i].weak_ref = OBSGetWeakRef(resolved);
						obs_source_inc_showing(resolved);
						cell_sources_[i].showing = true;
						srcHolder = resolved;
						obs_source_release(resolved);
						/* Phase 3 / M5: source just came back (e.g. user undo).
						 * Kick the volmeter rebuild so audio metering re-attaches
						 * on the very next frame instead of waiting up to 1s for
						 * the active-source poll. */
						volmeters_rebuild_requested_.store(true, std::memory_order_release);
					}
				}
				src = srcHolder;
			}

			/* Phase 3 / M5.4: hard guard against rendering a source that
			 * has been marked removed but is still reachable through our
			 * weak ref / a frontend handle. obs_source_video_render() and
			 * obs_render_main_texture() both descend into scene_video_render
			 * → update_transforms_and_prune_sources, which fires sceneitem
			 * signal handlers belonging to other plugins. streamdeck-plugin-obs
			 * has been observed to crash here (signal_handler_signal +0x122)
			 * when the scene was just removed via our context menu. Treat the
			 * cell as missing for this frame; the fallback block below will
			 * substitute a safe replacement source. */
			if (src && obs_source_removed(src)) {
				src = nullptr;
				srcHolder = nullptr;
				isPgm = false;
				isPrvwFallback = false;
			}

			/* Phase 3 / M6.6: probe external-provider health up-front so
			 * the drop-src and state-classification blocks below can
			 * both consume *this frame's* verdict. The original
			 * structure deferred the probe to the state-classification
			 * block, which made drop-src work off the previous frame's
			 * verdict (cs.last_health_state). That stale verdict caused
			 * a one-frame fallback flash on recovery: drop-src fired
			 * because last_health_state==Connecting, fallback was
			 * substituted, then the probe ran and reported Active \u2014
			 * but the frame had already been painted with fallback. */
			SignalRuntimeState current_supervisor_state = cs.last_health_state;
			bool current_supervisor_state_known = false;
			if (cs.provider_type != SignalProviderType::Unknown &&
			    !signal_provider_is_internal(cs.provider_type)) {
				const uint64_t now_health_ns_pre = os_gettime_ns();
				const uint64_t kProbeIntervalNs_pre = 1'000'000'000ULL;
				if (now_health_ns_pre - cs.last_health_ns >= kProbeIntervalNs_pre) {
					cell_sources_[i].last_health_ns = now_health_ns_pre;
					current_supervisor_state = tick_external_cell_health(
						i, cell.gridRow, cell.gridCol, now_health_ns_pre);
				} else {
					current_supervisor_state =
						(cs.last_health_state == SignalRuntimeState::Empty && src)
							? SignalRuntimeState::Connecting
							: cs.last_health_state;
				}
				cell_sources_[i].last_health_state = current_supervisor_state;
				current_supervisor_state_known = true;

				/* Phase 3 / M6.6: sticky fallback latch.
				 *
				 * Without this latch, every supervisor recreate
				 * cycle (Lost -> RetryScheduled -> Connecting ->
				 * Lost -> ...) would briefly drop the fallback
				 * during the Connecting window: the freshly
				 * recreated source reports OPENING for a few
				 * hundred ms before ffmpeg/vlc realize the URL is
				 * still bad and report ENDED again. While
				 * Connecting is technically "unhealthy" and IS in
				 * the drop-src set, the visual effect of cycling
				 * fallback off and on every retry feels broken to
				 * users.
				 *
				 * Latch: once Lost/Error/RetryScheduled is seen,
				 * we stay in fallback until Active. Connecting is
				 * not enough to clear \u2014 a real recovery flips to
				 * Active immediately when the first frame arrives. */
				if (current_supervisor_state == SignalRuntimeState::Lost ||
				    current_supervisor_state == SignalRuntimeState::Error ||
				    current_supervisor_state == SignalRuntimeState::RetryScheduled) {
					cell_sources_[i].fallback_latched = true;
				} else if (current_supervisor_state == SignalRuntimeState::Active) {
					cell_sources_[i].fallback_latched = false;
				}
				/* Connecting / Paused / Empty: leave the latch
				 * unchanged. */
			}

			/* Phase 3 / M6.6: extend M5 fallback to external providers.
			 *
			 * External cells own a `private_source` that stays alive
			 * across SIGNAL LOST (NDI receivers and Spout receivers
			 * recover automatically when the sender returns; tearing
			 * down on every loss would just churn sockets / shared
			 * textures). That means src != nullptr even when the
			 * cell is showing a black frame, so the existing
			 * `if (!src && ...)` fallback gate below never fires.
			 *
			 * Solution: when the supervisor's most recent verdict is
			 * unhealthy (Lost / Error / Connecting / RetryScheduled)
			 * AND the user configured RetryWithFallback, drop the
			 * src ref here so the unified fallback resolver can
			 * substitute the user's chosen PGM / PRVW / Scene /
			 * Source. The supervisor still owns recovery; this only
			 * changes what the cell paints in the meantime.
			 *
			 * The probe_health override per provider already covers
			 * the "src alive but no frames" case for NDI / Spout
			 * (zero-dim + 5 s grace) and for FFmpeg / VLC (media
			 * state machine), so cs.state is the authoritative
			 * signal here. We deliberately do NOT also gate on
			 * obs_source_get_width()==0 because that would cause
			 * the fallback to flicker during normal startup before
			 * the first frame arrives. */
			if (src && cs.provider_type != SignalProviderType::Unknown &&
			    !signal_provider_is_internal(cs.provider_type)) {
				const ExternalLostBehavior beh = cs.effective_lost.externalLostBehavior;
				const bool wants_fallback = (beh == ExternalLostBehavior::RetryWithFallback &&
							     !cs.effective_lost.fallbackType.empty());
				/* Use the sticky fallback_latched flag instead of
				 * the raw supervisor verdict: once Lost, stay in
				 * fallback until Active recovers. Connecting alone
				 * (which fires every recreate) does not clear the
				 * latch, so the user sees a continuous fallback
				 * during the entire retry cycle instead of
				 * fallback-off-fallback-off flicker. */
				if (wants_fallback && cs.fallback_latched) {
					src = nullptr;
					srcHolder = nullptr;
				}
			}

			/* Phase 3 / M5.4: apply Lost-Signal fallback when the primary
			 * source is missing or just got pruned above. Image / placeholder
			 * variants need their own texture loader (deferred to the same
			 * round that wires up bg_images_-style four-stage loading); only
			 * OBS-source fallbacks (pgm/prvw/scene/source) are wired here.
			 *
			 * Skipped for cs.pending_clear cells \u2014 those are about to be
			 * cleared by the queued main-thread mutation, so showing any
			 * fallback would just flicker for one frame before going Empty.
			 *
			 * Eligibility:
			 *   - internal cells: any cell with a configured type (the
			 *     classic M5 path: PGM/PRVW are never null but a removed
			 *     scene/source can null out src above);
			 *   - external cells: handled by the dedicated drop-src
			 *     block above; once src is null here the same fallback
			 *     resolver runs uniformly. */
			const bool fallback_eligible = (!cs.type.empty()) ||
						       (cs.provider_type != SignalProviderType::Unknown &&
							!signal_provider_is_internal(cs.provider_type));
			if (!src && fallback_eligible && !cs.pending_clear) {
				const LostSignalSettings &eff = cs.effective_lost;
				const std::string &ft = eff.fallbackType;
				if (ft == "pgm") {
					srcHolder = obs_frontend_get_current_scene();
					if (srcHolder && !obs_source_removed(srcHolder)) {
						src = srcHolder;
						isPgm = true;
						isFallback = true;
					} else {
						srcHolder = nullptr;
					}
				} else if (ft == "prvw") {
					srcHolder = obs_frontend_get_current_preview_scene();
					if (!srcHolder)
						srcHolder = obs_frontend_get_current_scene();
					if (srcHolder && !obs_source_removed(srcHolder)) {
						src = srcHolder;
						isFallback = true;
					} else {
						srcHolder = nullptr;
					}
				} else if ((ft == "scene" || ft == "source") && !eff.fallbackName.empty()) {
					srcHolder = obs_get_source_by_name(eff.fallbackName.c_str());
					if (srcHolder && !obs_source_removed(srcHolder)) {
						src = srcHolder;
						isFallback = true;
					} else {
						srcHolder = nullptr;
					}
				}
				/* "image" and "" leave src null — handled by future
				 * placeholder/signal-lost image renderer; for now the
				 * cell falls back to the existing MISSING SOURCE band. */
			}

			/* Phase 3 / M5.1a / M5.4: runtime state classification.
			 * FallbackActive takes precedence over MissingInternal so the
			 * status overlay logic and Reconnect Now eligibility map to
			 * the correct visual treatment. */
			SignalRuntimeState newState = cs.state;
			if (cs.pending_clear) {
				/* Phase 3 / M5.1 ClearCell: cell is being cleared on
				 * the next Qt main-thread tick. Treat as Empty so no
				 * MISSING SOURCE / FALLBACK overlay flashes during
				 * the gap. */
				newState = SignalRuntimeState::Empty;
			} else if (cs.provider_type != SignalProviderType::Unknown &&
				   !signal_provider_is_internal(cs.provider_type)) {
				/* Phase 3 / M6.6: the supervisor probe ran earlier this
				 * frame (see current_supervisor_state above) so the
				 * fallback substitution and this state classification
				 * both consume the same fresh verdict. If for any reason
				 * the probe didn't run (provider_type became Unknown
				 * mid-frame, etc.), fall back to the sticky last verdict. */
				const SignalRuntimeState supervisor_state = current_supervisor_state_known
										    ? current_supervisor_state
										    : cs.last_health_state;
				/* Phase 3 / M6.6: sticky display state during retry.
				 *
				 * The supervisor cycles every external cell through
				 * Lost -> RetryScheduled -> Connecting -> Lost
				 * during a failing retry loop (URL unreachable,
				 * sender absent). Mapping that raw verdict to the
				 * status overlay 1-for-1 produced visible flicker:
				 * SIGNAL LOST -> CONNECTING flash -> SIGNAL LOST
				 * every retry attempt.
				 *
				 * Sticky policy: once the user-visible failure is
				 * latched, hold ONE chosen overlay until the
				 * supervisor really reports Active. The choice of
				 * overlay is driven by the user's ExternalLostBehavior:
				 *
				 *   - SignalLostOverlay / SignalLostImage:
				 *       hold SIGNAL LOST (red band). The user
				 *       explicitly asked to see the lost state.
				 *   - RetryWithFallback:
				 *       hold FallbackActive (yellow FALLBACK band).
				 *       Implicitly when isFallback (source-type
				 *       fallback resolved), or when image fallback
				 *       is configured (image renderer paints the
				 *       art, status reads FALLBACK).
				 *   - RetryOnly:
				 *       hold Connecting (blue CONNECTING... band).
				 *       No fallback configured \u2014 the only useful
				 *       hint is that retry is in progress.
				 *
				 * The sticky window starts at the first Lost / Error
				 * / RetryScheduled verdict and ends the frame the
				 * supervisor returns Active (when the source truly
				 * has frames). */
				SignalRuntimeState display_state;
				const auto beh = cs.effective_lost.externalLostBehavior;
				const auto &eff = cs.effective_lost;
				const bool image_configured =
					(beh == ExternalLostBehavior::RetryWithFallback &&
					 eff.fallbackType == std::string("image") && !eff.fallbackName.empty()) ||
					(beh == ExternalLostBehavior::SignalLostImage &&
					 !eff.signalLostImagePath.empty());
				if (isFallback) {
					display_state = SignalRuntimeState::FallbackActive;
				} else if (cs.fallback_latched && supervisor_state != SignalRuntimeState::Active) {
					switch (beh) {
					case ExternalLostBehavior::RetryWithFallback:
						display_state = image_configured
									? SignalRuntimeState::FallbackActive
									: SignalRuntimeState::Lost;
						break;
					case ExternalLostBehavior::SignalLostOverlay:
					case ExternalLostBehavior::SignalLostImage:
						display_state = SignalRuntimeState::Lost;
						break;
					case ExternalLostBehavior::RetryOnly:
					default:
						display_state = SignalRuntimeState::Connecting;
						break;
					}
				} else {
					display_state = supervisor_state;
				}
				newState = display_state;
			} else if (cs.type.empty()) {
				newState = SignalRuntimeState::Empty;
			} else if (isFallback) {
				newState = SignalRuntimeState::FallbackActive;
			} else if (cs.type == "pgm" || cs.type == "prvw") {
				newState = src ? SignalRuntimeState::Active : SignalRuntimeState::MissingInternal;
			} else if (src) {
				newState = SignalRuntimeState::Active;
			} else {
				newState = SignalRuntimeState::MissingInternal;
			}
			if (newState != cs.state) {
				/* Phase 3 / M5: any cross between Active and a non-Active
				 * state changes which sources should be metered. Kick a
				 * deferred volmeter rebuild so the next frame on the
				 * render thread re-attaches — the 1Hz active-source poll
				 * can miss this when a removed scene's audio sources are
				 * still reachable through PGM (e.g. global mic). */
				const bool was_active = cs.state == SignalRuntimeState::Active;
				const bool now_active = newState == SignalRuntimeState::Active;
				if (was_active != now_active)
					volmeters_rebuild_requested_.store(true, std::memory_order_release);
				cell_sources_[i].state = newState;

				/* Phase 3 / M6.6: external cell state transitions
				 * (Active -> Lost / Lost -> Active) change which
				 * lost-image path compute_wanted_lost_image_path
				 * returns for this cell. The image loader pipeline
				 * lives on the Qt main thread and is normally
				 * triggered by source-removed signals or settings
				 * mutations \u2014 supervisor verdict transitions are
				 * silent on those channels. Kick rebuild_lost_signal_images
				 * via QTimer::singleShot so the loader picks up the
				 * new fallback / signal-lost path before the next
				 * frame paints. Internal cells get this rebuild via
				 * the source_remove signal handler already. */
				const bool external_cell = cs.provider_type != SignalProviderType::Unknown &&
							   !signal_provider_is_internal(cs.provider_type);
				if (external_cell) {
					QTimer::singleShot(0, this, [this]() { rebuild_lost_signal_images(); });
				}
			}
			if (newState == SignalRuntimeState::Active) {
				cell_sources_[i].last_active_ns = os_gettime_ns();
				cell_sources_[i].retry_attempt = 0;
			}
		}

		/* Signal rect: will be set if source exists, otherwise defaults to cell rect */
		int vrX = cellX, vrY = cellY, vrW = cell.w, vrH = cell.h;
		bool hasSignalRect = false;

		if (src) {
			/* Determine source dimensions for letterbox */
			uint32_t srcW, srcH;
			if (isPgm) {
				/* PGM uses canvas resolution (composited output) */
				struct obs_video_info ovi;
				obs_get_video_info(&ovi);
				srcW = ovi.base_width;
				srcH = ovi.base_height;
			} else {
				srcW = obs_source_get_width(src);
				srcH = obs_source_get_height(src);
			}

			if (srcW == 0 || srcH == 0) {
				/* Phase 3 / M6 step 10: source resolved but produces
				 * no frame yet (or has stopped producing frames).
				 * Common for:
				 *   - external cells where DistroAV cleared content
				 *     on disconnect (Clear content default);
				 *   - FFmpeg cells with an unreachable URL;
				 *   - sources still in their initial open phase.
				 *
				 * Treat the cell as no-signal for the rest of this
				 * frame's render: drop the source ref so the live
				 * video path below is skipped, but DO let the cell
				 * background, bg image, and lost-signal image
				 * render. The supervisor's state classification
				 * (Connecting / Lost / Error) drives the status
				 * overlay (RECONNECTING / SIGNAL LOST band) which
				 * is rendered unconditionally after this block. */
				src = nullptr;
				srcHolder = nullptr;
				isPgm = false;
				isPrvwFallback = false;
				isFallback = false;
				goto render_no_signal;
			}

			/* Determine content area (may be reduced for Below label mode) */
			int contentX = cellX;
			int contentY = cellY;
			int contentW = cell.w;
			int contentH = cell.h;

			if (i < (int)effective_visuals_.size() &&
			    effective_visuals_[i].label.displayMode == LabelDisplayMode::Below) {
				/* Reserve bottom portion for label + gutter separator */
				int labelRegionH = cell.h / 6;
				if (labelRegionH < 16)
					labelRegionH = 16;
				int gutterH = gutter_px_;
				contentH = cell.h - labelRegionH - gutterH;
				if (contentH < 16)
					contentH = 16;
			}

			/* Calculate letterbox/pillarbox rect within content area */
			double srcAspect = (double)srcW / (double)srcH;
			double contentAspect = (double)contentW / (double)contentH;

			if (srcAspect > contentAspect) {
				vrW = contentW;
				vrH = (int)((double)contentW / srcAspect + 0.5);
				vrX = contentX;
				vrY = contentY + (contentH - vrH) / 2;
			} else {
				vrH = contentH;
				vrW = (int)((double)contentH * srcAspect + 0.5);
				vrX = contentX + (contentW - vrW) / 2;
				vrY = contentY;
			}

			/* Snap to fill: if letterbox/pillarbox is tiny (<=16px total),
			 * skip it and stretch to fill content area (like OBS native).
			 *
			 * Why 16: layout-engine integer-divides the window width by
			 * the column count, so neighboring cells in the same row can
			 * differ in width by 1 px (e.g. 591 vs 592 for a 9-col grid).
			 * That 1 px ripples through the aspect math: a 16:9 source
			 * (1.7778) inside cell.h=328 needs vrW=583, leaving residual
			 * 8 in the 591 cell and 9 in the 592 cell. With the original
			 * threshold of 8, the 591 cell snapped (no pillarbox) while
			 * the 592 cell pillarboxed \u2014 visible row-wise inconsistency.
			 *
			 * 16 covers the worst-case grid rounding (a few px per cell)
			 * plus a small invisible-stretch margin (~2.7% on a 600 px
			 * cell), and stays well below any genuine aspect mismatch
			 * (4:3 source in 16:9 cell residuals are in the 100s of px). */
			constexpr int SNAP_THRESHOLD = 16;
			bool snapped = false;
			if ((contentW - vrW) <= SNAP_THRESHOLD && (contentH - vrH) <= SNAP_THRESHOLD) {
				vrX = contentX;
				vrY = contentY;
				vrW = contentW;
				vrH = contentH;
				snapped = true;
			}

			/* Phase 3 / M6.6 fill diagnostic: emit one [fill] line per
			 * unique (provider, srcW, srcH, cellW, cellH) tuple. Lets
			 * us correlate visible pillarbox / letterbox surprises with
			 * the actual numbers the renderer is consuming, including
			 * cases where the source advertises non-canvas dimensions
			 * (NDI Output Scaled resolution, HLS variant playlists,
			 * etc.). One-shot per tuple keeps log volume bounded even
			 * when the window is resized live. */
			if (i < (int)cell_sources_.size() &&
			    cell_sources_[i].provider_type != SignalProviderType::Unknown &&
			    !signal_provider_is_internal(cell_sources_[i].provider_type)) {
				const uint64_t h = ((uint64_t)srcW << 48) ^ ((uint64_t)srcH << 32) ^
						   ((uint64_t)(uint32_t)cell.w << 16) ^ (uint64_t)(uint32_t)cell.h;
				if (cell_sources_[i].fill_log_hash != h) {
					cell_sources_[i].fill_log_hash = h;
					obs_log(LOG_INFO,
						"%s[fill] cell (%d,%d) provider=%s src=%ux%u (%.4f) cell=%dx%d "
						"content=%dx%d (%.4f) vr=%dx%d snap=%s",
						log_prefix().c_str(), cell.gridRow, cell.gridCol,
						signal_provider_to_string(cell_sources_[i].provider_type), srcW, srcH,
						srcAspect, cell.w, cell.h, contentW, contentH, contentAspect, vrW, vrH,
						snapped ? "yes" : "no");
				}
			}

			hasSignalRect = true;

			/* Draw background fill for FillSignalOnly mode:
			 * Only fill the signal rect area; pillarbox/letterbox stays gutter color. */
			if (signalFillOnly) {
				uint32_t bgColor = 0xFF000000;
				if (i < (int)effective_visuals_.size() && effective_visuals_[i].background.colorEnabled)
					bgColor = effective_visuals_[i].background.color;
				startRegion(vrX, vrY, vrW, vrH, 0.0f, (float)vrW, 0.0f, (float)vrH);
				gs_effect_set_color(colorParam, bgColor);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, vrW, vrH);
				endRegion();
			}

			/* Draw background image if available */
			if (i < (int)bg_images_.size() && bg_images_[i].texture) {
				gs_texture_t *tex = bg_images_[i].texture;
				uint32_t imgW = bg_images_[i].width;
				uint32_t imgH = bg_images_[i].height;
				if (imgW > 0 && imgH > 0) {
					/* Determine target rect based on fill mode */
					const BackgroundSettings *bgS = i < (int)effective_visuals_.size()
										? &effective_visuals_[i].background
										: nullptr;
					int tgtX, tgtY, tgtW, tgtH;
					if (bgS && bgS->fillMode == BackgroundFillMode::FillSignalOnly) {
						tgtX = vrX;
						tgtY = vrY;
						tgtW = vrW;
						tgtH = vrH;
					} else {
						tgtX = cellX;
						tgtY = cellY;
						tgtW = cell.w;
						tgtH = cell.h;
					}

					int drawW, drawH, drawX, drawY;
					if (bgS && bgS->imageFitMode == ImageFitMode::Stretch) {
						drawX = tgtX;
						drawY = tgtY;
						drawW = tgtW;
						drawH = tgtH;
					} else {
						/* Fit: maintain aspect ratio */
						double imgAspect = (double)imgW / (double)imgH;
						double tgtAspect = (double)tgtW / (double)tgtH;
						if (imgAspect > tgtAspect) {
							drawW = tgtW;
							drawH = (int)((double)tgtW / imgAspect + 0.5);
							drawX = tgtX;
							drawY = tgtY + (tgtH - drawH) / 2;
						} else {
							drawH = tgtH;
							drawW = (int)((double)tgtH * imgAspect + 0.5);
							drawX = tgtX + (tgtW - drawW) / 2;
							drawY = tgtY;
						}
					}
					gs_effect_t *defEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					gs_eparam_t *imgParam = gs_effect_get_param_by_name(defEffect, "image");
					gs_effect_set_texture(imgParam, tex);
					startRegion(drawX, drawY, drawW, drawH, 0.0f, (float)imgW, 0.0f, (float)imgH);
					while (gs_effect_loop(defEffect, "Draw"))
						gs_draw_sprite(tex, 0, imgW, imgH);
					endRegion();
				}
			}

			/* Render into video rect */
			startRegion(vrX, vrY, vrW, vrH, 0.0f, (float)srcW, 0.0f, (float)srcH);
			if (isPgm) {
				obs_render_main_texture();
			} else if (src && !obs_source_removed(src)) {
				/* Phase 3 / M5.4: redundant-but-cheap removed check
				 * just before video_render. Source state can flip
				 * between resolve and render even within one frame
				 * (other plugins / scripts can mark it removed). */
				obs_source_video_render(src);
				if (i < (int)cell_sources_.size())
					cell_sources_[i].render_calls++;
			}
			endRegion();

			/* Draw PRVW fallback indicator (yellow bar at bottom) */
			if (isPrvwFallback) {
				int barH = (std::max)(2, cell.h / 20);
				startRegion(cellX, cellY + cell.h - barH, cell.w, barH, 0.0f, (float)cell.w, 0.0f,
					    (float)barH);
				/* Yellow with some transparency: 0xCCFFD400 (ARGB) */
				gs_effect_set_color(colorParam, 0xCCFFD400);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, cell.w, barH);
				endRegion();
			}
		} else {
			/* Empty cell / no signal - leave as gutter/window background.
			 * Only draw background image if one is configured.
			 *
			 * Phase 3 / M6 step 10: also reachable via the goto from
			 * the if(src) branch when srcW/srcH are zero (source open
			 * but not producing frames). The cell background fill
			 * already painted earlier in the loop covers the canvas. */
		render_no_signal:
			if (i < (int)bg_images_.size() && bg_images_[i].texture) {
				gs_texture_t *tex = bg_images_[i].texture;
				uint32_t imgW = bg_images_[i].width;
				uint32_t imgH = bg_images_[i].height;
				if (imgW > 0 && imgH > 0) {
					const BackgroundSettings *bgS = i < (int)effective_visuals_.size()
										? &effective_visuals_[i].background
										: nullptr;
					/* Empty cell has no signal rect, always use cell rect */
					int tgtX = cellX, tgtY = cellY, tgtW = cell.w, tgtH = cell.h;

					int drawW, drawH, drawX, drawY;
					if (bgS && bgS->imageFitMode == ImageFitMode::Stretch) {
						drawX = tgtX;
						drawY = tgtY;
						drawW = tgtW;
						drawH = tgtH;
					} else {
						double imgAspect = (double)imgW / (double)imgH;
						double tgtAspect = (double)tgtW / (double)tgtH;
						if (imgAspect > tgtAspect) {
							drawW = tgtW;
							drawH = (int)((double)tgtW / imgAspect + 0.5);
							drawX = tgtX;
							drawY = tgtY + (tgtH - drawH) / 2;
						} else {
							drawH = tgtH;
							drawW = (int)((double)tgtH * imgAspect + 0.5);
							drawX = tgtX + (tgtW - drawW) / 2;
							drawY = tgtY;
						}
					}
					gs_effect_t *defEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					gs_eparam_t *imgParam = gs_effect_get_param_by_name(defEffect, "image");
					gs_effect_set_texture(imgParam, tex);
					startRegion(drawX, drawY, drawW, drawH, 0.0f, (float)imgW, 0.0f, (float)imgH);
					while (gs_effect_loop(defEffect, "Draw"))
						gs_draw_sprite(tex, 0, imgW, imgH);
					endRegion();
				}
			}

			/* Phase 3 / M5.4: render placeholder / fallback static image
			 * on top of the optional bg image but below status overlay.
			 * No-op when the cell's configured lost-signal path is empty
			 * or didn't load. */
			render_lost_signal_image(i, cellX, cellY, cell.w, cell.h);
		}

		/* Render safe area guides (anchored to SignalRect, after video, before overlay) */
		if (hasSignalRect)
			render_safe_area(i, vrX, vrY, vrW, vrH);

		/* (PGM/PRVW highlight borders are rendered in two post-loop passes
		 * below so PGM (red) always paints on top of PRVW (green) even when
		 * the two cells are adjacent / share a gutter edge, and so the
		 * border sits on top of label & VU overlays — which is important in
		 * gutter == 0 layouts where the border is drawn INSIDE the cell.) */

		/* Render foreground overlay image if available */
		if (i < (int)overlay_images_.size() && overlay_images_[i].texture) {
			const OverlaySettings *ovl = nullptr;
			if (i < (int)effective_visuals_.size())
				ovl = &effective_visuals_[i].overlay;

			if (ovl && ovl->enabled && ovl->opacity > 0.0) {
				gs_texture_t *tex = overlay_images_[i].texture;
				uint32_t imgW = overlay_images_[i].width;
				uint32_t imgH = overlay_images_[i].height;

				/* Determine anchor rect based on anchorMode */
				int anchorX, anchorY, anchorW, anchorH;
				if (ovl->anchorMode == OverlayAnchorMode::Signal && hasSignalRect) {
					anchorX = vrX;
					anchorY = vrY;
					anchorW = vrW;
					anchorH = vrH;
				} else {
					anchorX = cellX;
					anchorY = cellY;
					anchorW = cell.w;
					anchorH = cell.h;
				}

				if (imgW > 0 && imgH > 0 && anchorW > 0 && anchorH > 0) {
					int drawX, drawY, drawW, drawH;
					if (ovl->fitMode == OverlayFitMode::Stretch) {
						drawX = anchorX;
						drawY = anchorY;
						drawW = anchorW;
						drawH = anchorH;
					} else {
						/* Fit: preserve aspect ratio */
						double imgAspect = (double)imgW / (double)imgH;
						double aAspect = (double)anchorW / (double)anchorH;
						if (imgAspect > aAspect) {
							drawW = anchorW;
							drawH = (int)((double)anchorW / imgAspect + 0.5);
							drawX = anchorX;
							drawY = anchorY + (anchorH - drawH) / 2;
						} else {
							drawH = anchorH;
							drawW = (int)((double)anchorH * imgAspect + 0.5);
							drawX = anchorX + (anchorW - drawW) / 2;
							drawY = anchorY;
						}
					}

					/* Apply opacity via color multiplier */
					uint32_t alpha = (uint32_t)(ovl->opacity * 255.0 + 0.5);
					if (alpha > 255)
						alpha = 255;
					uint32_t overlayColor = (alpha << 24) | 0x00FFFFFF;

					gs_effect_t *defEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					gs_eparam_t *imgParam = gs_effect_get_param_by_name(defEffect, "image");
					gs_effect_set_texture(imgParam, tex);
					gs_eparam_t *clrParam = gs_effect_get_param_by_name(defEffect, "color");
					if (clrParam) {
						struct vec4 clrVec;
						vec4_from_rgba(&clrVec, overlayColor);
						gs_effect_set_vec4(clrParam, &clrVec);
					}

					gs_blend_state_push();
					gs_enable_blending(true);
					gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

					startRegion(drawX, drawY, drawW, drawH, 0.0f, (float)imgW, 0.0f, (float)imgH);
					while (gs_effect_loop(defEffect, "Draw"))
						gs_draw_sprite(tex, 0, imgW, imgH);
					endRegion();

					gs_blend_state_pop();
				}
			}
		}

		/* Fill label region background (Below mode only, when labelRegionFill enabled) */
		if (i < (int)effective_visuals_.size() &&
		    effective_visuals_[i].label.displayMode == LabelDisplayMode::Below &&
		    effective_visuals_[i].background.labelRegionFill) {
			int labelRegionH = cell.h / 6;
			if (labelRegionH < 16)
				labelRegionH = 16;
			int gutterH = gutter_px_;
			int labelY = cellY + cell.h - labelRegionH;
			/* Fill the label row with bgColor (or default black if color not enabled) */
			uint32_t bgColor = effective_visuals_[i].background.colorEnabled
						   ? effective_visuals_[i].background.color
						   : 0xFF000000;
			startRegion(cellX, labelY, cell.w, labelRegionH, 0.0f, (float)cell.w, 0.0f,
				    (float)labelRegionH);
			gs_effect_set_color(colorParam, bgColor);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, cell.w, labelRegionH);
			endRegion();
			(void)gutterH;
		}

		/* Render label overlay */
		render_label(i, cell, vpX, vpY);

		/* Phase 3 / M5: status overlay (Missing Source for now). Rendered
		 * after label so a Below-mode label area never gets covered, and
		 * before VU meter / highlight which are intentionally on top. */
		render_status_overlay(i, cellX, cellY, cell.w, cell.h);

		/* Render VU meter bars */
		render_vu_meter(i, cell, vpX, vpY, vrX, vrY, vrW, vrH);
	}

	/* ---- PGM / PRVW highlight pass (post-cell, two layers) ----
	 *
	 * Done outside the per-cell loop so we can control layering precisely:
	 *
	 *   Pass 1 — draw all PRVW (green) borders
	 *   Pass 2 — draw all PGM  (red)   borders   ← always on top
	 *
	 * This guarantees PGM > PRVW visually even when two highlighted cells
	 * are adjacent (or diagonally touching), where the second cell's border
	 * would otherwise paint over the first's in the shared gutter zone.
	 *
	 * It also means highlight is the LAST thing drawn into each cell rect,
	 * which makes the gutter == 0 inset-border mode visible on top of any
	 * label / VU meter that happens to sit at the cell edge.
	 *
	 * compute_cell_highlight() walks the PGM/PRVW tree sets and is cheap
	 * but non-trivial; cache the per-cell kind once instead of recomputing
	 * in both passes. */
	std::vector<HighlightKind> cellKinds(cells.size(), HighlightKind::None);
	for (int i = 0; i < (int)cells.size(); i++) {
		if (i >= (int)effective_visuals_.size())
			continue;
		cellKinds[i] = compute_cell_highlight(i);
	}
	auto render_highlight_pass = [&](bool pgmPass) {
		for (int i = 0; i < (int)cells.size(); i++) {
			HighlightKind hk = cellKinds[i];
			if (hk == HighlightKind::None)
				continue;
			bool isPgm = (hk == HighlightKind::PgmDirect || hk == HighlightKind::PgmNested);
			if (pgmPass != isPgm)
				continue;
			render_cell_highlight(cells[i], vpX, vpY, hk, effective_visuals_[i].highlight);
		}
	};
	render_highlight_pass(false); /* PRVW first */
	render_highlight_pass(true);  /* PGM on top */
}

/* ---- Label rendering ---- */

void MultiviewWindow::closeEvent(QCloseEvent *event)
{
	/* Stop rendering and release all source refs immediately.
	 * This ensures obs_source_dec_showing is called, which stops
	 * screen capture sources from signaling the OS (yellow border). */
	ready_ = false;
	destroy_display();
	release_source_refs();

	emit window_closed(uuid_);
	event->accept();
	hide();
}

void MultiviewWindow::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	if (display_created_) {
		QSize size = GetPixelSize(this);
		obs_display_resize(display_, size.width(), size.height());
	}
}

void MultiviewWindow::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::RightButton) {
		/* Hit test to determine which cell was clicked */
		QSize pixelSize = GetPixelSize(this);
		float ratio = (float)pixelSize.width() / (float)width();
		int mx = (int)(event->position().x() * ratio);
		int my = (int)(event->position().y() * ratio);

		/* Account for canvas aspect ratio viewport offset */
		int totalW = pixelSize.width();
		int totalH = pixelSize.height();
		double windowAspect = (double)totalW / (double)totalH;
		int vpX = 0, vpY = 0;
		int vpW = totalW, vpH = totalH;

		if (windowAspect > canvas_aspect_) {
			vpW = (int)((double)totalH * canvas_aspect_);
			vpX = (totalW - vpW) / 2;
		} else if (windowAspect < canvas_aspect_) {
			vpH = (int)((double)totalW / canvas_aspect_);
			vpY = (totalH - vpH) / 2;
		}

		/* Translate mouse to layout-local coordinates */
		int lx = mx - vpX;
		int ly = my - vpY;

		engine_.set_layout(layout_);
		engine_.set_viewport(vpW, vpH);
		engine_.compute();

		auto hit = engine_.hit_test(lx, ly);
		int cellIndex = -1;
		if (hit && hit->type == HitType::Cell)
			cellIndex = hit->cellIndex;

		show_context_menu(event->globalPosition().toPoint(), cellIndex);
	}

	QWidget::mousePressEvent(event);
}

/* ---- Context Menu ---- */

void MultiviewWindow::show_context_menu(const QPoint &pos, int cellIndex)
{
	QMenu menu(this);

	/* Fullscreen */
	QAction *fullscreenAction = menu.addAction(QStringLiteral("Fullscreen"));
	fullscreenAction->setCheckable(true);
	fullscreenAction->setChecked(isFullScreen());
	connect(fullscreenAction, &QAction::triggered, this, &MultiviewWindow::on_toggle_fullscreen);

	/* Always on top */
	QAction *onTopAction = menu.addAction(QStringLiteral("Always on Top"));
	onTopAction->setCheckable(true);
	onTopAction->setChecked(is_always_on_top_);
	connect(onTopAction, &QAction::triggered, this, &MultiviewWindow::on_toggle_always_on_top);

	/* Safe Area toggle (instance-level) */
	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			bool safeEnabled = false;
			/* Check instance effective safe area state */
			if (inst->visualSettings.safeAreaMode == InheritanceMode::Override)
				safeEnabled = inst->visualSettings.safeArea.enabled;
			else
				safeEnabled = config_->global_settings().visualSettings.safeArea.enabled;

			QAction *safeAreaAction = menu.addAction(QStringLiteral("Safe Area"));
			safeAreaAction->setCheckable(true);
			safeAreaAction->setChecked(safeEnabled);
			connect(safeAreaAction, &QAction::triggered, this, [this, safeEnabled]() {
				MultiviewInstance *inst = config_->find_instance(uuid_);
				if (!inst)
					return;
				/* Switch to override mode and toggle */
				inst->visualSettings.safeAreaMode = InheritanceMode::Override;
				inst->visualSettings.safeArea.enabled = !safeEnabled;
				config_->save();
				refresh_visual_settings();
			});
		}
	}

	/* VU Meter toggle (instance-level) */
	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			bool vuEnabled = false;
			if (inst->visualSettings.vuMeterMode == InheritanceMode::Override)
				vuEnabled = inst->visualSettings.vuMeter.enabled;
			else
				vuEnabled = config_->global_settings().visualSettings.vuMeter.enabled;

			QAction *vuAction = menu.addAction(QStringLiteral("VU Meter"));
			vuAction->setCheckable(true);
			vuAction->setChecked(vuEnabled);
			connect(vuAction, &QAction::triggered, this, [this, vuEnabled]() {
				MultiviewInstance *inst = config_->find_instance(uuid_);
				if (!inst)
					return;
				inst->visualSettings.vuMeterMode = InheritanceMode::Override;
				inst->visualSettings.vuMeter.enabled = !vuEnabled;
				config_->save();
				refresh_visual_settings();
			});
		}
	}

	/* Cell Display Settings (per-cell) */
	if (cellIndex >= 0) {
		QAction *cellSettingsAction = menu.addAction(QStringLiteral("Cell Display Settings..."));
		connect(cellSettingsAction, &QAction::triggered, this, [this, cellIndex]() {
			MultiviewInstance *inst = config_->find_instance(uuid_);
			if (!inst)
				return;

			/* Find cell (row, col) from engine */
			LayoutEngine tmpEngine;
			tmpEngine.set_layout(inst->layout);
			tmpEngine.set_viewport(100, 100);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			if (cellIndex >= (int)cells.size())
				return;
			int row = cells[cellIndex].gridRow;
			int col = cells[cellIndex].gridCol;

			/* Find or create CellVisualSettings for this cell */
			CellVisualSettings *cvs = nullptr;
			for (auto &c : inst->cellVisualSettings) {
				if (c.row == row && c.col == col) {
					cvs = &c;
					break;
				}
			}
			CellVisualSettings temp;
			if (!cvs) {
				temp.row = row;
				temp.col = col;
				cvs = &temp;
			}

			CellDisplaySettingsDialog dlg(CellDisplaySettingsDialog::Mode::Cell, this);
			dlg.set_cell_position(row, col);
			dlg.set_cell_settings(*cvs);
			if (dlg.exec() == QDialog::Accepted) {
				CellVisualSettings result = dlg.get_cell_settings();
				result.row = row;
				result.col = col;

				/* Update or insert */
				bool found = false;
				for (auto &c : inst->cellVisualSettings) {
					if (c.row == row && c.col == col) {
						c = result;
						found = true;
						break;
					}
				}
				if (!found)
					inst->cellVisualSettings.push_back(result);

				config_->save();
				refresh_visual_settings();
			}
		});

		/* Phase 3 / M5.2: Cell-scoped Signal Lost Settings entry. Kept as a
		 * separate dialog from Cell Display Settings to honor the design
		 * doc §9 split between visual config and runtime/strategy config. */
		QAction *signalLostAction = menu.addAction(QStringLiteral("Signal Lost Settings..."));
		connect(signalLostAction, &QAction::triggered, this, [this, cellIndex]() {
			MultiviewInstance *inst = config_->find_instance(uuid_);
			if (!inst)
				return;

			LayoutEngine tmpEngine;
			tmpEngine.set_layout(inst->layout);
			tmpEngine.set_viewport(100, 100);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			if (cellIndex >= (int)cells.size())
				return;
			int row = cells[cellIndex].gridRow;
			int col = cells[cellIndex].gridCol;

			CellLostSignalSettings working;
			if (const CellLostSignalSettings *existing = inst->find_cell_lost_signal(row, col)) {
				working = *existing;
			} else {
				working.row = row;
				working.col = col;
				working.mode = InheritanceMode::Inherit;
				working.settings = config_->global_settings().lostSignal;
			}

			SignalLostSettingsDialog dlg(SignalLostSettingsDialog::Mode::Cell, this);
			dlg.set_cell_position(row, col);
			dlg.set_cell_settings(working);
			if (dlg.exec() != QDialog::Accepted)
				return;

			CellLostSignalSettings result = dlg.get_cell_settings();
			result.row = row;
			result.col = col;

			/* Insert or update in the instance vector. We keep entries even
			 * when mode == Inherit so the user retains the previously
			 * configured payload across toggles — the to_obs_data() side
			 * already filters Inherit-only entries when persisting. */
			bool found = false;
			for (auto &c : inst->cellLostSignalSettings) {
				if (c.row == row && c.col == col) {
					c = result;
					found = true;
					break;
				}
			}
			if (!found)
				inst->cellLostSignalSettings.push_back(result);

			config_->save();
			notify_multiview_signal_settings_changed(uuid_);
		});
	}

	menu.addSeparator();

	if (cellIndex >= 0) {
		/* Check if cell has a source assigned */
		bool hasSource = false;
		bool isExternal = false;
		{
			std::lock_guard<std::recursive_mutex> lock(source_mutex_);
			if (cellIndex < (int)cell_sources_.size()) {
				const auto &cs = cell_sources_[cellIndex];
				/* Phase 3 / M6.1: external-provider cells leave
				 * cs.type empty (type/name are the legacy internal
				 * binding fields). Treat the cell as occupied if
				 * either an internal type or an external provider
				 * is set so the menu shows Change/Clear/Reconnect
				 * instead of Add Source. */
				if (!cs.type.empty() || cs.provider_type != SignalProviderType::Unknown)
					hasSource = true;
				if (cs.provider_type != SignalProviderType::Unknown &&
				    !signal_provider_is_internal(cs.provider_type))
					isExternal = true;
			}
		}

		if (hasSource) {
			QAction *changeAction = menu.addAction(QStringLiteral("Change Source..."));
			connect(changeAction, &QAction::triggered, this,
				[this, cellIndex]() { on_change_source(cellIndex); });

			/* Phase 3 / M6.1+ task 9.1.C: Edit Source... shown only
			 * for external-provider cells. Internal cells already
			 * have Change Source... covering the same surface; a
			 * second entry would be confusing. */
			if (isExternal) {
				QAction *editAction = menu.addAction(QStringLiteral("Edit Source..."));
				connect(editAction, &QAction::triggered, this,
					[this, cellIndex]() { on_edit_source(cellIndex); });
			}

			QAction *clearAction = menu.addAction(QStringLiteral("Clear Cell"));
			connect(clearAction, &QAction::triggered, this,
				[this, cellIndex]() { on_clear_cell(cellIndex); });

			/* Phase 3 / M5.3: Reconnect Now is enabled only when the cell is
			 * not already happily Active. Keeps the menu clean for steady
			 * cells and matches the "manual reconnect cooldown" semantics
			 * defined in [docs/phase-3-signal-lost-and-external-sources-design.md] §7.4.
			 *
			 * Phase 3 / M6.1+ polish: external local-file cells use
			 * "Replay Now" instead of "Reconnect Now" \u2014 there is no
			 * connection to re-establish, the action just restarts
			 * media playback (obs_source_media_restart).
			 *
			 * For external cells, expose the action even when state is
			 * Active so the user can manually replay a finished file
			 * or kick a stuck stream without waiting for the runtime
			 * to flip to Lost. The cooldown still throttles abuse. */
			bool canReconnect = false;
			bool isExternalLocalFile = false;
			bool isVlcCell = false;
			bool isFfmpegLocalFileCell = false;
			OBSSource vlcSourceSnapshot;
			OBSSource ffmpegLocalSourceSnapshot;
			{
				std::lock_guard<std::recursive_mutex> lock(source_mutex_);
				if (cellIndex < (int)cell_sources_.size()) {
					const auto &cs = cell_sources_[cellIndex];
					const bool isExternal = cs.provider_type != SignalProviderType::Unknown &&
								!signal_provider_is_internal(cs.provider_type);
					canReconnect = cs.state == SignalRuntimeState::MissingInternal ||
						       cs.state == SignalRuntimeState::Lost ||
						       cs.state == SignalRuntimeState::Connecting ||
						       cs.state == SignalRuntimeState::RetryScheduled ||
						       cs.state == SignalRuntimeState::FallbackActive ||
						       cs.state == SignalRuntimeState::Error ||
						       (isExternal && cs.state == SignalRuntimeState::Active);

					if (isExternal && cs.private_source) {
						obs_data_t *cur = obs_source_get_settings(cs.private_source);
						if (cur) {
							isExternalLocalFile = obs_data_get_bool(cur, "is_local_file");
							obs_data_release(cur);
						}
					}

					if (cs.provider_type == SignalProviderType::Vlc) {
						isVlcCell = true;
						/* Snapshot the source under the lock so the
						 * media_* calls below can run without holding
						 * source_mutex_ (those calls go through libVLC
						 * and we don't want lock inversion). */
						vlcSourceSnapshot = cs.private_source;
					}

					/* Phase 3 / M6.4: FFmpeg local-file cells get a
					 * Play/Pause action too. Network streams (RTMP/HLS/
					 * SRT/...) deliberately don't \u2014 ffmpeg_source's
					 * pause behavior on a live network stream is
					 * effectively "stall the decoder, then race a
					 * reconnect when unpaused", which produces a
					 * confusing UX (cell freezes for a while, then
					 * jumps to whatever the stream is doing now).
					 * Replay Now still covers the use case of kicking
					 * a stuck network stream. */
					if (cs.provider_type == SignalProviderType::Ffmpeg && isExternalLocalFile) {
						isFfmpegLocalFileCell = true;
						ffmpegLocalSourceSnapshot = cs.private_source;
					}
				}
			}
			/* VLC has no "connection" to re-establish either \u2014 the
			 * action restarts the current playlist entry via
			 * obs_source_media_restart, semantically identical to a
			 * local-file FFmpeg replay. */
			const bool useReplayLabel = isExternalLocalFile || isVlcCell;
			QAction *reconnectAction = menu.addAction(useReplayLabel ? QStringLiteral("Replay Now")
										 : QStringLiteral("Reconnect Now"));
			reconnectAction->setEnabled(canReconnect);
			connect(reconnectAction, &QAction::triggered, this,
				[this, cellIndex]() { (void)force_reconnect_cell(cellIndex); });

			/* Phase 3 / M6.4 playlist navigation: VLC is the only
			 * provider that exposes a real playlist (FFmpeg is one
			 * file/URL; NDI/Spout have no playlist concept). Wire
			 * Previous / Play-Pause / Next directly to
			 * obs_source_media_* which vlc_source registers via
			 * media_previous / media_play_pause / media_next. */
			if (isVlcCell && vlcSourceSnapshot) {
				QAction *prevAction = menu.addAction(QStringLiteral("Previous"));
				QAction *playPauseAction = menu.addAction(QStringLiteral("Play / Pause"));
				QAction *nextAction = menu.addAction(QStringLiteral("Next"));
				const obs_media_state st = obs_source_media_get_state(vlcSourceSnapshot);
				const bool currentlyPlaying = (st == OBS_MEDIA_STATE_PLAYING);
				connect(prevAction, &QAction::triggered, this, [src = vlcSourceSnapshot]() {
					if (src)
						obs_source_media_previous(src);
				});
				connect(playPauseAction, &QAction::triggered, this,
					[src = vlcSourceSnapshot, wasPlaying = currentlyPlaying]() {
						if (src)
							obs_source_media_play_pause(src, wasPlaying);
					});
				connect(nextAction, &QAction::triggered, this, [src = vlcSourceSnapshot]() {
					if (src)
						obs_source_media_next(src);
				});
			} else if (isFfmpegLocalFileCell && ffmpegLocalSourceSnapshot) {
				/* FFmpeg has no playlist, so only Play/Pause is
				 * meaningful (and only on local files \u2014 see the
				 * snapshot site for why network streams skip this). */
				QAction *playPauseAction = menu.addAction(QStringLiteral("Play / Pause"));
				const obs_media_state st = obs_source_media_get_state(ffmpegLocalSourceSnapshot);
				const bool currentlyPlaying = (st == OBS_MEDIA_STATE_PLAYING);
				connect(playPauseAction, &QAction::triggered, this,
					[src = ffmpegLocalSourceSnapshot, wasPlaying = currentlyPlaying]() {
						if (src)
							obs_source_media_play_pause(src, wasPlaying);
					});
			}
		} else {
			QAction *addAction = menu.addAction(QStringLiteral("Add Source..."));
			connect(addAction, &QAction::triggered, this,
				[this, cellIndex]() { on_add_source(cellIndex); });
		}

		menu.addSeparator();
	}

	QAction *editGridAction = menu.addAction(QStringLiteral("Edit Grid..."));
	connect(editGridAction, &QAction::triggered, this, &MultiviewWindow::on_edit_grid);

	menu.addSeparator();

	QAction *settingsAction = menu.addAction(QStringLiteral("Global Settings"));
	connect(settingsAction, &QAction::triggered, this, &MultiviewWindow::on_global_settings);

	QAction *closeAction = menu.addAction(QStringLiteral("Close"));
	connect(closeAction, &QAction::triggered, this, &QWidget::close);

	menu.exec(pos);
}

void MultiviewWindow::on_add_source(int cellIndex)
{
	SourcePicker picker(this);
	if (picker.exec() != QDialog::Accepted)
		return;

	CellAssignment ca = picker.result_assignment();

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Determine the (row, col) of the clicked cell from the engine */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	ca.row = r;
	ca.col = c;

	/* Replace existing assignment at (r,c) or add new */
	bool found = false;
	for (auto &a : inst->cellAssignments) {
		if (a.row == r && a.col == c) {
			a = ca;
			found = true;
			break;
		}
	}
	if (!found)
		inst->cellAssignments.push_back(ca);

	inst->signalDirty = true;
	config_->save();

	/* Phase 3 / M6.1+ task 9.1.A: try the single-cell incremental path
	 * first so other cells in the same window keep their external
	 * private sources alive. Falls back to refresh_sources() if cell
	 * count changed (shouldn't here — add/change does not affect cell
	 * count) or any other invariant slips. */
	if (!refresh_cell(r, c))
		refresh_sources();
}

void MultiviewWindow::on_change_source(int cellIndex)
{
	on_add_source(cellIndex); /* Same flow */
}

void MultiviewWindow::on_edit_source(int cellIndex)
{
	/* Phase 3 / M6.1+ task 9.1.C: Edit Source for external-provider
	 * cells. Opens the provider-specific form populated from the cell's
	 * current SignalConfig; on Save writes the new config back into
	 * the assignment and runs refresh_cell so other cells stay live.
	 *
	 * Internal cells (pgm/prvw/scene/source) never reach this entry
	 * because the menu hides Edit Source for them, but defend in depth
	 * against an unexpected dispatch path. */
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Resolve (row, col) from cellIndex via the layout engine. */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	/* Find the matching CellAssignment and confirm it's external. */
	CellAssignment *target = nullptr;
	for (auto &a : inst->cellAssignments) {
		if (a.row == r && a.col == c) {
			target = &a;
			break;
		}
	}
	if (!target || !target->signalConfig.is_external())
		return;

	/* Snapshot the current signalConfig (deep copy via SignalConfig copy
	 * semantics so the dialog can edit a private copy without touching
	 * the live assignment). */
	SignalConfig snapshot = target->signalConfig;

	EditSourceDialog dlg(snapshot, this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	SignalConfig new_cfg = dlg.signal_config();
	if (new_cfg.empty()) {
		/* Provider-specific form rejected validation post-Accept (rare;
		 * the dialog already shows a popup on its own). Bail without
		 * touching persisted state. */
		return;
	}

	/* Mutate, persist, then run the single-cell incremental refresh
	 * so neighboring external cells keep playing without interruption. */
	target->signalConfig = std::move(new_cfg);
	inst->signalDirty = true;
	config_->save();

	if (!refresh_cell(r, c))
		refresh_sources();
}

void MultiviewWindow::on_clear_cell(int cellIndex)
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Determine (row, col) of the cell */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	/* Remove assignment at (r,c) */
	auto &assignments = inst->cellAssignments;
	assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
					 [r, c](const CellAssignment &a) { return a.row == r && a.col == c; }),
			  assignments.end());

	inst->signalDirty = true;
	config_->save();

	/* Phase 3 / M6.1+ task 9.1.A: incremental path — the cleared cell's
	 * private source is released, the rest of the window untouched. */
	if (!refresh_cell(r, c))
		refresh_sources();
}

void MultiviewWindow::apply_clear_cell_for_rowcols(const std::vector<std::pair<int, int>> &rowCols)
{
	if (rowCols.empty())
		return;

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	auto &assignments = inst->cellAssignments;
	bool any_removed = false;
	for (const auto &rc : rowCols) {
		const int r = rc.first;
		const int c = rc.second;
		auto before = assignments.size();
		assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
						 [r, c](const CellAssignment &a) { return a.row == r && a.col == c; }),
				  assignments.end());
		if (assignments.size() != before) {
			any_removed = true;
			obs_log(LOG_INFO, "ClearCell: dropped assignment at (row=%d, col=%d) for instance '%s'", r, c,
				inst->name.c_str());
		}
	}

	if (!any_removed)
		return;

	inst->signalDirty = true;
	config_->save();

	/* Phase 3 / M6.1+ task 9.1.A: incremental path per cleared cell.
	 * If the count somehow changed (shouldn't — ClearCell does not edit
	 * the layout grid), fall back to the heavy refresh once and break
	 * out so we don't run it for every entry in rowCols. */
	for (const auto &rc : rowCols) {
		if (!refresh_cell(rc.first, rc.second)) {
			refresh_sources();
			break;
		}
	}
}

void MultiviewWindow::on_save_assignments()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	inst->signalDirty = false;
	config_->save();
	obs_log(LOG_INFO, "cell assignments saved for '%s'", inst->name.c_str());
}

void MultiviewWindow::on_edit_grid()
{
	open_manager_dialog();
}

void MultiviewWindow::on_global_settings()
{
	open_manager_dialog();
}

void MultiviewWindow::on_toggle_fullscreen()
{
	if (isFullScreen()) {
		showNormal();
	} else {
		showFullScreen();
	}
}

void MultiviewWindow::on_toggle_always_on_top()
{
	is_always_on_top_ = !is_always_on_top_;

#ifdef _WIN32
	/* Use SetWindowPos directly to avoid HWND recreation and flicker */
	HWND hwnd = (HWND)winId();
	HWND insertAfter = is_always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST;
	SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
	Qt::WindowFlags flags = windowFlags();
	if (is_always_on_top_)
		flags |= Qt::WindowStaysOnTopHint;
	else
		flags &= ~Qt::WindowStaysOnTopHint;

	destroy_display();
	setWindowFlags(flags);
	show();
	create_display();
#endif
}