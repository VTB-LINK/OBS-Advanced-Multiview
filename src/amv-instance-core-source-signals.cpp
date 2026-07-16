/*
OBS Advanced Multiview - per-instance core: OBS source-list signal bridge +
fallback-showing reconcile.

Split verbatim out of amv-instance-core-sources.cpp to keep that TU focused on
full refresh / private-source materialization / teardown. This TU holds the
light, signal-driven paths that plugin-main debounces from OBS's
source_create / source_remove / source_destroy / source_rename signals, plus
the deferred main-thread fallback-showing reconcile:

  - refresh_sources_lazy / update_source_refs_lazy (in-place re-bind)
  - on_source_being_removed / on_source_just_created
  - reconcile_fallback_showing (Signal-Lost v2)

Methods are unchanged; they remain members of AmvInstanceCore (declared in
amv-instance-core.hpp) and share its source_mutex_ / lock-order discipline.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-frontend-cache.hpp"
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

/* ---- file-local helpers (own copy; mirrors amv-instance-core-sources.cpp) ---- */

static bool source_is_audio_only(obs_source_t *src)
{
	if (!src)
		return false;
	uint32_t flags = obs_source_get_output_flags(src);
	return (flags & OBS_SOURCE_AUDIO) && !(flags & OBS_SOURCE_VIDEO);
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

		/* Issue #5 stage B: the removed source may also be some cell's
		 * tracked scene/source fallback target. This pass is intentionally
		 * NOT gated by the primary loop's type filter above — a fallback
		 * can hang off a cell whose primary is pgm/prvw/external too. Clear
		 * the fallback slot with the same asymmetric discipline as the
		 * primary weak_ref: drop the refs + flag so the next frame's draw
		 * stops resolving through this binding, but do NOT dec_showing
		 * (we are inside the source's remove flow; its show_refs are freed
		 * when it is destroyed). Leave cs.state alone — the draw waterfall
		 * reclassifies the cell from FallbackActive down to MissingInternal
		 * once fallback_weak_ref resolves to null on the next frame. */
		for (auto &cs : cell_sources_) {
			bool fb_hit = false;
			if (cs.fallback_weak_ref) {
				OBSSourceAutoRelease bound = OBSGetStrongRef(cs.fallback_weak_ref);
				if (bound == source)
					fb_hit = true;
			}
			if (!fb_hit && cs.fallback_shown_ref) {
				OBSSourceAutoRelease shown = OBSGetStrongRef(cs.fallback_shown_ref);
				if (shown == source)
					fb_hit = true;
			}
			if (fb_hit) {
				cs.fallback_weak_ref = nullptr;
				cs.fallback_shown_ref = nullptr;
				cs.fallback_active = false;
				cs.resolved_fallback_name.clear();
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

void AmvInstanceCore::reconcile_fallback_showing()
{
	/* Issue #5 stage B: reconcile the scene/source fallback inc/dec_showing
	 * pairing on the core's UI thread (posted, coalesced, from draw_cells).
	 *
	 * Done here — off the render thread and OUTSIDE source_mutex_ — because
	 * obs_source_inc_showing / dec_showing call obs_source_activate /
	 * _deactivate, which walk the source's active tree (taking libobs scene
	 * locks). Running that under our recursive mutex + the graphics lock
	 * would invert the (graphics -> source_mutex_) lock order. Same
	 * collect-under-lock / act-outside-lock discipline release_source_refs
	 * and refresh_cell use for the primary slot.
	 *
	 * Reconciliation is keyed on source identity (weak refs): a cell's
	 * desired-shown source is fallback_weak_ref while fallback_active, else
	 * none. When that differs from the source we currently hold
	 * (fallback_shown_ref) we dec the old and inc the new — so a
	 * fallbackName change (old -> new) and a primary recovery (target ->
	 * none) both fall out of the same compare. We commit fallback_shown_ref
	 * to the desired value under the lock and change the show counts after
	 * releasing it; if the render thread flips fallback_active again in the
	 * gap it simply posts another reconcile. */
	struct ShowOp {
		OBSWeakSource dec; /* source to dec_showing (may be empty) */
		OBSWeakSource inc; /* source to inc_showing (may be empty) */
	};
	std::vector<ShowOp> ops;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);
		for (auto &cs : cell_sources_) {
			OBSWeakSource desired = cs.fallback_active ? cs.fallback_weak_ref : OBSWeakSource();
			if (cs.fallback_shown_ref.Get() == desired.Get())
				continue;
			ShowOp op;
			op.dec = cs.fallback_shown_ref;
			op.inc = desired;
			ops.push_back(std::move(op));
			cs.fallback_shown_ref = desired;
		}
	}

	for (auto &op : ops) {
		/* Skip a source that is mid-removal: its show_refs are freed when
		 * it is destroyed, so a missed dec leaks nothing — same rationale
		 * as on_source_being_removed deliberately not dec'ing. */
		if (op.dec.Get()) {
			OBSSourceAutoRelease s = OBSGetStrongRef(op.dec);
			if (s && !obs_source_removed(s))
				obs_source_dec_showing(s);
		}
		if (op.inc.Get()) {
			OBSSourceAutoRelease s = OBSGetStrongRef(op.inc);
			if (s && !obs_source_removed(s))
				obs_source_inc_showing(s);
		}
	}
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
