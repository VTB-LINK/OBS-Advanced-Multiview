/*
OBS Advanced Multiview - per-instance core: source resolution, refresh,
per-frame tick, and external-output dispatch (issue #10).

Moved verbatim out of multiview-window.cpp during the core/view split; the
methods are unchanged except for living on AmvInstanceCore instead of
MultiviewWindow (and reading the OBS canvas base resolution where the old code
read the per-window viewport cache).

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-logging.hpp"
#include "amv-i18n.hpp"
#include "signal-provider.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QTimer>

#include <algorithm>
#include <cmath>

/* ---- file-local helpers (mirrors multiview-window.cpp) ---- */

static bool source_is_audio_only(obs_source_t *src)
{
	if (!src)
		return false;
	uint32_t flags = obs_source_get_output_flags(src);
	return (flags & OBS_SOURCE_AUDIO) && !(flags & OBS_SOURCE_VIDEO);
}

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

/* Reference viewport for sizing shared text/image resources: the OBS canvas
 * base resolution (falls back to 1920x1080). Replaces the per-window viewport
 * cache the single-window code used. */
int AmvInstanceCore::ref_vp_width() const
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.base_width > 0)
		return (int)ovi.base_width;
	return 1920;
}

int AmvInstanceCore::ref_vp_height() const
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.base_height > 0)
		return (int)ovi.base_height;
	return 1080;
}

void AmvInstanceCore::refresh_layout()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	layout_ = inst->layout;
	gutter_px_ = inst->effective_gutter(config_->global_settings().defaultGutterPx);

	/* Update layout with effective gutter */
	layout_.gutterPx = gutter_px_;

	/* Output engine layout cache is invalidated here; each VIEW invalidates
	 * its own viewport cache via on_core_state_changed() after this returns. */
	output_cached_vpW_ = 0;
	output_cached_vpH_ = 0;

	/* Re-resolve source refs: grid shape may have changed, affecting
	 * which cells exist and their indices */
	refresh_sources();

	/* Recompute effective visual settings for the new cell layout */
	refresh_visual_settings();
}

void AmvInstanceCore::refresh_sources()
{
	release_source_refs();
	update_source_refs();
	rebuild_label_sources();
	rebuild_volmeters();
	rebuild_lost_signal_images();
}

bool AmvInstanceCore::refresh_cell(int row, int col)
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
		cs.audio_only = false;
		cs.state = SignalRuntimeState::Empty;
		cs.last_active_ns = 0;
		cs.last_reconnect_ns = 0;
		cs.retry_attempt = 0;
		cs.provider_type = SignalProviderType::Unknown;
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
					cs.audio_only = ca->type == "source" && source_is_audio_only(src);
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
					/* Phase 3 / M6.6 H.5 hardening: clear sticky
					 * fallback latch on every fresh cell binding so a
					 * previously-failed cell that the user just re-edited
					 * to a working URL doesn't keep painting the fallback
					 * until the supervisor's first probe lands. The latch
					 * exists to mask retry flicker, not to outlive the
					 * source it was tracking. */
					cs.fallback_latched = false;
					cs.last_health_state = SignalRuntimeState::Empty;
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
void AmvInstanceCore::refresh_sources_lazy()
{
	update_source_refs_lazy();
	/* Phase 3 / M5.4: a lazy refresh can change cs.state (e.g. a source
	 * the user just undid is now Active again). Picking up the matching
	 * placeholder / fallback image needs to happen on the UI thread, so
	 * piggyback on this entry point. The rebuild itself is path-keyed —
	 * if nothing changed, it's a no-op. */
	rebuild_lost_signal_images();
}

void AmvInstanceCore::on_source_being_removed(obs_source_t *source)
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
			cs.audio_only = false;
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

void AmvInstanceCore::on_source_just_created(obs_source_t *source)
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
			cs.audio_only = cs.type == "source" && source_is_audio_only(source);
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

void AmvInstanceCore::update_source_refs_lazy()
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
						cs.audio_only = newType == "source" && source_is_audio_only(src);
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
		cs.audio_only = false;
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
			cs.audio_only = newType == "source" && source_is_audio_only(src);
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
void AmvInstanceCore::refresh_visual_settings()
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
		tmpEngine.set_viewport(ref_vp_width(), ref_vp_height());
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
		release_status_text_sources();
	}

	/* Image rebuild involves obs_enter_graphics() which must be called
	 * without holding source_mutex_ to prevent ABBA deadlock with the
	 * render thread (render thread: graphics lock -> source_mutex_). */
	rebuild_bg_images();
	rebuild_overlay_images();
	rebuild_lost_signal_images();
	rebuild_scale_label_sources();
}

void AmvInstanceCore::refresh_signal_settings()
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

