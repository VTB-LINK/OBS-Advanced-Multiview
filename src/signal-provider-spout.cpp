/*
OBS Advanced Multiview - obs-spout2 Spout signal provider (Phase 3 / M6.3)

Wraps obs-spout2's `spout_capture` so multiview cells can host Spout
sender output without the user manually adding the source. The source
is created as a private OBS source so it stays out of OBS's Sources
dock.

Discovery is performed via obs-spout2's own property callback, which
calls SpoutLibrary's GetSenderCount / GetSender on the source's
spout_receiver_ptr. We use a long-lived dormant private "discovery
probe" spout_capture source so the property dereference is always
safe (matches the NDI provider's discovery probe pattern).

Scope of this milestone (matches plan.md M6.3 first slice):
  - Availability detection (obs-spout2 installed? spout_capture
    registered?)
  - One-shot + manual Refresh discovery via the dormant probe
  - create_private_source with sender name + composite/tick settings
  - probe_health via the generic ISignalProvider default (width/height)
  - supports_media_restart=false, benefits_from_recreate=false
    (Spout sender presence is fully out of OBS's control; recreating
    the receiver doesn't conjure a sender. Recovery happens
    automatically when the sender returns and width/height go non-zero.)
  - prefers_unbuffered_async=true (GPU shared texture, every tick has
    the sender's latest write)

Spout has no audio. The supervisor and VU paths already handle that
case correctly: rebuild_volmeters skips Spout cells (configured in
M6.0 step 8), and the supervisor doesn't synthesize audio expectations.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "signal-provider.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char *kSpoutSourceId = "spout_capture";

/* obs-spout2 setting keys, mirrored verbatim from
 * win-spout-source.cpp so persisted JSON matches what the user would
 * see in OBS's own spout_capture dialog. */
constexpr const char *kKeySenderList = "spoutsenders";
constexpr const char *kKeyCompositeMode = "compositemode";
constexpr const char *kKeyTickSpeedLimit = "tickspeedlimit";

/* Special "first available sender" string token; obs-spout2 treats this
 * as a literal value of spoutsenders meaning "auto-pick whatever
 * sender is registered first". */
constexpr const char *kUseFirstAvailableSender = "usefirstavailablesender";

/* Composite mode integers from win-spout-source.cpp's #defines. */
constexpr int kCompositeModeOpaque = 1;
constexpr int kCompositeModeAlpha = 2;
constexpr int kCompositeModeDefault = 3;
constexpr int kCompositeModePremultiplied = 4;

/* Forward declarations: spout_discovery_scan and its 1Hz cache live
 * below the SpoutProvider class but the class needs them for
 * probe_health. */
std::vector<std::string> spout_discovery_scan();
std::vector<std::string> spout_discovery_scan_cached();

class SpoutProvider : public ISignalProvider {
public:
	SignalProviderType type() const override { return SignalProviderType::Spout; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "Spout (obs-spout2)"; }

	bool is_available() const override
	{
		if (!signal_provider_supported_on_platform(SignalProviderType::Spout))
			return false;
		return obs_source_get_display_name(kSpoutSourceId) != nullptr;
	}

	std::string unavailable_reason() const override
	{
		if (is_available())
			return std::string();
		if (!signal_provider_supported_on_platform(SignalProviderType::Spout))
			return signal_provider_unsupported_platform_reason(SignalProviderType::Spout);
		return "obs-spout2 plugin not installed (Windows-only Spout receiver).";
	}

