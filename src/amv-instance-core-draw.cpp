/*
OBS Advanced Multiview - per-instance core: grid composition (draw_cells)
(issue #10).

The per-cell composition loop moved out of AmvInstanceCore::draw_grid. The
VIEW computes its own layout (LayoutEngine at its window size) and passes the
resulting cells here; this method paints them into the current render target's
viewport using the shared cell state. So N views of different sizes share one
set of sources but each renders natively at its own resolution.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-logging.hpp"
#include "amv-i18n.hpp"

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

void AmvInstanceCore::draw_cells(const std::vector<CellRect> &cells, int vpX, int vpY, int vpW, int vpH)
{
	/* Acquires source_mutex_: called on the graphics thread by each VIEW's
	 * display render() (with the cells that view computed for its own size) and
	 * by the output pass. source_mutex_ is recursive, so a nested lock is
	 * cheap. The caller owns layout computation; we only paint `cells`. */
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

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
				if (!srcHolder)
					cell_sources_[i].audio_only = false;
				if (!srcHolder && !cs.name.empty() && re_resolve_counter_ == 0) {
					/* Source may have been re-added (undo) - throttled */
					obs_source_t *resolved = obs_get_source_by_name(cs.name.c_str());
					if (resolved) {
						cell_sources_[i].weak_ref = OBSGetWeakRef(resolved);
						cell_sources_[i].audio_only = cs.type == "source" &&
									      source_is_audio_only(resolved);
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
				const bool image_configured = (beh == ExternalLostBehavior::RetryWithFallback &&
							       eff.fallbackType == std::string("image") &&
							       !eff.fallbackName.empty()) ||
							      (beh == ExternalLostBehavior::SignalLostImage &&
							       !eff.signalLostImagePath.empty());
				if (isFallback) {
					display_state = SignalRuntimeState::FallbackActive;
				} else if (supervisor_state == SignalRuntimeState::RetryScheduled) {
					/* Phase 3 hardening tail: an active recovery
					 * attempt is in flight this tick (either a
					 * media_restart or a queued full recreate).
					 * Surface CONNECTING (blue band) regardless
					 * of latch + behavior, so the user sees the
					 * same "we are trying" feedback for both
					 * the cheap restart-then-recreate paths.
					 * Without this branch, SignalLostOverlay /
					 * SignalLostImage users would only ever
					 * see CONNECTING on the recreate path
					 * (which clears the latch via install),
					 * never on the lighter restart path. */
					display_state = SignalRuntimeState::Connecting;
				} else if (cs.fallback_latched && supervisor_state != SignalRuntimeState::Active) {
					switch (beh) {
					case ExternalLostBehavior::RetryWithFallback:
						display_state = image_configured ? SignalRuntimeState::FallbackActive
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
				if (cs.type == "source")
					cell_sources_[i].audio_only = false;
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
					/* Phase 3 / M6.6 hardening: coalesce posts.
					 * If an earlier transition already queued a
					 * rebuild that hasn't run yet, skip this one;
					 * a single rebuild covers all transitions
					 * observed up to the moment the lambda runs. */
					if (!lost_images_rebuild_pending_.exchange(true, std::memory_order_acq_rel)) {
						QTimer::singleShot(0, this, [this]() {
							lost_images_rebuild_pending_.store(false,
											   std::memory_order_release);
							rebuild_lost_signal_images();
						});
					}
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
				 *   - external cells where the provider cleared content
				 *     on disconnect or timeout;
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
					amv_log_detailed(
						LOG_INFO,
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

		/* Render safe area guides after video, before overlay. Anchor is
		 * configurable per resolved SafeAreaSettings (Cell or Signal). */
		render_safe_area(i, cellX, cellY, cell.w, cell.h, hasSignalRect ? vrX : 0, hasSignalRect ? vrY : 0,
				 hasSignalRect ? vrW : 0, hasSignalRect ? vrH : 0);

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
		    effective_visuals_[i].label.labelRegionFill) {
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

/* ---- Output (Spout / NDI) ---- */
