/*
OBS Advanced Multiview - signal / lost-signal / provider serialization (issue #10 split)

LostSignal/CellLostSignal/SceneClickSwitch settings, SignalConfig + CellAssignment
(de)serialization, the SignalProviderType<->string + platform-support helpers, and
clone_obs_data. Struct declarations live in multiview-instance.hpp; enum<->string
helpers in multiview-instance-serialize.hpp.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-instance.hpp"
#include "multiview-instance-serialize.hpp"

#include <obs.h>
#include <obs-data.h>

#include <cstring>

/* ========== LostSignalSettings (Phase 3 / M5) ==========
 *
 * fallbackType is intentionally a free-form string mirroring CellAssignment:
 *   - ""        : fallback disabled
 *   - "image"   : fallbackName is an absolute file path
 *   - "pgm"     : current Program scene
 *   - "prvw"    : current Preview scene
 *   - "scene"   : OBS scene name in fallbackName
 *   - "source"  : OBS source name in fallbackName
 * Future external-source fallback can extend this set without breaking
 * persistence; unknown values resolve to "" (disabled) on load.
 */

obs_data_t *LostSignalSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "internalMissingBehavior", internal_missing_behavior_to_str(internalMissingBehavior));
	obs_data_set_string(data, "externalLostBehavior", external_lost_behavior_to_str(externalLostBehavior));
	obs_data_set_string(data, "placeholderImagePath", placeholderImagePath.c_str());
	obs_data_set_string(data, "signalLostImagePath", signalLostImagePath.c_str());
	obs_data_set_string(data, "placeholderImageFitMode", image_fit_mode_to_str(placeholderImageFitMode));
	obs_data_set_string(data, "signalLostImageFitMode", image_fit_mode_to_str(signalLostImageFitMode));
	obs_data_set_string(data, "fallbackImageFitMode", image_fit_mode_to_str(fallbackImageFitMode));
	obs_data_set_string(data, "fallbackType", fallbackType.c_str());
	obs_data_set_string(data, "fallbackName", fallbackName.c_str());
	obs_data_set_int(data, "retryInitialMs", retryInitialMs);
	obs_data_set_int(data, "retryMaxMs", retryMaxMs);
	obs_data_set_int(data, "manualReconnectCooldownMs", manualReconnectCooldownMs);
	return data;
}

