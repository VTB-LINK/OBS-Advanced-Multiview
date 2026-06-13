/*
OBS Advanced Multiview - Signal provider registry (Phase 3 / M6)

Defines the minimal interface for external signal providers (FFmpeg media,
DistroAV NDI, obs-spout2 Spout, VLC; WebRTC reserved). Providers wrap an
OBS host plugin's source type ("ffmpeg_source" / "ndi_source" /
"spout_capture" / "vlc_source") and expose a uniform availability +
lifecycle surface to the rest of the plugin.

This is the registry skeleton: it owns the provider catalog and answers
"is this provider available right now?", which is what SourcePicker /
ManagerDialog need before any provider implementation lands. Concrete
runtime methods (create/update/release/reconnect/health) are declared
here but live in per-provider .cpp files added in later milestones.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "multiview-instance.hpp"

#include <obs.h>
#include <obs.hpp>

#include <string>
#include <vector>

/* Forward declaration: the multiview window's per-cell runtime container.
 * Provider implementations mutate fields on this struct (private_source,
 * provider_type, settings hash, health timestamps). The full definition
 * lives in multiview-window.hpp; we forward-declare here to keep the
 * provider ABI symmetric and reduce header coupling. */
class MultiviewWindow;

/* Per-call snapshot of a runtime cell as the provider sees it.
 *
 * Phase C ships only the read-only fields needed for availability checks.
 * Phase D will extend this with mutable runtime references for create /
 * update / release. Keeping it as a struct rather than passing the raw
 * CellSource lets us evolve the runtime container without rebuilding
 * every provider .cpp. */
struct ProviderRuntimeView {
	int cell_row = -1;
	int cell_col = -1;
	std::string instance_uuid;
	const SignalConfig *config = nullptr; /* not owned */
};

/* Minimal provider interface. Implementations are stateless singletons
 * registered with the registry at module load. */
class ISignalProvider {
public:
	virtual ~ISignalProvider() = default;

	virtual SignalProviderType type() const = 0;

	/* Stable persisted id, identical to signal_provider_to_string(type()).
	 * Returned as const char* so callers can log / compare without
	 * std::string allocation. */
	virtual const char *id() const = 0;

	/* Human-visible label, may be localized in the future. Phase 3
	 * keeps it as plain ASCII to match the existing status overlay
	 * conventions. */
	virtual const char *display_name() const = 0;

	/* Availability check.
	 *
	 * For internal providers (PGM/PRVW/Scene/Source) this always returns
	 * true because the OBS frontend is by definition present.
	 *
	 * For external providers this should be cheap and side-effect-free:
	 * the SourcePicker may call it every time it (re)builds the tabs. The
	 * recommended first path is `obs_source_get_display_name(source_id)
	 * != nullptr`; concrete implementations may add `obs_get_source_
	 * output_flags` or `obs_get_source_defaults` for diagnostics. */
	virtual bool is_available() const = 0;

	/* Optional human-readable reason when is_available() is false. The
	 * SourcePicker shows this verbatim so users know why an external tab
	 * is disabled (e.g. "DistroAV not installed"). Empty string when the
	 * provider is available. */
	virtual std::string unavailable_reason() const = 0;

	/* Phase 3 / M6 step 9: external-provider lifecycle hook.
	 *
	 * Implementations create or update an OBS private source matching
	 * `cfg` and return a +1 strong ref via the OBSSource out-param. The
	 * caller (MultiviewWindow runtime) owns the returned ref and releases
	 * it through `release_private_source` or by destroying the OBSSource.
	 *
	 * `desired_name` is the deterministic per-cell name the runtime built
	 * (e.g. `OBS Advanced Multiview/<uuid8>/<row>,<col>/ffmpeg`).
	 * Implementations MUST pass it to obs_source_create_private so the
	 * source is visible in logs without polluting the user-facing scene
	 * list (private sources are excluded from obs_enum_sources).
	 *
	 * Internal providers (PGM/PRVW/Scene/Source) return nullptr unchanged
	 * because the runtime never asks them for a private source — they are
	 * resolved via the existing M5 weak-ref path. The default empty
	 * implementation below covers them.
	 *
	 * Must NOT be called while holding the multiview source_mutex_ (see
	 * docs/phase-3-signal-lost-and-external-sources-design.md §6 lock
	 * order). Implementations may call obs_source_create_private,
	 * obs_source_update, obs_source_release. */
	virtual OBSSource create_private_source(const std::string &desired_name, const SignalConfig &cfg) const
	{
		(void)desired_name;
		(void)cfg;
		return OBSSource();
	}

