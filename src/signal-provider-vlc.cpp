/*
OBS Advanced Multiview - VLC signal provider (Phase 3 / M6.4)

Wraps OBS's built-in `vlc_source` so multiview cells can host media via
VLC's protocol stack (live network streams, playlists, formats
ffmpeg_source cannot open). Marked optional in plan.md M6.4: vlc_source
ships with OBS Studio only when the vlc-video plugin was built against
libVLC at packaging time, so availability is not guaranteed even on
modern OBS installs.

Architecture mirrors the FFmpeg provider:
  - Availability detection via obs_source_get_display_name("vlc_source").
  - create_private_source rebuilds the playlist obs_data_array_t from
    the user's persisted item list, then re-asserts default behavior /
    loop / network_caching / track values that the form may have
    dropped to keep persisted JSON compact.
  - probe_health consumes the canonical OBS media-state machine
    (vlc_source publishes it the same way ffmpeg_source does).
  - supports_media_restart=true, benefits_from_recreate=true (vlc_source
    responds to obs_source_media_restart, and re-opening the source
    re-resolves URLs / re-binds libVLC media list).
  - prefers_unbuffered_async returns true only when the playlist
    contains exclusively local files. Same rationale as FFmpeg: local
    decoders keep up so unbuffered avoids the 30 s timestamp-convergence
    stutter window after activate; live streams need buffered timing
    for jitter smoothing.

Scope of this milestone matches the M6.4 acceptance checklist:
  - source id `vlc_source`
  - min settings: playlist + loop + behavior + network_caching + track
  - provider unavailable -> tab gated, no crash
  - VLC has audio: rebuild_volmeters handles it via the generic
    external-cell path (no special-case in multiview-window-vu.cpp,
    unlike Spout which is hard-coded silent)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "signal-provider.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

constexpr const char *kVlcSourceId = "vlc_source";

/* Settings keys defined by upstream vlc-video plugin (see
 * obs-studio/plugins/vlc-video/vlc-video-source.c). Spellings pinned
 * verbatim so a typo cannot silently break persistence parity with
 * OBS's own VLC source dialog. */
constexpr const char *kKeyPlaylist = "playlist";
constexpr const char *kKeyLoop = "loop";
constexpr const char *kKeyShuffle = "shuffle";
constexpr const char *kKeyBehavior = "playback_behavior";
constexpr const char *kKeyNetworkCaching = "network_caching";
constexpr const char *kKeyTrack = "track";
constexpr const char *kKeySubtitleEnable = "subtitle_enable";
constexpr const char *kKeySubtitleTrack = "subtitle_track";

constexpr const char *kBehaviorStopRestart = "stop_restart";
constexpr const char *kBehaviorPauseUnpause = "pause_unpause";
constexpr const char *kBehaviorAlwaysPlay = "always_play";

/* Each playlist entry is an obs_data_t with a single "value" string
 * (vlc-video-source.c line 637 reads it via obs_data_get_string(item,
 * "value")). Local paths and URLs are both stored the same way; libVLC
 * disambiguates at parse time. */
constexpr const char *kPlaylistItemValueKey = "value";

/* Loose URL detection used to decide whether to treat the playlist as
 * "exclusively local" for unbuffered policy. We only need to recognize
 * "scheme://" style URLs; libVLC accepts a much wider syntax but if it
 * contains "://" we assume network. */
bool entry_is_local(const char *value)
{
	if (!value || !*value)
		return false;
	return std::strstr(value, "://") == nullptr;
}

class VlcProvider : public ISignalProvider {
public:
	SignalProviderType type() const override { return SignalProviderType::Vlc; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "VLC Media"; }

	bool is_available() const override
	{
		/* vlc-video plugin is conditionally built; on OBS installs
		 * without libVLC at packaging time the source id is simply
		 * never registered. */
		return obs_source_get_display_name(kVlcSourceId) != nullptr;
	}

	std::string unavailable_reason() const override
	{
		if (is_available())
			return std::string();
		return "OBS built-in VLC source (vlc_source) is not registered "
		       "(this OBS install was built without libVLC).";
	}

