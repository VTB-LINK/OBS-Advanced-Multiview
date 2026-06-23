/*
OBS Advanced Multiview - External cell health supervisor (Phase 3 / M6 step 10)

Provider-agnostic state machine that drives external cells through
Connecting / Active / Lost / Error transitions plus media_restart and
full-recreate actions. Called from the render thread at ~1 Hz per cell.

The supervisor consumes two surfaces only:
  1. ISignalProvider::probe_health() / supports_media_restart() /
     benefits_from_recreate()  --  three virtual methods that every
     external provider implements.
  2. CellSource bookkeeping fields  --  source_created_ns,
     connecting_since_ns, lost_since_ns, last_active_ns, last_reconnect_ns,
     media_restart_attempts, retry_attempt.

That means NDI / Spout / VLC / future providers do NOT need their own
supervisor code; they just override the three provider virtuals and the
generic logic here handles state transitions, timing, backoff, and the
refresh_cell scheduling for full recreate.

Lock-order notes (mirrors docs/ROADMAP.md §6):
  - Caller holds source_mutex_ for the duration of the tick.
  - obs_source_media_restart() is safe to call under the mutex; it just
    signals ffmpeg_source's worker thread.
  - Full recreate (refresh_cell) is NOT safe under the mutex because
    refresh_cell itself uses the four-phase unlock-side pattern. We
    queue it onto the Qt main thread via QTimer::singleShot.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "signal-provider.hpp"
#include "amv-logging.hpp"

#include <obs.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QTimer>

namespace {

/* Tunables. All in nanoseconds. Tuned for slow network streams (HLS with
 * tens-of-seconds segments, or a congested link) where the first frame can take
 * 15-40 s to arrive: Opening grace 15 s, then a restart every 10 s, escalate to
 * Lost after 3 restarts or 60 s total without frames, then schedule a full
 * recreate (for providers that benefit) after manualReconnectCooldownMs.
 *
 * Why these are generous: obs_source_media_restart() REWINDS the source's
 * buffering to zero, so poking a stream that is still slowly buffering its first
 * frame prevents it from EVER loading (the bug this widening fixes). After any
 * restart the source re-enters Opening, so this grace also protects the
 * post-reconnect re-buffer. Harmless for local files — they reach Active in well
 * under a second, far below the grace, so the grace never fires for healthy
 * local media; local replay-after-end uses the separate Lost cooldown below. */
constexpr uint64_t NS_PER_SEC = 1'000'000'000ULL;
constexpr uint64_t kOpeningGraceNs = 15 * NS_PER_SEC;
constexpr uint64_t kConnectingTotalNs = 60 * NS_PER_SEC;
constexpr int kMaxMediaRestartAttempts = 3;
constexpr uint64_t kMediaRestartCooldownNs = 10 * NS_PER_SEC;

/* Signal-Lost v2: fixed reconnect backoff ladder for the Lost state.
 * Wait before the Nth recovery action (media_restart / recreate):
 * 5 -> 10 -> 15 -> 20 -> 30 s, then hold 30 s/attempt until Active or a
 * manual Reconnect. Deliberately NOT user-exposed (replaces the old flat
 * manualReconnectCooldownMs cadence + its >=5s clamp). The separate
 * Opening-phase protections (kOpeningGraceNs / kConnectingTotalNs /
 * kMediaRestartCooldownNs) still guard first-frame loading and are unrelated
 * to this loss-recovery ladder. */
constexpr int kBackoffLadderSec[] = {5, 10, 15, 20, 30};
constexpr int kBackoffLadderSteps = (int)(sizeof(kBackoffLadderSec) / sizeof(kBackoffLadderSec[0]));

inline uint64_t backoff_cooldown_ns(int attempt)
{
	const int idx = attempt < 0 ? 0 : (attempt >= kBackoffLadderSteps ? kBackoffLadderSteps - 1 : attempt);
	return (uint64_t)kBackoffLadderSec[idx] * NS_PER_SEC;
}

/* Clamp the ladder index for the "step N/total" log display. */
inline int backoff_ladder_step(int attempt)
{
	return attempt > kBackoffLadderSteps ? kBackoffLadderSteps : attempt;
}

} // namespace