bool AmvInstanceCore::force_reconnect_cell(int cellIndex)
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
	 * instance / global config disappears mid-call.
	 *
	 * Also capture the cell's grid row/col here so the recreate
	 * escalation below can queue refresh_cell(row, col) without a
	 * second LayoutEngine pass. -1 means "unknown" (instance vanished). */
	int cooldownMs = 1000;
	int cellRow = -1;
	int cellCol = -1;
	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			LayoutEngine tmpEngine;
			tmpEngine.set_layout(inst->layout);
			tmpEngine.set_viewport(100, 100);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			if (cellIndex < (int)cells.size()) {
				cellRow = cells[cellIndex].gridRow;
				cellCol = cells[cellIndex].gridCol;
				const CellLostSignalSettings *cls = inst->find_cell_lost_signal(cellRow, cellCol);
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
			amv_log_detailed(LOG_DEBUG, "reconnect cooldown active for cell %d (%llu/%d ms)", cellIndex,
					 (unsigned long long)elapsedMs, cooldownMs);
			return false;
		}
	}
	cs.last_reconnect_ns = now;
	cs.retry_attempt++;

	/* Phase 3 hardening tail: manual Reconnect Now mirrors the supervisor
	 * Lost-branch logic (capability-driven, no per-provider branches).
	 *
	 * The user is explicitly asking for a recovery attempt, so each click
	 * must do *something* visible. The escalation ladder is the same as
	 * the supervisor:
	 *
	 *   supports_media_restart  &&  attempts < kMaxLostRestartAttempts
	 *     -> media_restart, bump lost_restart_attempts
	 *   supports_media_restart  &&  !benefits_from_recreate
	 *     -> media_restart on every click (no recreate is meaningful);
	 *        reset counter so the click ladder doesn't lock out
	 *   benefits_from_recreate
	 *     -> queue refresh_cell on the Qt main thread, reset counter
	 *   neither (NDI / Spout)
	 *     -> ack the click for cooldown bookkeeping; host plugin owns
	 *        actual recovery
	 *
	 * Held strong ref via OBSSource (private_source) survives the call;
	 * obs_source_media_restart is safe to call on any source type that
	 * registered the media callbacks (ffmpeg_source / vlc_source). */
	if (cs.provider_type != SignalProviderType::Unknown && !signal_provider_is_internal(cs.provider_type)) {
		const auto *provider = SignalProviderRegistry::instance().find(cs.provider_type);
		const bool restart_eligible = provider && provider->supports_media_restart();
		const bool recreate_eligible = provider && provider->benefits_from_recreate();

		if (!cs.private_source) {
			/* No live source to act on: if the provider supports
			 * recreate and we have row/col, queue refresh_cell;
			 * otherwise just mark Error so the overlay surfaces. */
			cs.state = SignalRuntimeState::Error;
			amv_log_detailed(LOG_INFO, "%sreconnect cell %d: external provider has no private source",
					 log_prefix().c_str(), cellIndex);
			if (recreate_eligible && cellRow >= 0 && cellCol >= 0) {
				cs.lost_restart_attempts = 0;
				QTimer::singleShot(0, this, [this, cellRow, cellCol]() {
					if (!refresh_cell(cellRow, cellCol))
						refresh_sources();
				});
			}
			return true;
		}

		if (restart_eligible && cs.lost_restart_attempts < kMaxLostRestartAttempts) {
			obs_source_media_restart(cs.private_source);
			cs.lost_restart_attempts++;
			cs.state = SignalRuntimeState::Connecting;
			amv_log_detailed(LOG_INFO,
					 "%sreconnect cell %d: media_restart #%d/%d on external provider '%s'",
					 log_prefix().c_str(), cellIndex, cs.lost_restart_attempts,
					 kMaxLostRestartAttempts, signal_provider_to_string(cs.provider_type));
			return true;
		}

		if (recreate_eligible && cellRow >= 0 && cellCol >= 0) {
			cs.lost_restart_attempts = 0;
			cs.state = SignalRuntimeState::Connecting;
			amv_log_detailed(LOG_INFO,
					 "%sreconnect cell %d: scheduling full recreate of external provider '%s'",
					 log_prefix().c_str(), cellIndex, signal_provider_to_string(cs.provider_type));
			QTimer::singleShot(0, this, [this, cellRow, cellCol]() {
				if (!refresh_cell(cellRow, cellCol))
					refresh_sources();
			});
			return true;
		}

		if (restart_eligible) {
			/* restart-only or recreate unavailable due to missing
			 * row/col: fall back to repeating the cheap restart and
			 * reset the counter so the user can keep clicking. */
			obs_source_media_restart(cs.private_source);
			cs.lost_restart_attempts = 0;
			cs.state = SignalRuntimeState::Connecting;
			amv_log_detailed(LOG_INFO, "%sreconnect cell %d: media_restart on external provider '%s'",
					 log_prefix().c_str(), cellIndex, signal_provider_to_string(cs.provider_type));
			return true;
		}

		/* NDI / Spout: no useful action; host plugin owns reconnect.
		 * Touch last_reconnect_ns above so the cooldown still ticks. */
		amv_log_detailed(
			LOG_INFO,
			"%sreconnect cell %d: provider '%s' has no manual recovery action (host plugin auto-reconnects)",
			log_prefix().c_str(), cellIndex, signal_provider_to_string(cs.provider_type));
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
			cs.audio_only = cs.type == "source" && source_is_audio_only(resolved);
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

void AmvInstanceCore::update_source_refs()
{
	/* Phase 3 / M6 step 9: per-cell external provider intents collected
	 * inside the locked section, materialized OUTSIDE the lock so
	 * obs_source_create_private never runs while we hold source_mutex_
	 * (see docs/ROADMAP.md §6 lock order). Each intent owns a deep copy of the
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
			cell_sources_[i].audio_only = false;
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
				cell_sources_[i].audio_only = ca->type == "source" && source_is_audio_only(src);
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
				cs.lost_restart_attempts = 0;
				/* Phase 3 / M6.6 H.5 hardening: clear sticky fallback
				 * latch on full refresh too. Same rationale as the
				 * single-cell refresh_cell path above. */
				cs.fallback_latched = false;
				cs.last_health_state = SignalRuntimeState::Empty;
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

void AmvInstanceCore::recompute_effective_lost_locked(const std::vector<CellRect> &cells)
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

void AmvInstanceCore::release_source_refs()
{
	/* Collect textures to destroy outside the mutex to avoid
	 * deadlock: render thread holds graphics lock then takes mutex,
	 * so we must never hold mutex while calling obs_enter_graphics(). */
	std::vector<gs_texture_t *> textures_to_destroy;

	/* Phase 3 / M6: external private sources must be released OUTSIDE
	 * source_mutex_ (see docs/ROADMAP.md §6 lock order). Move them into a local
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

void AmvInstanceCore::tick_once_per_frame()
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	/* De-dup across N views + the output driver: all of them drive this each
	 * frame on the single OBS graphics thread (serialized). Tick on the first
	 * call of a frame and skip the rest, using half the frame interval as the
	 * guard window. The tick body is idempotent, so even if the guard admits a
	 * second call it only repeats harmless re-resolve/snapshot work. */
	const uint64_t now = os_gettime_ns();
	uint64_t min_gap_ns = 8000000; /* ~half a 60fps frame */
	{
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.fps_num > 0)
			min_gap_ns = (uint64_t)(1000000000ull * ovi.fps_den / ovi.fps_num) / 2;
	}
	if (last_tick_token_ != 0 && now - last_tick_token_ < min_gap_ns)
		return;
	last_tick_token_ = now;

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
}

void AmvInstanceCore::render_output_only()
{
	/* Graphics-thread external-output pass, independent of the display.
	 * Renders the grid into the output manager's per-resolution texrender(s)
	 * and dispatches to each enabled backend. The manager self-reconciles
	 * from output_settings_ each frame. No-op when output is disabled. */
	if (!output_)
		return;
	output_->render_all(instance_display_name(), output_settings_, [this](int w, int h) {
		/* Output renders at its own (canvas/config) resolution via the core's
		 * own engine, independent of any view's viewport cache. */
		if (w != output_cached_vpW_ || h != output_cached_vpH_) {
			output_engine_.set_layout(layout_);
			output_engine_.set_viewport(w, h);
			output_engine_.compute();
			output_cached_vpW_ = w;
			output_cached_vpH_ = h;
		}
		draw_cells(output_engine_.cells(), 0, 0, w, h);
	});
}

std::string AmvInstanceCore::instance_display_name() const
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (inst && !inst->name.empty())
		return inst->name;
	return "OBS Advanced Multiview";
}

