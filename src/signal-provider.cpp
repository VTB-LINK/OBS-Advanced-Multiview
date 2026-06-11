/*
OBS Advanced Multiview - Signal provider registry (Phase 3 / M6)

Registry implementation. Phase C ships only the registry plumbing plus
internal-provider adapters for PGM / PRVW / Scene / Source so the rest of
the plugin can transition to "ask the registry" without behavior change.
External providers are registered by their own translation units in later
milestones.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "signal-provider.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstring>

/* ========== Registry ========== */

SignalProviderRegistry &SignalProviderRegistry::instance()
{
	static SignalProviderRegistry inst;
	return inst;
}

void SignalProviderRegistry::register_provider(ISignalProvider *provider)
{
	if (!provider)
		return;

	/* Last-write-wins by type. We do not own provider lifetime: providers
	 * are static singletons living in their own translation units. */
	auto it = std::find_if(providers_.begin(), providers_.end(),
			       [&](ISignalProvider *p) { return p && p->type() == provider->type(); });
	if (it != providers_.end())
		*it = provider;
	else
		providers_.push_back(provider);
}

const ISignalProvider *SignalProviderRegistry::find(SignalProviderType type) const
{
	auto it = std::find_if(providers_.begin(), providers_.end(),
			       [&](ISignalProvider *p) { return p && p->type() == type; });
	return it == providers_.end() ? nullptr : *it;
}

const ISignalProvider *SignalProviderRegistry::find(const char *persisted_id) const
{
	if (!persisted_id || !*persisted_id)
		return nullptr;
	auto it = std::find_if(providers_.begin(), providers_.end(),
			       [&](ISignalProvider *p) { return p && std::strcmp(p->id(), persisted_id) == 0; });
	return it == providers_.end() ? nullptr : *it;
}

std::vector<SignalProviderType> SignalProviderRegistry::available_types() const
{
	std::vector<SignalProviderType> out;
	out.reserve(providers_.size());
	for (auto *p : providers_) {
		if (p && p->is_available())
			out.push_back(p->type());
	}
	return out;
}

/* ========== Internal-provider adapters ========== */

/* PGM / PRVW / Scene / Source providers all wrap the existing M5 internal
 * paths. They report `is_available() == true` unconditionally because the
 * OBS frontend is by definition present whenever this plugin loads.
 *
 * Phase C only needs them to populate the registry so SourcePicker /
 * registry consumers can iterate uniformly. The render path still goes
 * through MultiviewWindow's M5 logic; provider create/update/release for
 * internal cells will be wired in a later milestone if and when we move
 * the full M5 lifecycle behind the provider interface. */
namespace {

class InternalProviderBase : public ISignalProvider {
public:
	bool is_available() const override { return true; }
	std::string unavailable_reason() const override { return std::string(); }
};

class InternalPgmProvider : public InternalProviderBase {
public:
	SignalProviderType type() const override { return SignalProviderType::InternalPgm; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "Program (PGM)"; }
};

class InternalPrvwProvider : public InternalProviderBase {
public:
	SignalProviderType type() const override { return SignalProviderType::InternalPrvw; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "Preview (PRVW)"; }
};

class InternalSceneProvider : public InternalProviderBase {
public:
	SignalProviderType type() const override { return SignalProviderType::InternalScene; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "OBS Scene"; }
};

class InternalSourceProvider : public InternalProviderBase {
public:
	SignalProviderType type() const override { return SignalProviderType::InternalSource; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "OBS Source"; }
};

static InternalPgmProvider g_internal_pgm;
static InternalPrvwProvider g_internal_prvw;
static InternalSceneProvider g_internal_scene;
static InternalSourceProvider g_internal_source;

} /* anonymous namespace */

/* ========== Module entry points ========== */

void signal_provider_registry_init()
{
	auto &reg = SignalProviderRegistry::instance();
	reg.register_provider(&g_internal_pgm);
	reg.register_provider(&g_internal_prvw);
	reg.register_provider(&g_internal_scene);
	reg.register_provider(&g_internal_source);

	/* External providers (FFmpeg / NDI / Spout / VLC / WebRTC reserved)
	 * register themselves from their own translation units when those
	 * milestones land. Logging here gives a single anchor in the OBS log
	 * for diagnosing "no providers visible" issues. */
	obs_log(LOG_INFO, "[signal-provider] registry initialized with %zu internal provider(s)",
		(size_t)reg.providers().size());
}

void signal_provider_registry_shutdown()
{
	/* Providers are static singletons owned by translation units; we do
	 * not delete anything here. Resetting the registry is unnecessary
	 * because the entire DLL is unloading; this hook exists so future
	 * external providers that need explicit teardown have a guaranteed
	 * shutdown phase. */
}
