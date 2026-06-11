/*
OBS Advanced Multiview - FFmpeg signal provider (Phase 3 / M6.1)

Wraps OBS's built-in `ffmpeg_source` so multiview cells can host network
media (RTMP / HLS / FLV / SRT / file URLs / any FFmpeg-accepted URL)
without the user manually adding the source to a scene. The source is
created as a private OBS source (via `obs_source_create_private`) so it
stays out of `obs_enum_sources` / the OBS Sources dock and only this
plugin can render or release it.

Scope of this milestone:
  - Availability detection (is the host plugin present?).
  - One-shot create-or-update from a SignalConfig.
  - Release via OBSSource RAII; no SDK calls, no extra threads.

Health, reconnect, fallback and SIGNAL LOST overlays land in step 10.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "signal-provider.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <cstring>

namespace {

constexpr const char *kFfmpegSourceId = "ffmpeg_source";

/* Settings keys defined by the upstream `ffmpeg_source` plugin (see
 * obs-studio/plugins/obs-ffmpeg/obs-ffmpeg-source.c). We pin the exact
 * spellings here so a typo cannot silently render a non-functional URL
 * source. M6.1 only sets the live-URL essentials; advanced playback
 * settings (looping, speed, hardware decode, color range) are left at
 * source defaults so OBS's own behavior is the reference. */
constexpr const char *kKeyInput = "input";
constexpr const char *kKeyIsLocalFile = "is_local_file";
constexpr const char *kKeyLocalFile = "local_file";
constexpr const char *kKeyReconnectDelaySec = "reconnect_delay_sec";
constexpr const char *kKeyBufferingMb = "buffering_mb";
constexpr const char *kKeyRestartOnActivate = "restart_on_activate";
constexpr const char *kKeyCloseWhenInactive = "close_when_inactive";
constexpr const char *kKeyClearOnMediaEnd = "clear_on_media_end";
constexpr const char *kKeyLinearAlpha = "linear_alpha";

class FfmpegProvider : public ISignalProvider {
public:
	SignalProviderType type() const override { return SignalProviderType::Ffmpeg; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "FFmpeg Media"; }

	bool is_available() const override
	{
		/* Cheapest path: ask OBS whether the source type is registered.
		 * obs_source_get_display_name returns nullptr when no plugin
		 * registered the id, which is the same signal SourcePicker
		 * uses for placeholder vs real tab decisions. ffmpeg_source is
		 * shipped with OBS Studio so this should be true on every
		 * supported install; we still gate on it so the picker shows
		 * a clear reason if the user removed the obs-ffmpeg plugin. */
		return obs_source_get_display_name(kFfmpegSourceId) != nullptr;
	}

	std::string unavailable_reason() const override
	{
		if (is_available())
			return std::string();
		return "OBS built-in FFmpeg media source (ffmpeg_source) is not registered.";
	}

	OBSSource create_private_source(const std::string &desired_name, const SignalConfig &cfg) const override
	{
		if (!is_available()) {
			obs_log(LOG_WARNING, "[signal-provider/ffmpeg] create skipped: ffmpeg_source unavailable");
			return OBSSource();
		}

		obs_data_t *src_settings = cfg.providerSettings;

		/* Phase 3 / M6.1+ task 9.1.B: support both network and local-file
		 * playback. is_local_file picks which input key matters. We
		 * still treat "no URL and no path" as a soft failure so the
		 * runtime can paint MISSING and let the user re-edit. */
		const bool is_local_file = src_settings ? obs_data_get_bool(src_settings, kKeyIsLocalFile) : false;
		const char *input_url = src_settings ? obs_data_get_string(src_settings, kKeyInput) : nullptr;
		const char *local_path = src_settings ? obs_data_get_string(src_settings, kKeyLocalFile) : nullptr;
		const bool has_input = is_local_file ? (local_path && *local_path) : (input_url && *input_url);
		if (!has_input) {
			obs_log(LOG_WARNING, "[signal-provider/ffmpeg] create skipped: empty %s",
				is_local_file ? "local_file path" : "input URL");
			return OBSSource();
		}

		/* Build OBS source settings starting from the user's persisted
		 * providerSettings (so all advanced keys travel through), then
		 * re-assert the M6.1 defaults for any key the form doesn't fill,
		 * then HARD-LOCK the two activation-safety keys regardless of
		 * what's in providerSettings. The locks are explained in
		 * provider-settings-forms.hpp.
		 *
		 * obs_data_create_from_json + obs_data_get_json gives a deep
		 * copy without dragging in obs_data_apply (which would write
		 * defaults back into the user's persisted object). */
		obs_data_t *settings = obs_data_create();
		if (src_settings) {
			const char *json = obs_data_get_json(src_settings);
			if (json && *json) {
				obs_data_t *copy = obs_data_create_from_json(json);
				if (copy) {
					obs_data_apply(settings, copy);
					obs_data_release(copy);
				}
			}
		}

		/* Defaults that the form may have omitted (set_or_default_*
		 * deliberately drops default values to keep persisted JSON
		 * compact). */
		if (!obs_data_has_user_value(settings, kKeyReconnectDelaySec))
			obs_data_set_int(settings, kKeyReconnectDelaySec, 10);
		if (!obs_data_has_user_value(settings, kKeyBufferingMb))
			obs_data_set_int(settings, kKeyBufferingMb, 2);
		if (!obs_data_has_user_value(settings, kKeyClearOnMediaEnd))
			obs_data_set_bool(settings, kKeyClearOnMediaEnd, true);

		/* HARD LOCKS \u2014 these keys are never user-editable. They pair
		 * with our inc_active activation model: if either flips,
		 * playback never starts or stops on every Multiview cell
		 * visibility change. We overwrite even if the form somehow
		 * persisted a different value. */
		obs_data_set_bool(settings, kKeyRestartOnActivate, true);
		obs_data_set_bool(settings, kKeyCloseWhenInactive, false);

		obs_source_t *raw = obs_source_create_private(kFfmpegSourceId, desired_name.c_str(), settings);
		obs_data_release(settings);
		if (!raw) {
			obs_log(LOG_WARNING, "[signal-provider/ffmpeg] obs_source_create_private failed for '%s'",
				desired_name.c_str());
			return OBSSource();
		}

		obs_log(LOG_INFO, "[signal-provider/ffmpeg] created private source '%s' %s='%s'", desired_name.c_str(),
			is_local_file ? "local_file" : "input", is_local_file ? local_path : input_url);

		OBSSource wrapper(raw);
		obs_source_release(raw);
		return wrapper;
	}
};

static FfmpegProvider g_ffmpeg_provider;

} /* anonymous namespace */

/* ========== Module entry point ========== */

/* Called from signal_provider_registry_init via a TU-local hook in
 * signal-provider.cpp. Keeping the registration in this file keeps the
 * FFmpeg specifics out of the registry skeleton. */
void register_ffmpeg_provider()
{
	SignalProviderRegistry::instance().register_provider(&g_ffmpeg_provider);
}