	OBSSource create_private_source(const std::string &desired_name, const SignalConfig &cfg) const override
	{
		if (!is_available()) {
			obs_log(LOG_WARNING,
				"[signal-provider/spout] create skipped for '%s': spout_capture unavailable",
				desired_name.c_str());
			return OBSSource();
		}

		obs_data_t *src = cfg.providerSettings;
		const char *sender_name = src ? obs_data_get_string(src, kKeySenderList) : nullptr;
		if (!sender_name || !*sender_name) {
			/* obs-spout2 itself defaults to the literal string
			 * "usefirstavailablesender", which always works (it
			 * resolves to whatever sender is up at that moment).
			 * Match that fallback so a config without the key
			 * still produces a usable cell. */
			sender_name = kUseFirstAvailableSender;
			obs_log(LOG_INFO, "[signal-provider/spout] no sender selected, defaulting to first-available");
		}

		/* Deep-copy via the shared helper so our defaults don't leak
		 * back into the persisted config. */
		obs_data_t *settings = ISignalProvider::deep_copy_provider_settings(src);
		/* Always re-assert sender_name so a missing-key config still
		 * resolves to first-available. */
		obs_data_set_string(settings, kKeySenderList, sender_name);

		/* obs-spout2 defaults: composite=Default, tick=Fast(100ms).
		 * Re-apply only when missing so user choices survive. */
		if (!obs_data_has_user_value(settings, kKeyCompositeMode))
			obs_data_set_int(settings, kKeyCompositeMode, kCompositeModeDefault);
		/* obs-spout2 ships Fast (100 ms) as its own default but we
		 * pin Normal (500 ms) for multiview cells: the form picks
		 * 500 by default, and a missing-key fallback should match
		 * what the user would see with the form. */
		if (!obs_data_has_user_value(settings, kKeyTickSpeedLimit))
			obs_data_set_int(settings, kKeyTickSpeedLimit, 500);

		obs_source_t *raw = obs_source_create_private(kSpoutSourceId, desired_name.c_str(), settings);
		obs_data_release(settings);
		if (!raw) {
			obs_log(LOG_WARNING, "[signal-provider/spout] obs_source_create_private failed for '%s'",
				desired_name.c_str());
			return OBSSource();
		}

		amv_log_detailed(LOG_INFO, "[signal-provider/spout] created private source '%s' sender='%s'",
				 desired_name.c_str(), sender_name);

		OBSSource wrapper(raw);
		obs_source_release(raw);
		return wrapper;
	}

	/* probe_health override: the default ISignalProvider impl (width
	 * + height + 5s grace) is insufficient for Spout because obs-spout2
	 * caches the last successful sender's width/height fields and
	 * returns them from get_width/get_height even after the sender has
	 * gone away. Worse, in usefirstavailablesender mode obs-spout2
	 * silently re-binds to whatever sender is up first, so the cell
	 * keeps painting valid frames — from a different source than the
	 * user picked.
	 *
	 * The robust check is to ask our discovery probe (the same one the
	 * SourcePicker / EditSource forms use) whether the cell's wanted
	 * sender is currently registered on this machine, then combine
	 * that with the standard width/height + grace check. */
	HealthReport probe_health(obs_source_t *src, uint64_t age_ns) const override
	{
		HealthReport r;
		if (!src)
			return r;
		r.width = obs_source_get_width(src);
		r.height = obs_source_get_height(src);

		/* Pull the cell's persisted sender choice straight from the
		 * live private source so we don't have to plumb SignalConfig
		 * through the supervisor. obs_source_get_settings returns an
		 * incremented ref. */
		std::string wanted;
		obs_data_t *cur = obs_source_get_settings(src);
		if (cur) {
			const char *s = obs_data_get_string(cur, kKeySenderList);
			if (s)
				wanted.assign(s);
			obs_data_release(cur);
		}
		if (wanted.empty())
			wanted = kUseFirstAvailableSender;

		/* Cached so the 1 Hz supervisor doesn't bombard obs-spout2's
		 * property scan; the cache TTL is 1 s so the supervisor's
		 * verdict tracks reality within a single tick. */
		const auto avail = spout_discovery_scan_cached();
		bool sender_present = false;
		if (wanted == kUseFirstAvailableSender) {
			sender_present = !avail.empty();
		} else {
			sender_present = std::find(avail.begin(), avail.end(), wanted) != avail.end();
		}

		constexpr uint64_t kGraceNs = 5ULL * 1000 * 1000 * 1000;
		if (sender_present && r.width > 0 && r.height > 0) {
			r.code = HealthCode::Active;
		} else if (age_ns < kGraceNs) {
			/* Cold start: discovery probe and the cell's own
			 * receiver both need a beat to come up. Don't escalate
			 * to Lost yet. */
			r.code = HealthCode::Opening;
		} else {
			r.code = HealthCode::Lost;
			if (!sender_present) {
				if (wanted == kUseFirstAvailableSender)
					r.reason = "No Spout senders on this machine";
				else
					r.reason = "Spout sender '" + wanted + "' not present";
			} else {
				r.reason = "Spout sender present but receiver has no frame";
			}
		}
		return r;
	}

	bool supports_media_restart() const override { return false; }
	bool benefits_from_recreate() const override { return false; }
	bool prefers_unbuffered_async(const SignalConfig &) const override { return true; }
};

static SpoutProvider g_spout_provider;