LostSignalSettings LostSignalSettings::from_obs_data(obs_data_t *data)
{
	LostSignalSettings s;
	if (!data)
		return s;
	s.internalMissingBehavior =
		internal_missing_behavior_from_str(obs_data_get_string(data, "internalMissingBehavior"));
	s.externalLostBehavior = external_lost_behavior_from_str(obs_data_get_string(data, "externalLostBehavior"));
	s.placeholderImagePath = obs_data_get_string(data, "placeholderImagePath");
	s.signalLostImagePath = obs_data_get_string(data, "signalLostImagePath");

	/* Phase 3 / M6.6 H.5 hardening: clamp path strings to a reasonable
	 * upper bound so a hand-edited config can't blow up the renderer's
	 * gs_image_file loader with a multi-MB filename. PATH_MAX on Windows
	 * is 260 by default, but Win32 long-path support extends it to 32767.
	 * We pick 4096 as a generous compromise: large enough for any real
	 * path the user might pick (including UNC + Unicode), small enough
	 * that bad data is bounded. Same idiom as LabelSettings::fontFamily
	 * clamp from Phase 2 hardening. */
	constexpr size_t kMaxPathBytes = 4096;
	if (s.placeholderImagePath.size() > kMaxPathBytes)
		s.placeholderImagePath.resize(kMaxPathBytes);
	if (s.signalLostImagePath.size() > kMaxPathBytes)
		s.signalLostImagePath.resize(kMaxPathBytes);

	/* Fit modes: missing / unknown values fall back to enum default
	 * (Stretch) via image_fit_mode_from_str() — consistent with how
	 * BackgroundSettings handles the same key on legacy configs. */
	if (obs_data_has_user_value(data, "placeholderImageFitMode"))
		s.placeholderImageFitMode =
			image_fit_mode_from_str(obs_data_get_string(data, "placeholderImageFitMode"));
	if (obs_data_has_user_value(data, "signalLostImageFitMode"))
		s.signalLostImageFitMode = image_fit_mode_from_str(obs_data_get_string(data, "signalLostImageFitMode"));
	if (obs_data_has_user_value(data, "fallbackImageFitMode"))
		s.fallbackImageFitMode = image_fit_mode_from_str(obs_data_get_string(data, "fallbackImageFitMode"));

	/* Whitelist fallbackType so an arbitrary string from disk can never reach
	 * the runtime. Anything unrecognised becomes "" (disabled) and the
	 * fallbackName is kept verbatim so the user can re-enable later. */
	const char *ft = obs_data_get_string(data, "fallbackType");
	if (ft && (strcmp(ft, "image") == 0 || strcmp(ft, "pgm") == 0 || strcmp(ft, "prvw") == 0 ||
		   strcmp(ft, "scene") == 0 || strcmp(ft, "source") == 0)) {
		s.fallbackType = ft;
	} else {
		s.fallbackType.clear();
	}
	s.fallbackName = obs_data_get_string(data, "fallbackName");

	/* Phase 3 / M6.6 H.5 hardening: clamp fallbackName length too. When
	 * fallbackType == "image" this is an absolute path (same renderer as
	 * placeholderImagePath); for "scene"/"source" it's an OBS source name
	 * (which OBS itself caps in practice but our config could carry
	 * arbitrary length). Same 4096 cap as the image-path fields above. */
	if (s.fallbackName.size() > 4096)
		s.fallbackName.resize(4096);

	if (obs_data_has_user_value(data, "retryInitialMs"))
		s.retryInitialMs = (int)obs_data_get_int(data, "retryInitialMs");
	if (obs_data_has_user_value(data, "retryMaxMs"))
		s.retryMaxMs = (int)obs_data_get_int(data, "retryMaxMs");
	if (obs_data_has_user_value(data, "manualReconnectCooldownMs"))
		s.manualReconnectCooldownMs = (int)obs_data_get_int(data, "manualReconnectCooldownMs");

	/* Clamps: match design defaults in [docs/phase-3-signal-lost-and-external-sources-design.md] §10
	 * (1s..30s backoff, 1s manual cooldown). Out-of-range values can come from
	 * hand-edited configs or future versions; clamp instead of reject. */
	if (s.retryInitialMs < 100)
		s.retryInitialMs = 100;
	if (s.retryInitialMs > 60000)
		s.retryInitialMs = 60000;
	if (s.retryMaxMs < s.retryInitialMs)
		s.retryMaxMs = s.retryInitialMs;
	if (s.retryMaxMs > 600000)
		s.retryMaxMs = 600000;
	if (s.manualReconnectCooldownMs < 0)
		s.manualReconnectCooldownMs = 0;
	if (s.manualReconnectCooldownMs > 60000)
		s.manualReconnectCooldownMs = 60000;
	return s;
}

/* ========== CellLostSignalSettings (Phase 3 / M5) ========== */

obs_data_t *CellLostSignalSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "row", row);
	obs_data_set_int(data, "col", col);
	obs_data_set_string(data, "mode", inheritance_mode_to_str(mode));
	obs_data_t *inner = settings.to_obs_data();
	obs_data_set_obj(data, "settings", inner);
	obs_data_release(inner);
	return data;
}

CellLostSignalSettings CellLostSignalSettings::from_obs_data(obs_data_t *data)
{
	CellLostSignalSettings c;
	if (!data)
		return c;
	c.row = (int)obs_data_get_int(data, "row");
	c.col = (int)obs_data_get_int(data, "col");
	c.mode = inheritance_mode_from_str(obs_data_get_string(data, "mode"));
	obs_data_t *inner = obs_data_get_obj(data, "settings");
	c.settings = LostSignalSettings::from_obs_data(inner);
	if (inner)
		obs_data_release(inner);
	return c;
}

LostSignalSettings resolve_effective_lost_signal(const LostSignalSettings &global, const CellLostSignalSettings *cell)
{
	if (cell && cell->mode == InheritanceMode::Override)
		return cell->settings;
	return global;
}

/* ========== SceneClickSwitchSettings ========== */

obs_data_t *SceneClickSwitchSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_bool(data, "doubleClickProgramEnabled", doubleClickProgramEnabled);
	return data;
}