	OBSSource create_private_source(const std::string &desired_name, const SignalConfig &cfg) const override
	{
		if (!is_available()) {
			obs_log(LOG_WARNING, "[signal-provider/vlc] create skipped: vlc_source unavailable");
			return OBSSource();
		}

		obs_data_t *src_settings = cfg.providerSettings;

		/* Soft-fail when the playlist is empty so the runtime paints
		 * MISSING and the user can re-edit, mirroring FFmpeg's
		 * empty-URL handling. */
		if (!src_settings) {
			obs_log(LOG_WARNING, "[signal-provider/vlc] create skipped: no providerSettings");
			return OBSSource();
		}
		obs_data_array_t *playlist = obs_data_get_array(src_settings, kKeyPlaylist);
		const size_t playlist_count = playlist ? obs_data_array_count(playlist) : 0;
		if (playlist)
			obs_data_array_release(playlist);
		if (playlist_count == 0) {
			obs_log(LOG_WARNING, "[signal-provider/vlc] create skipped: empty playlist");
			return OBSSource();
		}

		/* Deep-copy via JSON round-trip so our default re-assertions
		 * don't leak back into the user's persisted SignalConfig.
		 * Same pattern as the FFmpeg / NDI / Spout providers. */
		obs_data_t *settings = obs_data_create();
		{
			const char *json = obs_data_get_json(src_settings);
			if (json && *json) {
				obs_data_t *copy = obs_data_create_from_json(json);
				if (copy) {
					obs_data_apply(settings, copy);
					obs_data_release(copy);
				}
			}
		}

		/* Re-assert vlcs_defaults() values when the form dropped a
		 * key for compactness. Matches obs-studio plugins/vlc-video
		 * vlc-video-source.c vlcs_defaults exactly. */
		if (!obs_data_has_user_value(settings, kKeyLoop))
			obs_data_set_bool(settings, kKeyLoop, true);
		if (!obs_data_has_user_value(settings, kKeyShuffle))
			obs_data_set_bool(settings, kKeyShuffle, false);
		if (!obs_data_has_user_value(settings, kKeyBehavior))
			obs_data_set_string(settings, kKeyBehavior, kBehaviorStopRestart);
		if (!obs_data_has_user_value(settings, kKeyNetworkCaching))
			obs_data_set_int(settings, kKeyNetworkCaching, 400);
		if (!obs_data_has_user_value(settings, kKeyTrack))
			obs_data_set_int(settings, kKeyTrack, 1);
		if (!obs_data_has_user_value(settings, kKeySubtitleEnable))
			obs_data_set_bool(settings, kKeySubtitleEnable, false);
		if (!obs_data_has_user_value(settings, kKeySubtitleTrack))
			obs_data_set_int(settings, kKeySubtitleTrack, 1);

		obs_source_t *raw = obs_source_create_private(kVlcSourceId, desired_name.c_str(), settings);
		obs_data_release(settings);
		if (!raw) {
			obs_log(LOG_WARNING, "[signal-provider/vlc] obs_source_create_private failed for '%s'",
				desired_name.c_str());
			return OBSSource();
		}

		obs_log(LOG_INFO, "[signal-provider/vlc] created private source '%s' playlist_items=%zu",
			desired_name.c_str(), playlist_count);

		OBSSource wrapper(raw);
		obs_source_release(raw);
		return wrapper;
	}

	/* vlc_source publishes the canonical OBS media-state machine just
	 * like ffmpeg_source, so the same state -> HealthCode mapping
	 * applies. The 5 s grace covers libVLC's parse + open latency on
	 * cold start (network playlists in particular can take a beat to
	 * resolve). */
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
			r.reason = "playlist ended";
			break;
		case OBS_MEDIA_STATE_ERROR:
			r.code = HealthCode::Error;
			r.reason = "vlc error";
			break;
		case OBS_MEDIA_STATE_PAUSED:
			/* User pressed Play/Pause via the cell context menu.
			 * Treat as a benign user-initiated state, not a Lost
			 * (which would attempt restart and reset the playlist
			 * position). The cell keeps the last decoded frame. */
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

	/* Same logic as FFmpeg: unbuffered only when every playlist entry
	 * is a local file. Mixed playlists (one network URL + locals) get
	 * the safer buffered path so the network entry doesn't stutter. */
	bool prefers_unbuffered_async(const SignalConfig &cfg) const override
	{
		obs_data_t *s = cfg.providerSettings;
		if (!s)
			return false;
		obs_data_array_t *playlist = obs_data_get_array(s, kKeyPlaylist);
		if (!playlist)
			return false;
		const size_t n = obs_data_array_count(playlist);
		bool all_local = n > 0;
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(playlist, i);
			const char *value = item ? obs_data_get_string(item, kPlaylistItemValueKey) : nullptr;
			if (!entry_is_local(value)) {
				all_local = false;
			}
			obs_data_release(item);
			if (!all_local)
				break;
		}
		obs_data_array_release(playlist);
		return all_local;
	}
};

static VlcProvider g_vlc_provider;

} /* anonymous namespace */

/* ========== Module entry points ========== */

void register_vlc_provider()
{
	SignalProviderRegistry::instance().register_provider(&g_vlc_provider);
}

void signal_provider_vlc_shutdown()
{
	/* VLC provider has no long-lived discovery probe (playlist entries
	 * are user-typed URLs / paths, not enumerated). Hook exists for
	 * registry symmetry with NDI / Spout. */
}