AmvInstanceCore::SignalRuntimeState AmvInstanceCore::tick_external_cell_health(int cellIndex, int cellRow, int cellCol,
									       uint64_t now_ns)
{
	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return SignalRuntimeState::Empty;

	auto &cs = cell_sources_[cellIndex];

	/* Sanity: the supervisor is only meaningful for external cells with
	 * a registered provider. Caller filters but we re-check so this
	 * function is safe to invoke unconditionally if needed. */
	if (cs.provider_type == SignalProviderType::Unknown || signal_provider_is_internal(cs.provider_type)) {
		return cs.state;
	}

	const auto *provider = SignalProviderRegistry::instance().find(cs.provider_type);
	if (!provider) {
		cs.last_error_reason = "provider not registered";
		return SignalRuntimeState::Error;
	}

	/* If the cell has no live private source, we already failed creation
	 * earlier (see refresh_cell error branch). Stay in Error so the
	 * SIGNAL LOST overlay surfaces the failure to the user. */
	obs_source_t *raw = cs.private_source.Get();
	if (!raw) {
		return SignalRuntimeState::Error;
	}

	/* Bootstrap fresh sources: refresh_cell installs the source but does
	 * not stamp source_created_ns. First tick takes care of it; age_ns
	 * starts at 0 which is what the providers' grace window expects. */
	if (cs.source_created_ns == 0)
		cs.source_created_ns = now_ns;
	const uint64_t age_ns = now_ns - cs.source_created_ns;

	const ISignalProvider::HealthReport report = provider->probe_health(raw, age_ns);
	cs.last_dimensions_w = report.width;
	cs.last_dimensions_h = report.height;
	if (!report.reason.empty())
		cs.last_error_reason = report.reason;

	/* Per-cell diagnostic, every 5s. For timed media it reports playback-time
	 * advancement (a real stall/buffering signal — render rate can't show this
	 * because we redraw the latest frame every display frame regardless); a
	 * sub-threshold advance emits a yellow WARNING that surfaces even with
	 * detailed logs off. Live/non-timed cells just emit a health heartbeat.
	 * The healthy info lines are gated behind the global Detailed logs toggle
	 * so they don't pollute normal operation; failures also surface via state
	 * transitions (overlays + WARNING/ERROR paths). */
	constexpr uint64_t kPerfLogIntervalNs = 5ULL * NS_PER_SEC;
	/* Below this fraction of real-time advancement, a PLAYING timed media is
	 * considered stalled/buffering and gets a yellow WARNING. Healthy playback
	 * advances ~1:1 (the playback clock is wall-driven), a stuck/buffering one
	 * advances ~0, so 0.5 cleanly flags real stalls without false-firing on the
	 * small sampling jitter over a 5s window. */
	constexpr double kMediaProgressWarnRatio = 0.5;
	if (now_ns - cs.last_perf_log_ns >= kPerfLogIntervalNs) {
		const uint64_t elapsed_ns = cs.last_perf_log_ns == 0 ? kPerfLogIntervalNs
								     : (now_ns - cs.last_perf_log_ns);
		const double elapsed_ms = (double)elapsed_ns / 1e6;

		/* For timed media (local file / VLC, duration > 0) monitor playback-time
		 * advancement to catch stalls/buffering. Live & network sources (m3u8,
		 * NDI, Spout) have no meaningful duration and OBS exposes no per-source
		 * delivered-frame count, so they fall through to the plain heartbeat. */
		const int64_t duration_ms = obs_source_media_get_duration(raw);
		bool media_logged = false;
		if (duration_ms > 0) {
			const int64_t cur_ms = obs_source_media_get_time(raw);
			const obs_media_state mstate = obs_source_media_get_state(raw);
			if (mstate == OBS_MEDIA_STATE_PLAYING && cs.last_perf_log_ns != 0 &&
			    cs.last_media_time_ms >= 0) {
				int64_t advanced = cur_ms - cs.last_media_time_ms;
				if (advanced < 0)
					advanced = (int64_t)elapsed_ms; /* looped/seeked back — treat as healthy */
				const double ratio = elapsed_ms > 0.0 ? (double)advanced / elapsed_ms : 1.0;
				media_logged = true;
				if (ratio < kMediaProgressWarnRatio) {
					obs_log(LOG_WARNING,
						"%s[media] cell (%d,%d) provider=%s STALLED: playback advanced only "
						"%lldms over %.0fms wall (%.0f%%) frame=%ux%u",
						log_prefix().c_str(), cellRow, cellCol,
						signal_provider_to_string(cs.provider_type), (long long)advanced,
						elapsed_ms, ratio * 100.0, cs.last_dimensions_w, cs.last_dimensions_h);
				} else {
					amv_log_detailed(
						LOG_INFO,
						"%s[media] cell (%d,%d) provider=%s OK: playback advanced %lld/%.0fms "
						"wall (%.0f%%) frame=%ux%u",
						log_prefix().c_str(), cellRow, cellCol,
						signal_provider_to_string(cs.provider_type), (long long)advanced,
						elapsed_ms, ratio * 100.0, cs.last_dimensions_w, cs.last_dimensions_h);
				}
			}
			cs.last_media_time_ms = cur_ms;
		} else {
			cs.last_media_time_ms = -1;
		}

		/* Live / non-timed cells: lightweight health + dimensions heartbeat. */
		if (!media_logged) {
			amv_log_detailed(LOG_INFO, "%s[perf] cell (%d,%d) provider=%s frame=%ux%u health=%d state=%d",
					 log_prefix().c_str(), cellRow, cellCol,
					 signal_provider_to_string(cs.provider_type), cs.last_dimensions_w,
					 cs.last_dimensions_h, (int)report.code, (int)cs.state);
		}

		cs.render_calls = 0;
		cs.last_perf_log_ns = now_ns;
	}

	switch (report.code) {
	case ISignalProvider::HealthCode::Active:
		/* Source is producing frames. Clear all the recovery
		 * bookkeeping so a future drop starts a clean countdown. */
		cs.last_active_ns = now_ns;
		cs.connecting_since_ns = 0;
		cs.lost_since_ns = 0;
		cs.media_restart_attempts = 0;
		cs.lost_restart_attempts = 0;
		cs.retry_attempt = 0;
		cs.recovery_attempt = 0;
		return SignalRuntimeState::Active;

	case ISignalProvider::HealthCode::Paused:
		/* User pressed Play/Pause from the cell context menu. Not a
		 * failure: don't escalate, don't auto-restart, don't run the
		 * fallback substitution. Reset the same counters Active
		 * resets so resuming playback starts from a clean slate. */
		cs.last_active_ns = now_ns;
		cs.connecting_since_ns = 0;
		cs.lost_since_ns = 0;
		cs.media_restart_attempts = 0;
		cs.lost_restart_attempts = 0;
		cs.retry_attempt = 0;
		cs.recovery_attempt = 0;
		return SignalRuntimeState::Paused;

	case ISignalProvider::HealthCode::Opening: {
		/* Stamp the start of the Connecting phase on first entry. */
		if (cs.connecting_since_ns == 0)
			cs.connecting_since_ns = now_ns;
		const uint64_t opening_for = now_ns - cs.connecting_since_ns;

		/* Issue #10: an optional per-source first-frame timeout (slow network
		 * streams) overrides both the grace and the total. Setting them equal
		 * means we wait the full timeout WITHOUT firing a media_restart (which
		 * rewinds buffering and would stop a slow stream from ever loading),
		 * then escalate to Lost so the recovery path takes over. Unset (0) ->
		 * built-in defaults (grace 15 s, total 60 s, with mid-load restarts). */
		const uint64_t effGraceNs = cs.first_frame_timeout_ms > 0
						    ? (uint64_t)cs.first_frame_timeout_ms * 1'000'000ULL
						    : kOpeningGraceNs;
		const uint64_t effTotalNs = cs.first_frame_timeout_ms > 0
						    ? (uint64_t)cs.first_frame_timeout_ms * 1'000'000ULL
						    : kConnectingTotalNs;

		/* Escalation A: stuck Opening for too long total -> Lost. */
		if (opening_for >= effTotalNs || cs.media_restart_attempts >= kMaxMediaRestartAttempts) {
			cs.lost_since_ns = now_ns;
			return SignalRuntimeState::Lost;
		}

		/* Escalation B: past the opening grace and the provider says
		 * media_restart helps -> kick a restart, throttled by
		 * kMediaRestartCooldownNs. Internally this calls
		 * obs_source_media_restart, which ffmpeg_source/vlc_source
		 * handle on their own worker thread. */
		if (opening_for >= effGraceNs && provider->supports_media_restart() && now_ns >= cs.next_retry_ns) {
			obs_source_media_restart(raw);
			cs.media_restart_attempts++;
			cs.next_retry_ns = now_ns + kMediaRestartCooldownNs;
			cs.last_reconnect_ns = now_ns;
			amv_log_detailed(
				LOG_INFO,
				"%s[health][auto] cell (%d,%d) opening media_restart #%d/%d on '%s' (reason='%s'; opening_for %llus, next in %llus)",
				log_prefix().c_str(), cellRow, cellCol, cs.media_restart_attempts,
				kMaxMediaRestartAttempts, signal_provider_to_string(cs.provider_type),
				cs.last_error_reason.c_str(), (unsigned long long)(opening_for / NS_PER_SEC),
				(unsigned long long)(kMediaRestartCooldownNs / NS_PER_SEC));
			return SignalRuntimeState::Connecting;
		}

		return SignalRuntimeState::Connecting;
	}

	case ISignalProvider::HealthCode::Lost: {
		if (cs.lost_since_ns == 0)
			cs.lost_since_ns = now_ns;

		/* Phase 3 / M6.6: auto-recreate is now provider-only, not
		 * behavior-gated. Earlier the supervisor refused to recreate
		 * when the user picked SignalLostOverlay or SignalLostImage,
		 * the rationale being "user explicitly wants to stare at the
		 * overlay/image". But that left FFmpeg/VLC cells permanently
		 * stuck the moment they hit ENDED with those modes — even
		 * though NDI/Spout cells recovered automatically (their host
		 * plugins reconnect internally). User-visible inconsistency.
		 *
		 * Now: all behaviors trigger retry; the choice only controls
		 * *what overlay is shown* during the unhealthy window
		 * (handled in the render-thread state classifier, not here).
		 * Routing across providers is capability-driven via two
		 * ISignalProvider virtuals — no per-provider branches here. */
		const bool recreate_eligible = provider->benefits_from_recreate();
		const bool restart_eligible = provider->supports_media_restart();

		/* NDI / Spout: no useful recovery action on our side. The host
		 * plugin reconnects internally; we just keep painting SIGNAL
		 * LOST until the source comes back. */
		if (!recreate_eligible && !restart_eligible)
			return SignalRuntimeState::Lost;

		/* Signal-Lost v2: laddered backoff (5/10/15/20/30s, hold 30s),
		 * indexed by recovery_attempt — replaces the old flat
		 * manualReconnectCooldownMs cadence. cooldown_ns is the wait before
		 * THIS attempt; after acting we bump recovery_attempt and schedule
		 * next_retry_ns from the (grown) ladder step. */
		uint64_t cooldown_ns = backoff_cooldown_ns(cs.recovery_attempt);

		const uint64_t lost_for = now_ns - cs.lost_since_ns;
		if (lost_for < cooldown_ns)
			return SignalRuntimeState::Lost;

		/* Throttle so we don't queue retries every tick once the
		 * cooldown has elapsed. next_retry_ns is shared with the
		 * Opening media_restart throttle above; safe because Lost
		 * and Connecting don't co-occur for the same cell. */
		if (now_ns < cs.next_retry_ns)
			return SignalRuntimeState::Lost;

		/* Phase 3 hardening tail (Plan C, provider-agnostic):
		 *
		 * Within a single Lost window try the cheap path first —
		 * obs_source_media_restart — up to kMaxLostRestartAttempts
		 * times. Only if those cheap retries fail to flip the source
		 * back to Active do we promote to a full recreate.
		 *
		 * Why: full recreate tears down and rebuilds the OBS private
		 * source (release strong ref + obs_source_create_private +
		 * settings dump + active/showing/audio/VU rewire). For a bad
		 * URL this fires on every cooldown tick and floods OBS logs
		 * with `Media Source 'xxx': settings:` blocks. media_restart
		 * reuses the same source, so the cheap path produces only the
		 * provider's own `MP: Failed` line plus our supervisor INFO,
		 * not the full settings dump.
		 *
		 * Capability matrix (purely virtual-driven):
		 *   supports_media_restart && benefits_from_recreate  (ffmpeg, vlc)
		 *     -> restart N times, then recreate, reset, repeat
		 *   supports_media_restart && !benefits_from_recreate
		 *     -> restart every cooldown (no full recreate is meaningful)
		 *   !supports_media_restart && benefits_from_recreate
		 *     -> recreate every cooldown (legacy behavior)
		 *   !supports_media_restart && !benefits_from_recreate  (ndi, spout)
		 *     -> handled by the early return above */
		if (restart_eligible && cs.lost_restart_attempts < kMaxLostRestartAttempts) {
			/* Log the ladder step + the cooldown that just elapsed BEFORE
			 * this attempt (so the 5s first step is visible), then advance. */
			const int laddered_step = backoff_ladder_step(cs.recovery_attempt + 1);
			const unsigned long long waited_s = backoff_cooldown_ns(cs.recovery_attempt) / NS_PER_SEC;
			obs_source_media_restart(raw);
			cs.lost_restart_attempts++;
			cs.recovery_attempt++;
			cs.next_retry_ns = now_ns + backoff_cooldown_ns(cs.recovery_attempt);
			cs.last_reconnect_ns = now_ns;
			amv_log_detailed(
				LOG_INFO,
				"%s[health][auto] cell (%d,%d) lost media_restart #%d/%d on '%s' (reason='%s'; ladder step %d/%d, after %llus wait)",
				log_prefix().c_str(), cellRow, cellCol, cs.lost_restart_attempts,
				kMaxLostRestartAttempts, signal_provider_to_string(cs.provider_type),
				cs.last_error_reason.c_str(), laddered_step, kBackoffLadderSteps, waited_s);
			return SignalRuntimeState::RetryScheduled;
		}

		if (!recreate_eligible) {
			/* restart-only provider: stay Lost; reset the
			 * cheap-retry counter so subsequent cooldowns keep
			 * firing restart instead of being permanently locked
			 * out after the first burst. */
			cs.lost_restart_attempts = 0;
			return SignalRuntimeState::Lost;
		}

		/* Cheap retries exhausted (or restart unsupported): fall back
		 * to full recreate. Reset the cheap counter so the next Lost
		 * window after the recreate starts again with the light path. */
		cs.lost_restart_attempts = 0;
		const int laddered_step = backoff_ladder_step(cs.recovery_attempt + 1);
		const unsigned long long waited_s = backoff_cooldown_ns(cs.recovery_attempt) / NS_PER_SEC;
		cs.recovery_attempt++;
		cs.next_retry_ns = now_ns + backoff_cooldown_ns(cs.recovery_attempt);
		cs.retry_attempt++;
		amv_log_detailed(
			LOG_INFO,
			"%s[health][auto] cell (%d,%d) scheduling full recreate of '%s' (attempt #%d, reason='%s'; ladder step %d/%d, after %llus wait)",
			log_prefix().c_str(), cellRow, cellCol, signal_provider_to_string(cs.provider_type),
			cs.retry_attempt, cs.last_error_reason.c_str(), laddered_step, kBackoffLadderSteps, waited_s);

		/* Queue refresh_cell onto the Qt main thread — it must NOT
		 * run while we hold source_mutex_. By the time the timer
		 * fires the user may have edited or cleared the cell, so we
		 * re-check via refresh_cell's normal validation path. */
		QTimer::singleShot(0, this, [this, cellRow, cellCol]() {
			if (!refresh_cell(cellRow, cellCol))
				refresh_sources();
		});
		return SignalRuntimeState::RetryScheduled;
	}

	case ISignalProvider::HealthCode::Error:
		/* Hard error \u2014 same recreate gating as Lost above, but report
		 * Error so the overlay can surface a distinct color in the
		 * future. Today the status overlay shows SIGNAL LOST for both. */
		if (cs.lost_since_ns == 0)
			cs.lost_since_ns = now_ns;
		return SignalRuntimeState::Error;

	case ISignalProvider::HealthCode::Unknown:
	default:
		return cs.state;
	}
}