void AmvInstanceCore::apply_output_settings()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	InstanceOutputSettings next = inst ? inst->outputSettings : InstanceOutputSettings{};

	/* Serialize against the render thread: the display draw callback holds the
	 * OBS graphics context while render() reads output_ / output_settings_, so
	 * mutate them under obs_enter_graphics. The manager itself reconciles
	 * backends from the config on the graphics thread inside render_all(). */
	obs_enter_graphics();
	output_settings_ = next;
	if (next.any_enabled()) {
		if (!output_)
			output_ = std::make_unique<MultiviewOutputManager>();
	} else if (output_) {
		output_->teardown_locked();
		output_.reset();
	}
	obs_leave_graphics();
}

AmvInstanceCore::CellRuntimeSnapshot AmvInstanceCore::snapshot_cell(int cellIndex)
{
	CellRuntimeSnapshot snap;
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);
	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return snap;
	const auto &cs = cell_sources_[cellIndex];
	snap.valid = true;
	snap.type = cs.type;
	snap.provider_type = cs.provider_type;
	snap.state = cs.state;
	snap.private_source = cs.private_source;
	snap.weak_ref = cs.weak_ref;
	return snap;
}

void AmvInstanceCore::apply_clear_cell_for_rowcols(const std::vector<std::pair<int, int>> &rowCols)
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

	/* Incremental path per cleared cell; fall back to a full refresh once if
	 * the cell count somehow changed (it shouldn't — ClearCell doesn't edit
	 * the layout grid). */
	for (const auto &rc : rowCols) {
		if (!refresh_cell(rc.first, rc.second)) {
			refresh_sources();
			break;
		}
	}
}
