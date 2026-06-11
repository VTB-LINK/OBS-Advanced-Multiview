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