	/* Release a private source previously returned by
	 * create_private_source. Default implementation is a no-op because
	 * OBSSource RAII already drops the +1 strong ref on destruction.
	 * Providers that hold additional state (signal subscriptions, retry
	 * timers, etc.) can override to clean up. */
	virtual void release_private_source(OBSSource &src) const { src = nullptr; }

	/* Phase 3 / M6 step 10: external health supervisor surface.
	 *
	 * These three methods are the entire per-provider contract the
	 * health supervisor needs. NDI / Spout / VLC / future providers
	 * implement them and the supervisor (multiview-window-health.cpp)
	 * handles everything else: timing, backoff, recreate scheduling,
	 * overlay-state mapping. */

	/* Health codes the supervisor consumes. Provider-agnostic by design:
	 * what matters is whether the source is producing usable output, not
	 * which underlying primitive (media_state, dimensions, frame
	 * timestamps, ...) revealed it. */
	enum class HealthCode {
		Unknown, /* not probed yet / no signal */
		Active,  /* producing valid frames */
		Opening, /* attempting to open / waiting first frame / buffering */
		Paused,  /* user-initiated pause (FFmpeg/VLC media_play_pause);
		            not an error, do not escalate to Lost or attempt
		            auto-restart. Cell keeps showing the last frame. */
		Lost,    /* was producing, no longer; expected recoverable */
		Error,   /* unrecoverable from current state without recreate */
	};

	struct HealthReport {
		HealthCode code = HealthCode::Unknown;
		uint32_t width = 0;
		uint32_t height = 0;
		std::string reason; /* one-line, optional, for log + overlay */
	};

	/* Probe the current health of a live private source. age_ns is the
	 * monotonic time elapsed since create_private_source returned, so
	 * default implementations can be lenient on fresh sources still
	 * spinning up. Called at ~1 Hz from the render thread; must be
	 * cheap and side-effect-free.
	 *
	 * Default implementation looks at width/height only — covers NDI
	 * and Spout style sources that have no media-state concept. FFmpeg
	 * overrides to also inspect obs_source_media_get_state. */
	virtual HealthReport probe_health(obs_source_t *src, uint64_t age_ns) const
	{
		HealthReport r;
		if (!src)
			return r;
		r.width = obs_source_get_width(src);
		r.height = obs_source_get_height(src);
		if (r.width > 0 && r.height > 0) {
			r.code = HealthCode::Active;
		} else if (age_ns < 5ULL * 1000 * 1000 * 1000) {
			/* Young source still spinning up; report as Opening so
			 * the supervisor doesn't escalate to Lost prematurely. */
			r.code = HealthCode::Opening;
		} else {
			r.code = HealthCode::Lost;
		}
		return r;
	}

	/* Does obs_source_media_restart() do anything useful for this
	 * provider? FFmpeg/VLC: yes (re-opens the input via the worker
	 * thread). NDI/Spout: no (no media-state, no restart semantics —
	 * recovery happens via the host plugin's own discovery). When
	 * false, the supervisor skips the restart phase entirely and goes
	 * straight to Lost-then-recreate when applicable. */
	virtual bool supports_media_restart() const { return false; }

	/* When the source is Lost, does releasing + recreating the private
	 * source plausibly recover it? FFmpeg/VLC: yes (re-resolve URL,
	 * re-open file). NDI: no by default (the underlying NDI receiver
	 * recovers automatically when the source returns; recreating just
	 * churns sockets). Spout: no (sender presence is fully out of our
	 * control; recreating doesn't conjure a sender). When false, the
	 * supervisor never schedules a full recreate; the cell sits in Lost
	 * and recovers when the provider's host plugin observes the source
	 * return. */
	virtual bool benefits_from_recreate() const { return false; }

