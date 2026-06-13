/*
OBS Advanced Multiview - DistroAV NDI signal provider (Phase 3 / M6.2)

Wraps DistroAV's `ndi_source` so multiview cells can host NDI streams
without the user adding the source to a scene. The source is created as
a private OBS source (obs_source_create_private) so it stays out of the
OBS Sources dock.

Discovery is performed via DistroAV's own property callback (it builds a
combo box list populated by NDIFinder under the hood). We use a long-
lived private "discovery probe" source so the async callback fired by
NDIFinder can safely dereference the source's context for the rest of
the OBS session.

Scope of this milestone (matches plan.md §9 M6.2 first slice):
  - Availability detection (DistroAV installed? ndi_source registered?)
  - Safe one-shot + Refresh discovery
  - create_private_source with the essential NDI settings keys
  - probe_health via the generic ISignalProvider default (width/height)
  - supports_media_restart=false, benefits_from_recreate=false
    (NDI recovery is fully owned by DistroAV's receiver)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "signal-provider.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace {

constexpr const char *kNdiSourceId = "ndi_source";

/* DistroAV setting keys, mirrored verbatim from DistroAV's ndi-source.cpp
 * so the user can hand-edit settings.json and have it work the same way. */
constexpr const char *kKeySourceName = "ndi_source_name";
constexpr const char *kKeyBehavior = "ndi_behavior";
constexpr const char *kKeyBehaviorTimeout = "ndi_behavior_timeout";
constexpr const char *kKeyBandwidth = "ndi_bw_mode";
constexpr const char *kKeySync = "ndi_sync";
constexpr const char *kKeyFramesync = "ndi_framesync";
constexpr const char *kKeyHwAccel = "ndi_recv_hw_accel";
constexpr const char *kKeyFixAlpha = "ndi_fix_alpha_blending";
constexpr const char *kKeyYuvRange = "yuv_range";
constexpr const char *kKeyYuvColorspace = "yuv_colorspace";
constexpr const char *kKeyLatency = "latency";
constexpr const char *kKeyAudio = "ndi_audio";

constexpr int kBehaviorKeepActive = 0;

class NdiProvider : public ISignalProvider {
public:
	SignalProviderType type() const override { return SignalProviderType::Ndi; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "NDI (DistroAV)"; }

	bool is_available() const override
	{
		/* DistroAV registers `ndi_source` via obs_register_source. If
		 * obs_source_get_display_name returns null, DistroAV is not
		 * installed (or failed to load — same outcome from our side). */
		return obs_source_get_display_name(kNdiSourceId) != nullptr;
	}

	std::string unavailable_reason() const override
	{
		if (is_available())
			return std::string();
		return "DistroAV (NDI) plugin not installed or its NDI Runtime is missing.";
	}