SceneClickSwitchSettings SceneClickSwitchSettings::from_obs_data(obs_data_t *data)
{
	SceneClickSwitchSettings s;
	if (!data)
		return s;
	/* Default true (parity with OBS built-in MultiviewMouseSwitch) when key
	 * is absent on legacy configs. */
	if (obs_data_has_user_value(data, "enabled"))
		s.enabled = obs_data_get_bool(data, "enabled");
	if (obs_data_has_user_value(data, "doubleClickProgramEnabled"))
		s.doubleClickProgramEnabled = obs_data_get_bool(data, "doubleClickProgramEnabled");
	return s;
}

/* ---------- SignalProviderType ---------- */

const char *signal_provider_to_string(SignalProviderType p)
{
	switch (p) {
	case SignalProviderType::InternalPgm:
		return "internal_pgm";
	case SignalProviderType::InternalPrvw:
		return "internal_prvw";
	case SignalProviderType::InternalScene:
		return "internal_scene";
	case SignalProviderType::InternalSource:
		return "internal_source";
	case SignalProviderType::Ffmpeg:
		return "ffmpeg";
	case SignalProviderType::Ndi:
		return "ndi";
	case SignalProviderType::Spout:
		return "spout";
	case SignalProviderType::Vlc:
		return "vlc";
	case SignalProviderType::WebRtcReserved:
		return "webrtc_reserved";
	case SignalProviderType::Unknown:
	default:
		return "unknown";
	}
}

SignalProviderType signal_provider_from_string(const char *s)
{
	if (!s || !*s)
		return SignalProviderType::Unknown;
	if (strcmp(s, "internal_pgm") == 0)
		return SignalProviderType::InternalPgm;
	if (strcmp(s, "internal_prvw") == 0)
		return SignalProviderType::InternalPrvw;
	if (strcmp(s, "internal_scene") == 0)
		return SignalProviderType::InternalScene;
	if (strcmp(s, "internal_source") == 0)
		return SignalProviderType::InternalSource;
	if (strcmp(s, "ffmpeg") == 0)
		return SignalProviderType::Ffmpeg;
	if (strcmp(s, "ndi") == 0)
		return SignalProviderType::Ndi;
	if (strcmp(s, "spout") == 0)
		return SignalProviderType::Spout;
	if (strcmp(s, "vlc") == 0)
		return SignalProviderType::Vlc;
	if (strcmp(s, "webrtc_reserved") == 0)
		return SignalProviderType::WebRtcReserved;
	return SignalProviderType::Unknown;
}

bool signal_provider_is_internal(SignalProviderType p)
{
	switch (p) {
	case SignalProviderType::InternalPgm:
	case SignalProviderType::InternalPrvw:
	case SignalProviderType::InternalScene:
	case SignalProviderType::InternalSource:
		return true;
	default:
		return false;
	}
}

bool signal_provider_supported_on_platform(SignalProviderType p)
{
	switch (p) {
	case SignalProviderType::Spout:
		/* obs-spout2 is built on Windows-only DirectX shared
		 * textures and Off-World-Live ships no macOS / Linux
		 * build. Hard-gate on _WIN32 so non-Windows OBS never
		 * tries to construct a spout_capture private source. */
#if defined(_WIN32)
		return true;
#else
		return false;
#endif
	default:
		/* Internal providers, FFmpeg (built into libobs), NDI
		 * (DistroAV ships universal), VLC (vlc-video bundled with
		 * official builds), and the reserved WebRTC slot are all
		 * cross-platform. */
		return true;
	}
}

const char *signal_provider_unsupported_platform_reason(SignalProviderType p)
{
	if (signal_provider_supported_on_platform(p))
		return "";
	switch (p) {
	case SignalProviderType::Spout:
		return "Spout is a Windows-only protocol (DirectX shared textures); obs-spout2 has no macOS or Linux build.";
	default:
		return "This provider is not supported on the current platform.";
	}
}

/* ---------- SignalConfig ---------- */

/* OBS does not expose a deep-copy API for obs_data_t, so we round-trip
 * through JSON. This is the same approach used elsewhere in OBS plugins
 * for cloning settings objects and is safe for the simple key/value
 * shapes we expect from provider settings. */
static obs_data_t *clone_obs_data(obs_data_t *src)
{
	if (!src)
		return nullptr;
	const char *json = obs_data_get_json(src);
	if (!json || !*json)
		return obs_data_create();
	return obs_data_create_from_json(json);
}

