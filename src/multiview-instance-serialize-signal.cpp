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

/* ---- Signal-Lost v2 <-> legacy bridge ---- */

void derive_legacy_lost_fields(LostSignalSettings &s)
{
	/* Hardening: a "Fallback" display with no usable target (empty
	 * fallbackType, or an image fallback with no path) would derive a
	 * contradictory legacy state (e.g. RetryWithFallback with nothing to
	 * show). The dialog never produces this, but a hand-edited / migrated
	 * config could — so treat an unusable fallback as Black for derivation.
	 * s.displayContent itself is left intact (the user's choice persists). */
	const bool fallback_usable = s.displayContent == LostDisplayContent::Fallback &&
				     (s.fallbackType == "pgm" || s.fallbackType == "prvw" ||
				      s.fallbackType == "scene" || s.fallbackType == "source" ||
				      (s.fallbackType == "image" && !s.fallbackName.empty()));
	const LostDisplayContent effContent = (s.displayContent == LostDisplayContent::Fallback && !fallback_usable)
						      ? LostDisplayContent::Black
						      : s.displayContent;

	/* INTERNAL render path (internalMissingBehavior + placeholder image). */
	switch (effContent) {
	case LostDisplayContent::ClearCell:
		s.internalMissingBehavior = InternalMissingBehavior::ClearCell;
		break;
	case LostDisplayContent::Fallback:
		if (s.fallbackType == "image") {
			s.internalMissingBehavior = InternalMissingBehavior::PlaceholderImage;
			s.placeholderImagePath = s.fallbackName;
			s.placeholderImageFitMode = s.fallbackImageFitMode;
		} else {
			/* pgm/prvw/scene/source: the draw fallback block renders the
			 * source via fallbackType; the base internal behavior is Black. */
			s.internalMissingBehavior = InternalMissingBehavior::Black;
		}
		break;
	case LostDisplayContent::Black:
	case LostDisplayContent::LastFrame:
	default:
		/* Internal cells have no decoded last frame to keep, so LastFrame
		 * degrades to Black (+ the missing-source overlay). */
		s.internalMissingBehavior = InternalMissingBehavior::Black;
		break;
	}

	/* EXTERNAL render path (externalLostBehavior + signal-lost image). */
	switch (effContent) {
	case LostDisplayContent::Fallback:
		if (s.fallbackType == "image") {
			s.externalLostBehavior = ExternalLostBehavior::SignalLostImage;
			s.signalLostImagePath = s.fallbackName;
			s.signalLostImageFitMode = s.fallbackImageFitMode;
		} else {
			s.externalLostBehavior = ExternalLostBehavior::RetryWithFallback;
		}
		break;
	case LostDisplayContent::Black:
	case LostDisplayContent::LastFrame:
	case LostDisplayContent::ClearCell:
	default:
		/* External cells keep the private source's last/black frame either
		 * way; the status band picks the overlay color. (None can't be
		 * fully honored through the legacy enum — it still shows the red
		 * band; acceptable until the render path is migrated.) */
		s.externalLostBehavior = (s.statusBand == LostStatusBand::Reconnecting)
						 ? ExternalLostBehavior::RetryOnly
						 : ExternalLostBehavior::SignalLostOverlay;
		break;
	}
}

void migrate_lost_settings_v1_to_v2(LostSignalSettings &s)
{
	/* Collapse the two legacy behaviors into one unified display + band.
	 * External wins when the user customized it (non-default overlay);
	 * otherwise the internal behavior drives. recoveryPolicy is migrated
	 * separately (defaults Auto = pre-v2 always-auto-reconnect). */
	if (s.externalLostBehavior != ExternalLostBehavior::SignalLostOverlay) {
		switch (s.externalLostBehavior) {
		case ExternalLostBehavior::RetryOnly:
			s.displayContent = LostDisplayContent::LastFrame;
			s.statusBand = LostStatusBand::Reconnecting;
			break;
		case ExternalLostBehavior::RetryWithFallback:
			s.displayContent = LostDisplayContent::Fallback;
			s.statusBand = LostStatusBand::Auto;
			break;
		case ExternalLostBehavior::SignalLostImage:
			if (s.signalLostImagePath.empty()) {
				/* "Show signal-lost image" but no path ever set -> Black. */
				s.displayContent = LostDisplayContent::Black;
				s.statusBand = LostStatusBand::Auto;
			} else {
				s.displayContent = LostDisplayContent::Fallback;
				s.statusBand = LostStatusBand::Auto;
				s.fallbackType = "image";
				s.fallbackName = s.signalLostImagePath;
				s.fallbackImageFitMode = s.signalLostImageFitMode;
			}
			break;
		default:
			break;
		}
	} else {
		switch (s.internalMissingBehavior) {
		case InternalMissingBehavior::PlaceholderImage:
			if (s.placeholderImagePath.empty()) {
				/* "Placeholder image" but no path ever set -> Black. */
				s.displayContent = LostDisplayContent::Black;
				s.statusBand = LostStatusBand::Auto;
			} else {
				s.displayContent = LostDisplayContent::Fallback;
				s.statusBand = LostStatusBand::Auto;
				s.fallbackType = "image";
				s.fallbackName = s.placeholderImagePath;
				s.fallbackImageFitMode = s.placeholderImageFitMode;
			}
			break;
		case InternalMissingBehavior::ClearCell:
			s.displayContent = LostDisplayContent::ClearCell;
			s.statusBand = LostStatusBand::Auto;
			break;
		case InternalMissingBehavior::Black:
		default:
			s.displayContent = LostDisplayContent::Black;
			/* Auto already derives the red MISSING/SIGNAL LOST band for the
			 * lost states, so this preserves the legacy look while showing
			 * the cleaner "Auto" default in the dialog. */
			s.statusBand = LostStatusBand::Auto;
			break;
		}
	}

	/* Defense in depth: migration may have copied a legacy image path into
	 * fallbackName after the from_obs_data clamp ran, so clamp again. */
	if (s.fallbackName.size() > 4096)
		s.fallbackName.resize(4096);
}