	OBSSource create_private_source(const std::string &desired_name, const SignalConfig &cfg) const override
	{
		if (!is_available()) {
			obs_log(LOG_WARNING, "[signal-provider/ndi] create skipped for '%s': ndi_source unavailable",
				desired_name.c_str());
			return OBSSource();
		}

		obs_data_t *src = cfg.providerSettings;
		const char *ndi_name = src ? obs_data_get_string(src, kKeySourceName) : nullptr;
		if (!ndi_name || !*ndi_name) {
			obs_log(LOG_WARNING, "[signal-provider/ndi] create skipped for '%s': empty ndi_source_name",
				desired_name.c_str());
			return OBSSource();
		}

		/* Deep-copy the user's settings so our defaults don't leak
		 * back into the persisted config. Same pattern as the other
		 * external providers (helper on ISignalProvider). */
		obs_data_t *settings = ISignalProvider::deep_copy_provider_settings(src);

		/* Re-apply DistroAV's own defaults for keys the user didn't
		 * set. DistroAV's get_defaults runs at create time anyway, but
		 * being explicit here keeps the OBS log readable when
		 * log_changes prints the resolved settings. */
		if (!obs_data_has_user_value(settings, kKeyBandwidth))
			obs_data_set_int(settings, kKeyBandwidth, 0); /* Highest */
		if (!obs_data_has_user_value(settings, kKeySync))
			obs_data_set_int(settings, kKeySync, 2); /* SourceTimecode */
		if (!obs_data_has_user_value(settings, kKeyLatency))
			obs_data_set_int(settings, kKeyLatency, 0); /* Normal */
		if (!obs_data_has_user_value(settings, kKeyAudio))
			obs_data_set_bool(settings, kKeyAudio, true);
		if (!obs_data_has_user_value(settings, kKeyBehavior))
			obs_data_set_int(settings, kKeyBehavior, kBehaviorKeepActive);

		/* Phase 3 / M6.2 fix: hard-lock ndi_behavior_timeout to
		 * PROP_TIMEOUT_CLEAR_CONTENT (= 0) so the source reports
		 * width/height = 0 on disconnect and the health supervisor
		 * can flip the cell to Lost. DistroAV's default is
		 * PROP_TIMEOUT_KEEP_CONTENT (freeze last frame, dimensions
		 * stay non-zero), which would defeat lost detection.
		 *
		 * The "keep last frame" behavior is intentionally NOT
		 * exposed in our NDI form because we don't have a clean way
		 * to render a SIGNAL LOST overlay over a frozen frame
		 * (DistroAV reports the source as still active, so the
		 * supervisor never escalates). Users who want frozen-frame
		 * behavior can configure NDI sources directly inside an
		 * OBS scene and bind the cell to that scene as a normal
		 * scene cell. */
		obs_data_set_int(settings, kKeyBehaviorTimeout, 0);

		obs_source_t *raw = obs_source_create_private(kNdiSourceId, desired_name.c_str(), settings);
		obs_data_release(settings);
		if (!raw) {
			obs_log(LOG_WARNING, "[signal-provider/ndi] obs_source_create_private failed for '%s'",
				desired_name.c_str());
			return OBSSource();
		}

		/* Display-pipeline buffering policy is decided in
		 * prefers_unbuffered_async() and applied uniformly by the
		 * multiview runtime after create. */

		amv_log_detailed(LOG_INFO, "[signal-provider/ndi] created private source '%s' ndi_source_name='%s'",
				 desired_name.c_str(), ndi_name);

		OBSSource wrapper(raw);
		obs_source_release(raw);
		return wrapper;
	}

	/* probe_health: default impl (width/height + 5s grace) is already the
	 * right policy for NDI. When the NDI source is disconnected DistroAV
	 * sets width/height = 0; we treat that as Lost. Recovery happens via
	 * DistroAV's internal NDIFinder + receiver loop, so we report
	 * supports_media_restart=false (no media_restart semantics in NDI)
	 * and benefits_from_recreate=false (recreating ndi_source just churns
	 * sockets; the receiver will reconnect on its own when the source
	 * returns). The supervisor will sit in Lost / show SIGNAL LOST
	 * overlay until DistroAV brings the source back, at which point
	 * width/height go non-zero and we flip back to Active. */
	bool supports_media_restart() const override { return false; }
	bool benefits_from_recreate() const override { return false; }

	/* NDI is already a low-latency monitoring protocol; DistroAV's own
	 * latency_mode setting handles receiver-side buffering. Unbuffered
	 * display-side = lowest possible monitoring delay, matches what
	 * users opening a Multiview cell expect from "show me what NDI is
	 * sending right now". */
	bool prefers_unbuffered_async(const SignalConfig &) const override { return true; }
};

static NdiProvider g_ndi_provider;