	/* Phase 3 / M6.1 perf: should the multiview runtime force the OBS
	 * async-display pipeline into unbuffered mode for this private
	 * source?
	 *
	 * Two cases the providers actually need:
	 *   - Local files (FFmpeg/VLC): YES. Decoder always has the next
	 *     frame ready by tick time; OBS's default buffered timing
	 *     (frame->timestamp + timing_adjust) just produces a 30s
	 *     timing-convergence stutter window before settling at native
	 *     fps. Skipping it gives instant 60 fps from the first frame.
	 *   - Network streams (FFmpeg URL): NO. Frames arrive unevenly;
	 *     buffered timing is exactly what makes playback look smooth.
	 *     Unbuffered renders at 60 fps but visually 'show frame N a
	 *     few times, jump to frame N+5' = stutter.
	 *   - NDI: YES. DistroAV is already a low-latency monitoring
	 *     protocol; the receiver drops late frames internally.
	 *     Unbuffered = lowest possible monitoring delay.
	 *   - Spout: YES. Pure GPU-shared-texture, every tick has the
	 *     latest frame the sender wrote. No timing math needed.
	 *
	 * Default returns false so a new provider is buffered (safe for
	 * any source type that produces frames at irregular wall-clock
	 * intervals). cfg lets per-provider impls inspect settings (e.g.
	 * FFmpeg toggles per is_local_file). The decision is read once
	 * after create_private_source by the multiview runtime, which
	 * applies obs_source_set_async_unbuffered. Providers should NOT
	 * call obs_source_set_async_unbuffered themselves \u2014 routing it
	 * through one place keeps the policy testable and the perf
	 * tradeoffs documented in one spot. */
	virtual bool prefers_unbuffered_async(const SignalConfig &cfg) const
	{
		(void)cfg;
		return false;
	}

	/* Phase 3 / M6.6 H.3: shared helper for create_private_source.
	 *
	 * All four external providers (FFmpeg, NDI, Spout, VLC) need to
	 * deep-copy the user's persisted providerSettings into a fresh
	 * obs_data_t so subsequent default re-assertions / hard-locks
	 * don't leak back into the persisted SignalConfig. The cheapest
	 * deep copy that doesn't drag in obs_data_apply's default-
	 * propagation behavior is a JSON round-trip:
	 *
	 *   obs_data_get_json(src) -> obs_data_create_from_json -> obs_data_apply
	 *
	 * This helper centralizes that idiom. Returns a +1 obs_data_t
	 * (callers must obs_data_release). Always returns a non-null
	 * pointer so callers don't need to null-check; an empty src
	 * just yields an empty obs_data_t. */
	static obs_data_t *deep_copy_provider_settings(obs_data_t *src)
	{
		obs_data_t *out = obs_data_create();
		if (!src)
			return out;
		const char *json = obs_data_get_json(src);
		if (!json || !*json)
			return out;
		obs_data_t *copy = obs_data_create_from_json(json);
		if (copy) {
			obs_data_apply(out, copy);
			obs_data_release(copy);
		}
		return out;
	}
};

/* Registry singleton. */
class SignalProviderRegistry {
public:
	static SignalProviderRegistry &instance();

	/* Register a provider. Last registration wins for a given type so
	 * tests / future overrides can replace built-ins. */
	void register_provider(ISignalProvider *provider);

	/* Lookup by enum or persisted string. Returns nullptr if no provider
	 * for the given type has been registered. */
	const ISignalProvider *find(SignalProviderType type) const;
	const ISignalProvider *find(const char *persisted_id) const;

	/* Iterate all registered providers in registration order. The
	 * SourcePicker uses this to build its provider-tab list once at
	 * dialog open time. */
	const std::vector<ISignalProvider *> &providers() const { return providers_; }

	/* Build a list of provider types currently available. Cheap; do not
	 * cache across SourcePicker open events because availability can
	 * change at runtime when the user installs/removes plugins. */
	std::vector<SignalProviderType> available_types() const;

private:
	SignalProviderRegistry() = default;
	~SignalProviderRegistry() = default;
	SignalProviderRegistry(const SignalProviderRegistry &) = delete;
	SignalProviderRegistry &operator=(const SignalProviderRegistry &) = delete;

	std::vector<ISignalProvider *> providers_;
};

/* Module-level entry points. Called from plugin-main during obs_module_load
 * and obs_module_unload so registry lifetime matches the plugin DLL. */
void signal_provider_registry_init();
void signal_provider_registry_shutdown();