SignalConfig::SignalConfig(const SignalConfig &other)
	: provider(other.provider),
	  displayName(other.displayName),
	  providerSettings(clone_obs_data(other.providerSettings))
{
}

SignalConfig::SignalConfig(SignalConfig &&other) noexcept
	: provider(other.provider),
	  displayName(std::move(other.displayName)),
	  providerSettings(other.providerSettings)
{
	other.provider = SignalProviderType::Unknown;
	other.providerSettings = nullptr;
}

SignalConfig &SignalConfig::operator=(const SignalConfig &other)
{
	if (this == &other)
		return *this;
	if (providerSettings)
		obs_data_release(providerSettings);
	provider = other.provider;
	displayName = other.displayName;
	providerSettings = clone_obs_data(other.providerSettings);
	return *this;
}

SignalConfig &SignalConfig::operator=(SignalConfig &&other) noexcept
{
	if (this == &other)
		return *this;
	if (providerSettings)
		obs_data_release(providerSettings);
	provider = other.provider;
	displayName = std::move(other.displayName);
	providerSettings = other.providerSettings;
	other.provider = SignalProviderType::Unknown;
	other.providerSettings = nullptr;
	return *this;
}

SignalConfig::~SignalConfig()
{
	if (providerSettings) {
		obs_data_release(providerSettings);
		providerSettings = nullptr;
	}
}

obs_data_t *SignalConfig::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "provider", signal_provider_to_string(provider));
	obs_data_set_string(data, "displayName", displayName.c_str());
	if (providerSettings) {
		obs_data_t *snap = clone_obs_data(providerSettings);
		if (snap) {
			obs_data_set_obj(data, "settings", snap);
			obs_data_release(snap);
		}
	}
	return data;
}

SignalConfig SignalConfig::from_obs_data(obs_data_t *data)
{
	SignalConfig cfg;
	if (!data)
		return cfg;
	cfg.provider = signal_provider_from_string(obs_data_get_string(data, "provider"));
	cfg.displayName = obs_data_get_string(data, "displayName");
	if (obs_data_has_user_value(data, "settings")) {
		obs_data_t *inner = obs_data_get_obj(data, "settings");
		if (inner) {
			cfg.providerSettings = clone_obs_data(inner);
			obs_data_release(inner);
		}
	}
	return cfg;
}

/* ---------- CellAssignment ---------- */

obs_data_t *CellAssignment::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "row", row);
	obs_data_set_int(data, "col", col);
	obs_data_set_string(data, "type", type.c_str());
	obs_data_set_string(data, "name", name.c_str());

	/* Phase 3 / M6: only persist signalConfig for non-empty external
	 * cells, plus the forward-compat case where an unknown provider type
	 * was loaded with settings we don't yet understand (we keep the raw
	 * payload so a future build can resume it). Internal cells stay
	 * byte-compatible with M5 v3 configs — no extra keys, no empty
	 * objects. */
	const bool persist_signal_config =
		signalConfig.is_external() ||
		(signalConfig.provider == SignalProviderType::Unknown && signalConfig.providerSettings != nullptr);
	if (persist_signal_config) {
		obs_data_t *cfg = signalConfig.to_obs_data();
		if (cfg) {
			obs_data_set_obj(data, "signalConfig", cfg);
			obs_data_release(cfg);
		}
	}
	return data;
}

CellAssignment CellAssignment::from_obs_data(obs_data_t *data)
{
	CellAssignment ca;
	ca.row = (int)obs_data_get_int(data, "row");
	ca.col = (int)obs_data_get_int(data, "col");
	/* Legacy data without row/col will read as 0,0 - we use a sentinel
	 * to detect this case: if "row" key doesn't exist, mark as -1 */
	if (!obs_data_has_user_value(data, "row"))
		ca.row = -1;
	if (!obs_data_has_user_value(data, "col"))
		ca.col = -1;
	ca.type = obs_data_get_string(data, "type");
	ca.name = obs_data_get_string(data, "name");

	if (obs_data_has_user_value(data, "signalConfig")) {
		obs_data_t *cfg = obs_data_get_obj(data, "signalConfig");
		ca.signalConfig = SignalConfig::from_obs_data(cfg);
		obs_data_release(cfg);
	}
	return ca;
}