/* ========== Discovery helpers (public-ish) ==========
 *
 * Lifetime model:
 *   We keep one long-lived private "discovery probe" ndi_source per OBS
 *   session. DistroAV's NDIFinder is a static singleton that owns the
 *   network-discovery thread; calling obs_source_properties on our probe
 *   triggers the property callback which lazily kicks NDIFinder's async
 *   refresh (5s cache window). Each call returns the *cached* list and
 *   schedules an async update of the in-memory list which we re-poll on
 *   the next call (e.g. user clicks Refresh).
 *
 *   Why a long-lived probe: DistroAV's property callback captures the
 *   source pointer in a lambda passed to NDIFinder's async thread. If we
 *   destroy the probe between the synchronous return and the async
 *   callback firing, the callback's `s->obs_source` deref will crash
 *   OBS. Keeping the probe alive for the OBS session sidesteps the
 *   lifetime puzzle entirely; the source itself is dormant
 *   (no inc_active / inc_showing) so it spends no CPU. */

static std::once_flag g_probe_init_once;
static obs_source_t *g_discovery_probe = nullptr;
static std::mutex g_discovery_mutex;

static void ensure_discovery_probe_locked()
{
	if (g_discovery_probe)
		return;
	if (obs_source_get_display_name(kNdiSourceId) == nullptr)
		return; /* DistroAV not installed */

	obs_data_t *settings = obs_data_create();
	/* Lowest bandwidth + audio off so the probe doesn't actually try to
	 * pull a stream even if it accidentally resolves a sender. The source
	 * stays inactive anyway (no scene, no inc_active), but defending in
	 * depth costs nothing. */
	obs_data_set_int(settings, kKeyBandwidth, 1 /* Lowest */);
	obs_data_set_bool(settings, kKeyAudio, false);
	g_discovery_probe =
		obs_source_create_private(kNdiSourceId, "OBS Advanced Multiview/NDI discovery probe", settings);
	obs_data_release(settings);
	if (!g_discovery_probe)
		obs_log(LOG_WARNING,
			"[signal-provider/ndi] failed to create NDI discovery probe; live list unavailable");
}

/* Returned to the SourcePicker NDI tab. Sorted, deduped, ASCII order so
 * the UI can present a stable list. Returns an empty vector if DistroAV
 * is not installed or the probe could not be created. */
std::vector<std::string> ndi_discovery_scan()
{
	std::vector<std::string> out;
	std::lock_guard<std::mutex> lock(g_discovery_mutex);
	ensure_discovery_probe_locked();
	if (!g_discovery_probe)
		return out;

	obs_properties_t *props = obs_source_properties(g_discovery_probe);
	if (!props)
		return out;

	obs_property_t *list = obs_properties_get(props, kKeySourceName);
	if (list) {
		const size_t n = obs_property_list_item_count(list);
		for (size_t i = 0; i < n; i++) {
			const char *name = obs_property_list_item_string(list, i);
			if (name && *name)
				out.emplace_back(name);
		}
	}
	obs_properties_destroy(props);

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

/* Drop the discovery probe at module unload. The OBS shutdown path
 * destroys all sources anyway, but explicit cleanup gives us a clean log
 * line and a defined teardown order. */
void ndi_discovery_shutdown()
{
	std::lock_guard<std::mutex> lock(g_discovery_mutex);
	if (g_discovery_probe) {
		obs_source_release(g_discovery_probe);
		g_discovery_probe = nullptr;
	}
}

} /* anonymous namespace */

/* ========== Module entry points ==========
 *
 * Registered through signal_provider_registry_init via a TU-local hook,
 * matching the FFmpeg provider pattern. The discovery helpers are
 * exposed to SourcePicker through free function declarations in a
 * header (provider-settings-forms.hpp pulls them in). */

void register_ndi_provider()
{
	SignalProviderRegistry::instance().register_provider(&g_ndi_provider);
}

std::vector<std::string> signal_provider_ndi_discover_sources()
{
	return ndi_discovery_scan();
}

void signal_provider_ndi_shutdown()
{
	ndi_discovery_shutdown();
}
