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
#include "amv-logging.hpp"

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
			obs_log(LOG_WARNING,
				"[signal-provider/ffmpeg] create skipped for '%s': ffmpeg_source unavailable",
				desired_name.c_str());
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
			obs_log(LOG_WARNING, "[signal-provider/ffmpeg] create skipped for '%s': empty %s",
				desired_name.c_str(), is_local_file ? "local_file path" : "input URL");
			return OBSSource();
		}

		/* Build OBS source settings starting from the user's persisted
		 * providerSettings (so all advanced keys travel through), then
		 * re-assert the M6.1 defaults for any key the form doesn't fill,
		 * then HARD-LOCK the two activation-safety keys regardless of
		 * what's in providerSettings. The locks are explained in
		 * provider-settings-forms.hpp. */
		obs_data_t *settings = ISignalProvider::deep_copy_provider_settings(src_settings);

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
		/* Phase 3 / M6.1+ post-9.1.B fix: ffmpeg_source's get_defaults
		 * makes is_local_file default to TRUE. Always re-assert the
		 * mode here too, so even if the persisted providerSettings
		 * lost the key (older config, manual JSON edit), the runtime
		 * value matches our intent. */
		obs_data_set_bool(settings, kKeyIsLocalFile,
				  is_local_file); /* Phase 3 / M6.1+ post-9.1.B fix: keys that only make sense
		 * for local-file playback must be force-cleared on network
		 * streams. The form preserves checkbox state across mode
		 * switches, so a user who first picked a local file with
		 * looping enabled and then switched to URL mode would otherwise
		 * persist looping=true into the network-stream providerSettings.
		 *
		 * For HLS / RTMP / SRT streams, looping=true confuses
		 * ffmpeg_source's seek-on-media-end logic on segment boundaries
		 * (visible symptom: stream never plays or freezes one segment in).
		 * seekable=true and speed_percent != 100 are similarly
		 * meaningless for live network streams — lock them off too. */
		if (!is_local_file) {
			obs_data_set_bool(settings, "looping", false);
			obs_data_set_bool(settings, "seekable", false);
			obs_data_set_int(settings, "speed_percent", 100);
		}
		obs_source_t *raw = obs_source_create_private(kFfmpegSourceId, desired_name.c_str(), settings);
		obs_data_release(settings);
		if (!raw) {
			obs_log(LOG_WARNING, "[signal-provider/ffmpeg] obs_source_create_private failed for '%s'",
				desired_name.c_str());
			return OBSSource();
		}

		/* Display-pipeline buffering policy is decided in
		 * prefers_unbuffered_async() and applied uniformly by the
		 * multiview runtime after create. See signal-provider.hpp
		 * for why this lives at the provider interface level. */

		amv_log_detailed(LOG_INFO, "[signal-provider/ffmpeg] created private source '%s' %s='%s'",
				 desired_name.c_str(), is_local_file ? "local_file" : "input",
				 is_local_file ? local_path : input_url);

		OBSSource wrapper(raw);
		obs_source_release(raw);
		return wrapper;
	}

	/* Phase 3 / M6 step 10: probe ffmpeg_source health.
	 *
	 * ffmpeg_source publishes the canonical OBS media-state machine via
	 * obs_source_media_get_state, so we can map directly:
	 *
	 *   PLAYING + valid dimensions  -> Active
	 *   PLAYING but zero dimensions -> Opening (codec still figuring it out)
	 *   OPENING / BUFFERING         -> Opening
	 *   ENDED                       -> Lost (network stream finished, or
	 *                                       non-looping local file done)
	 *   ERROR                       -> Error (URL unreachable, codec
	 *                                       incompatible, etc.)
	 *   STOPPED / PAUSED / NONE     -> Opening while young, then Lost
	 *
	 * Young-source grace is 5 s: ffmpeg_source's worker thread can take
	 * a few hundred ms to open an HLS playlist before media_state flips
	 * to PLAYING. Without the grace the supervisor would briefly mark
	 * the cell Lost during normal startup. */
	HealthReport probe_health(obs_source_t *src, uint64_t age_ns) const override
	{
		HealthReport r;
		if (!src)
			return r;
		r.width = obs_source_get_width(src);
		r.height = obs_source_get_height(src);

		const obs_media_state state = obs_source_media_get_state(src);
		switch (state) {
		case OBS_MEDIA_STATE_PLAYING:
			if (r.width > 0 && r.height > 0) {
				r.code = HealthCode::Active;
			} else {
				r.code = HealthCode::Opening;
				r.reason = "playing but no frames";
			}
			break;
		case OBS_MEDIA_STATE_OPENING:
			r.code = HealthCode::Opening;
			r.reason = "opening";
			break;
		case OBS_MEDIA_STATE_BUFFERING:
			r.code = HealthCode::Opening;
			r.reason = "buffering";
			break;
		case OBS_MEDIA_STATE_ENDED:
			r.code = HealthCode::Lost;
			r.reason = "media ended";
			break;
		case OBS_MEDIA_STATE_ERROR:
			r.code = HealthCode::Error;
			r.reason = "ffmpeg error";
			break;
		case OBS_MEDIA_STATE_PAUSED:
			/* User pressed Play/Pause via the cell context menu (or
			 * obs_source_media_play_pause from anywhere else). This
			 * is a user-initiated state, not a failure — don't
			 * escalate to Lost or kick a restart. The cell keeps the
			 * last decoded frame visible. */
			r.code = HealthCode::Paused;
			r.reason = "paused";
			break;
		case OBS_MEDIA_STATE_STOPPED:
		case OBS_MEDIA_STATE_NONE:
		default:
			if (age_ns < 5ULL * 1000 * 1000 * 1000)
				r.code = HealthCode::Opening;
			else
				r.code = HealthCode::Lost;
			break;
		}
		return r;
	}

	bool supports_media_restart() const override { return true; }
	bool benefits_from_recreate() const override { return true; }

	/* Local files: yes -- decoder keeps up, no jitter, OBS's buffered
	 * timing math just produces a 30s convergence stutter window after
	 * activate. Network streams: NO -- frames arrive unevenly, buffered
	 * timing is what makes playback smooth. See signal-provider.hpp
	 * for the full rationale. */
	bool prefers_unbuffered_async(const SignalConfig &cfg) const override
	{
		obs_data_t *src = cfg.providerSettings;
		return src && obs_data_get_bool(src, kKeyIsLocalFile);
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