obs_data_t *LostSignalSettings::to_obs_data() const
{
	/* v2 canonical schema. The legacy render fields (internalMissingBehavior
	 * / externalLostBehavior / placeholder + signalLost paths / retryInitial
	 * / retryMax) are DERIVED on load and no longer persisted. */
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "displayContent", lost_display_content_to_str(displayContent));
	obs_data_set_string(data, "statusBand", lost_status_band_to_str(statusBand));
	obs_data_set_string(data, "recoveryPolicy", recovery_policy_to_str(recoveryPolicy));
	obs_data_set_string(data, "fallbackType", fallbackType.c_str());
	obs_data_set_string(data, "fallbackName", fallbackName.c_str());
	obs_data_set_string(data, "fallbackImageFitMode", image_fit_mode_to_str(fallbackImageFitMode));
	obs_data_set_int(data, "manualReconnectCooldownMs", manualReconnectCooldownMs);
	return data;
}

LostSignalSettings LostSignalSettings::from_obs_data(obs_data_t *data)
{
	LostSignalSettings s;
	if (!data)
		return s;

	constexpr size_t kMaxPathBytes = 4096;

	/* recoveryPolicy is shared by v1 (stage 2a) and v2 configs. */
	s.recoveryPolicy = recovery_policy_from_str(obs_data_get_string(data, "recoveryPolicy"));

	/* fallbackType whitelist + fallbackName clamp apply to both schemas. */
	const char *ft = obs_data_get_string(data, "fallbackType");
	if (ft && (strcmp(ft, "image") == 0 || strcmp(ft, "pgm") == 0 || strcmp(ft, "prvw") == 0 ||
		   strcmp(ft, "scene") == 0 || strcmp(ft, "source") == 0)) {
		s.fallbackType = ft;
	} else {
		s.fallbackType.clear();
	}
	s.fallbackName = obs_data_get_string(data, "fallbackName");
	if (s.fallbackName.size() > kMaxPathBytes)
		s.fallbackName.resize(kMaxPathBytes);
	if (obs_data_has_user_value(data, "fallbackImageFitMode"))
		s.fallbackImageFitMode = image_fit_mode_from_str(obs_data_get_string(data, "fallbackImageFitMode"));

	if (obs_data_has_user_value(data, "manualReconnectCooldownMs"))
		s.manualReconnectCooldownMs = (int)obs_data_get_int(data, "manualReconnectCooldownMs");
	if (s.manualReconnectCooldownMs < 0)
		s.manualReconnectCooldownMs = 0;
	if (s.manualReconnectCooldownMs > 60000)
		s.manualReconnectCooldownMs = 60000;

	if (obs_data_has_user_value(data, "displayContent")) {
		/* v2 schema. */
		s.displayContent = lost_display_content_from_str(obs_data_get_string(data, "displayContent"));
		s.statusBand = lost_status_band_from_str(obs_data_get_string(data, "statusBand"));
	} else {
		/* Legacy (pre-v2) schema: read the old fields, then migrate up. */
		s.internalMissingBehavior =
			internal_missing_behavior_from_str(obs_data_get_string(data, "internalMissingBehavior"));
		s.externalLostBehavior =
			external_lost_behavior_from_str(obs_data_get_string(data, "externalLostBehavior"));
		s.placeholderImagePath = obs_data_get_string(data, "placeholderImagePath");
		s.signalLostImagePath = obs_data_get_string(data, "signalLostImagePath");
		if (s.placeholderImagePath.size() > kMaxPathBytes)
			s.placeholderImagePath.resize(kMaxPathBytes);
		if (s.signalLostImagePath.size() > kMaxPathBytes)
			s.signalLostImagePath.resize(kMaxPathBytes);
		if (obs_data_has_user_value(data, "placeholderImageFitMode"))
			s.placeholderImageFitMode =
				image_fit_mode_from_str(obs_data_get_string(data, "placeholderImageFitMode"));
		if (obs_data_has_user_value(data, "signalLostImageFitMode"))
			s.signalLostImageFitMode =
				image_fit_mode_from_str(obs_data_get_string(data, "signalLostImageFitMode"));
		migrate_lost_settings_v1_to_v2(s);
	}

	/* Always re-derive the legacy render fields from the canonical v2 fields
	 * so the unchanged render path sees consistent values. */
	derive_legacy_lost_fields(s);
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