/* ========== Discovery helpers ==========
 *
 * obs-spout2's win_spout_properties dereferences the source's
 * spout_receiver_ptr to call GetSenderCount / GetSender. The receiver
 * is created in win_spout_source_create (synchronously) so a freshly
 * created private spout_capture source is always safe to query \u2014 but
 * destroying it between obs_source_properties() returning and our
 * caller copying the strings would race their internal cleanup.
 *
 * Same lifetime model as NDI: keep one long-lived private probe per
 * OBS session. The probe is dormant (no inc_active), bound to
 * usefirstavailablesender so it doesn't lock onto a specific sender
 * and prevent that sender from being available to a real cell. */

static obs_source_t *g_discovery_probe = nullptr;
static std::mutex g_discovery_mutex;

static void ensure_discovery_probe_locked()
{
	if (g_discovery_probe)
		return;
	if (obs_source_get_display_name(kSpoutSourceId) == nullptr)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, kKeySenderList, kUseFirstAvailableSender);
	/* Slowest tick so the dormant probe imposes minimal overhead. */
	obs_data_set_int(settings, kKeyTickSpeedLimit, 1000);
	obs_data_set_int(settings, kKeyCompositeMode, kCompositeModeDefault);
	g_discovery_probe =
		obs_source_create_private(kSpoutSourceId, "OBS Advanced Multiview/Spout discovery probe", settings);
	obs_data_release(settings);
	if (!g_discovery_probe)
		obs_log(LOG_WARNING, "[signal-provider/spout] failed to create Spout discovery probe");
}

std::vector<std::string> spout_discovery_scan()
{
	std::vector<std::string> out;
	std::lock_guard<std::mutex> lock(g_discovery_mutex);
	ensure_discovery_probe_locked();
	if (!g_discovery_probe)
		return out;

	obs_properties_t *props = obs_source_properties(g_discovery_probe);
	if (!props)
		return out;

	obs_property_t *list = obs_properties_get(props, kKeySenderList);
	if (list) {
		const size_t n = obs_property_list_item_count(list);
		for (size_t i = 0; i < n; i++) {
			const char *name = obs_property_list_item_string(list, i);
			if (!name || !*name)
				continue;
			/* Skip the synthetic "usefirstavailablesender" entry;
			 * the form will surface that as a dedicated checkbox. */
			if (std::string(name) == kUseFirstAvailableSender)
				continue;
			out.emplace_back(name);
		}
	}
	obs_properties_destroy(props);

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

/* 1 Hz cached wrapper for the health supervisor. The supervisor probes
 * every external cell at ~1 Hz, so without a cache a multiview with N
 * Spout cells would trigger N obs_source_properties scans per tick.
 * The cache TTL is intentionally short (1000 ms) so a sender that
 * appears or disappears is reflected within one supervisor cycle.
 *
 * UI Refresh button does NOT go through this cache — it calls
 * spout_discovery_scan() directly via signal_provider_spout_discover_senders(),
 * so a manual click always sees the freshest enumeration. */
std::vector<std::string> spout_discovery_scan_cached()
{
	using clock = std::chrono::steady_clock;
	static std::mutex cache_mutex;
	static std::vector<std::string> cached;
	static clock::time_point last_scan;
	static bool seeded = false;

	const auto now = clock::now();
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (seeded && (now - last_scan) < std::chrono::milliseconds(1000))
			return cached;
	}

	/* Run the actual scan outside the cache lock so concurrent
	 * callers don't serialize behind a slow GetSenderCount call. The
	 * spout_discovery_scan itself is mutex-protected internally for
	 * the long-lived probe pointer, so this is safe. */
	auto fresh = spout_discovery_scan();

	std::lock_guard<std::mutex> lock(cache_mutex);
	cached = std::move(fresh);
	last_scan = now;
	seeded = true;
	return cached;
}

void spout_discovery_shutdown()
{
	std::lock_guard<std::mutex> lock(g_discovery_mutex);
	if (g_discovery_probe) {
		obs_source_release(g_discovery_probe);
		g_discovery_probe = nullptr;
	}
}

} /* anonymous namespace */

/* ========== Module entry points ========== */

void register_spout_provider()
{
	SignalProviderRegistry::instance().register_provider(&g_spout_provider);
}

std::vector<std::string> signal_provider_spout_discover_senders()
{
	if (!signal_provider_supported_on_platform(SignalProviderType::Spout))
		return {};
	return spout_discovery_scan();
}

void signal_provider_spout_shutdown()
{
	spout_discovery_shutdown();
}
