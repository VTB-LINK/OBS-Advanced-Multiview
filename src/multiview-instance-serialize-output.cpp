/*
OBS Advanced Multiview - external output settings serialization (issue #11)

Split out of multiview-instance.cpp (issue #10 hardening): OutputResolutionMode /
OutputAudioMode enum mapping, the OBS advanced-output rescale readers, and the
OutputBackendSettings / InstanceOutputSettings (de)serialization + output
dimension resolution. Self-contained — its enum/rescale helpers are used only
here. Struct declarations live in multiview-instance.hpp.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-instance.hpp"
#include "multiview-output.hpp"
#include "config-limits.hpp"

#include <obs.h>
#include <obs-data.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

/* ---------- External output settings (issue #11) ---------- */

static const char *output_res_mode_to_str(OutputResolutionMode m)
{
	switch (m) {
	case OutputResolutionMode::ObsOutput:
		return "obsOutput";
	case OutputResolutionMode::ObsStreamRescale:
		return "obsStreamRescale";
	case OutputResolutionMode::ObsRecordRescale:
		return "obsRecordRescale";
	case OutputResolutionMode::Custom:
		return "custom";
	default:
		return "canvasBase";
	}
}

static OutputResolutionMode output_res_mode_from_str(const char *s)
{
	if (s && strcmp(s, "obsOutput") == 0)
		return OutputResolutionMode::ObsOutput;
	if (s && strcmp(s, "obsStreamRescale") == 0)
		return OutputResolutionMode::ObsStreamRescale;
	if (s && strcmp(s, "obsRecordRescale") == 0)
		return OutputResolutionMode::ObsRecordRescale;
	if (s && strcmp(s, "custom") == 0)
		return OutputResolutionMode::Custom;
	return OutputResolutionMode::CanvasBase;
}

static const char *output_audio_mode_to_str(OutputAudioMode m)
{
	switch (m) {
	case OutputAudioMode::ManualTrack:
		return "manualTrack";
	case OutputAudioMode::None:
		return "none";
	default:
		return "followStreaming";
	}
}

static OutputAudioMode output_audio_mode_from_str(const char *s)
{
	if (s && strcmp(s, "manualTrack") == 0)
		return OutputAudioMode::ManualTrack;
	if (s && strcmp(s, "none") == 0)
		return OutputAudioMode::None;
	return OutputAudioMode::FollowStreaming;
}

/* Shared reader for an OBS advanced-mode encoder "Rescale Output" setting.
 * filterKey/resKey are the [AdvOut] config keys: streaming uses
 * RescaleFilter/RescaleRes, recording uses RecRescaleFilter/RecRescaleRes. */
static bool obs_advout_rescale_dimensions(const char *filterKey, const char *resKey, uint32_t &w, uint32_t &h)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return false;

	/* Per-encoder rescale only exists in Advanced output mode. */
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (!mode || strcmp(mode, "Advanced") != 0)
		return false;

	/* filter == OBS_SCALE_DISABLE (0) means the "Rescale Output" checkbox
	 * is off. */
	const int filter = (int)config_get_int(cfg, "AdvOut", filterKey);
	if (filter == OBS_SCALE_DISABLE)
		return false;

	const char *res = config_get_string(cfg, "AdvOut", resKey);
	if (!res || !*res)
		return false;

	unsigned int pw = 0, ph = 0;
	if (sscanf(res, "%ux%u", &pw, &ph) != 2 || pw == 0 || ph == 0)
		return false;

	w = pw;
	h = ph;
	return true;
}

bool obs_stream_rescale_dimensions(uint32_t &w, uint32_t &h)
{
	return obs_advout_rescale_dimensions("RescaleFilter", "RescaleRes", w, h);
}

bool obs_record_rescale_dimensions(uint32_t &w, uint32_t &h)
{
	return obs_advout_rescale_dimensions("RecRescaleFilter", "RecRescaleRes", w, h);
}

obs_data_t *OutputBackendSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_string(data, "resMode", output_res_mode_to_str(resMode));
	obs_data_set_int(data, "customWidth", customWidth);
	obs_data_set_int(data, "customHeight", customHeight);
	obs_data_set_int(data, "fpsDivisor", fpsDivisor);
	obs_data_set_string(data, "audioMode", output_audio_mode_to_str(audioMode));
	obs_data_set_int(data, "audioTrackIndex", audioTrackIndex);
	return data;
}

OutputBackendSettings OutputBackendSettings::from_obs_data(obs_data_t *data)
{
	OutputBackendSettings s;
	if (!data)
		return s;
	if (obs_data_has_user_value(data, "enabled"))
		s.enabled = obs_data_get_bool(data, "enabled");
	if (obs_data_has_user_value(data, "resMode"))
		s.resMode = output_res_mode_from_str(obs_data_get_string(data, "resMode"));
	if (obs_data_has_user_value(data, "customWidth"))
		s.customWidth = (uint32_t)obs_data_get_int(data, "customWidth");
	if (obs_data_has_user_value(data, "customHeight"))
		s.customHeight = (uint32_t)obs_data_get_int(data, "customHeight");
	/* Defensive clamp against hand-edited / corrupt configs: a huge or zero
	 * custom resolution feeds straight into gs_texrender_create and the GPU
	 * shared texture (shared kMinDim/kMaxDim bounds in config-limits.hpp). */
	if (s.customWidth < amv::limits::kMinDim)
		s.customWidth = amv::limits::kMinDim;
	else if (s.customWidth > amv::limits::kMaxDim)
		s.customWidth = amv::limits::kMaxDim;
	if (s.customHeight < amv::limits::kMinDim)
		s.customHeight = amv::limits::kMinDim;
	else if (s.customHeight > amv::limits::kMaxDim)
		s.customHeight = amv::limits::kMaxDim;
	if (obs_data_has_user_value(data, "fpsDivisor"))
		s.fpsDivisor = (int)obs_data_get_int(data, "fpsDivisor");
	/* Only full (1) and half (2) are legal divisors. */
	if (s.fpsDivisor != 1 && s.fpsDivisor != 2)
		s.fpsDivisor = 1;
	if (obs_data_has_user_value(data, "audioMode"))
		s.audioMode = output_audio_mode_from_str(obs_data_get_string(data, "audioMode"));
	if (obs_data_has_user_value(data, "audioTrackIndex"))
		s.audioTrackIndex = (int)obs_data_get_int(data, "audioTrackIndex");
	/* OBS mixer tracks are 1..6. */
	if (s.audioTrackIndex < amv::limits::kMinAudioTrack)
		s.audioTrackIndex = amv::limits::kMinAudioTrack;
	else if (s.audioTrackIndex > amv::limits::kMaxAudioTrack)
		s.audioTrackIndex = amv::limits::kMaxAudioTrack;
	return s;
}

obs_data_t *DeckLinkBackendSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "deviceHash", deviceHash.c_str());
	obs_data_set_int(data, "modeId", modeId);
	obs_data_set_int(data, "keyer", keyer);
	obs_data_set_bool(data, "forceSdr", forceSdr);
	return data;
}

DeckLinkBackendSettings DeckLinkBackendSettings::from_obs_data(obs_data_t *data)
{
	DeckLinkBackendSettings s;
	if (!data)
		return s;
	if (obs_data_has_user_value(data, "deviceHash"))
		s.deviceHash = obs_data_get_string(data, "deviceHash");
	if (obs_data_has_user_value(data, "modeId"))
		s.modeId = obs_data_get_int(data, "modeId");
	/* H2: a negative mode_id is garbage; clamp to 0 (= "unset"), which the
	 * backend treats as a hard refusal-to-create rather than feeding it to
	 * obs_output_create (null DeckLinkDeviceMode deref). */
	if (s.modeId < 0)
		s.modeId = 0;
	if (obs_data_has_user_value(data, "keyer"))
		s.keyer = (int)obs_data_get_int(data, "keyer");
	/* keyer is 0 (Disabled) / 1 (External) / 2 (Internal). */
	if (s.keyer < 0 || s.keyer > 2)
		s.keyer = 0;
	if (obs_data_has_user_value(data, "forceSdr"))
		s.forceSdr = obs_data_get_bool(data, "forceSdr");
	return s;
}

obs_data_t *AjaBackendSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "cardID", cardID.c_str());
	obs_data_set_int(data, "ioSelect", ioSelect);
	obs_data_set_int(data, "videoFormat", videoFormat);
	obs_data_set_int(data, "pixelFormat", pixelFormat);
	obs_data_set_int(data, "sdiTransport", sdiTransport);
	obs_data_set_int(data, "sdi4kTransport", sdi4kTransport);
	return data;
}

AjaBackendSettings AjaBackendSettings::from_obs_data(obs_data_t *data)
{
	AjaBackendSettings s;
	if (!data)
		return s;
	if (obs_data_has_user_value(data, "cardID"))
		s.cardID = obs_data_get_string(data, "cardID");
	if (obs_data_has_user_value(data, "ioSelect"))
		s.ioSelect = obs_data_get_int(data, "ioSelect");
	if (obs_data_has_user_value(data, "videoFormat"))
		s.videoFormat = obs_data_get_int(data, "videoFormat");
	if (obs_data_has_user_value(data, "pixelFormat"))
		s.pixelFormat = obs_data_get_int(data, "pixelFormat");
	if (obs_data_has_user_value(data, "sdiTransport"))
		s.sdiTransport = obs_data_get_int(data, "sdiTransport");
	if (obs_data_has_user_value(data, "sdi4kTransport"))
		s.sdi4kTransport = obs_data_get_int(data, "sdi4kTransport");
	/* A hand-edited / corrupt config could carry a negative garbage integer.
	 * AJA's aja_output_create safely returns nullptr on any invalid value (unlike
	 * DeckLink's crash), so this clamp is only about not stashing nonsense: pin a
	 * negative back to the field's "unset" sentinel / default. The authoritative
	 * validity check (membership in the enumerated lists) happens in the backend
	 * create task, not here. */
	if (s.ioSelect < 0)
		s.ioSelect = kAjaIoSelectionInvalid;
	if (s.videoFormat < 0)
		s.videoFormat = kAjaVideoFormatUnknown;
	if (s.pixelFormat < 0)
		s.pixelFormat = 0;
	if (s.sdiTransport < 0)
		s.sdiTransport = 0;
	if (s.sdi4kTransport < 0)
		s.sdi4kTransport = 1;
	return s;
}

const OutputBackendSettings &InstanceOutputSettings::at(OutputBackendKind kind) const
{
	auto it = backends.find(kind);
	if (it != backends.end())
		return it->second;
	/* No entry for this kind (default-constructed settings, or a kind compiled
	 * out of this build): a shared, program-lifetime disabled default. */
	static const OutputBackendSettings kDefault;
	return kDefault;
}

obs_data_t *InstanceOutputSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	/* Each backend's common settings under its registry id ("spout"/"ndi"/
	 * "decklink"): iterate the registry so a newly added kind is serialized with
	 * zero extra code (and a compiled-out kind is simply never written). */
	for (const auto &desc : output_backend_registry()) {
		obs_data_t *bo = at(desc.kind).to_obs_data();
		obs_data_set_obj(data, desc.id, bo);
		obs_data_release(bo);
	}
	/* DeckLink hardware settings, nested separately so the shared per-backend
	 * object stays hardware-agnostic. Always written (the member is always
	 * present) — harmless on a build without the DeckLink backend. */
	obs_data_t *dh = decklink.to_obs_data();
	obs_data_set_obj(data, "decklinkHw", dh);
	obs_data_release(dh);
	/* AJA hardware settings, nested separately (same rationale as decklinkHw).
	 * Always written; harmless on a build without the AJA backend. */
	obs_data_t *ah = aja.to_obs_data();
	obs_data_set_obj(data, "ajaHw", ah);
	obs_data_release(ah);
	/* Re-emit any sub-object for a kind not in this build's registry, verbatim,
	 * so a narrower build never drops a wider build's settings (see the member's
	 * declaration). These keys never collide with the registry ids or the
	 * "decklinkHw"/"ajaHw" keys above (from_obs_data only captured keys that
	 * matched none). */
	for (const auto &kv : unknownBackends) {
		obs_data_t *sub = obs_data_create_from_json(kv.second.c_str());
		if (sub) {
			obs_data_set_obj(data, kv.first.c_str(), sub);
			obs_data_release(sub);
		}
	}
	return data;
}

InstanceOutputSettings InstanceOutputSettings::from_obs_data(obs_data_t *data)
{
	InstanceOutputSettings s;
	if (!data)
		return s;
	/* One common-settings entry per registered backend kind, read from its
	 * registry-id sub-object (absent sub-object -> defaults). */
	for (const auto &desc : output_backend_registry()) {
		obs_data_t *bo = obs_data_get_obj(data, desc.id);
		s.backends[desc.kind] = OutputBackendSettings::from_obs_data(bo);
		obs_data_release(bo);
	}
	obs_data_t *dh = obs_data_get_obj(data, "decklinkHw");
	s.decklink = DeckLinkBackendSettings::from_obs_data(dh);
	obs_data_release(dh);
	obs_data_t *ah = obs_data_get_obj(data, "ajaHw");
	s.aja = AjaBackendSettings::from_obs_data(ah);
	obs_data_release(ah);
	/* M1: a DeckLink config (a device is selected) locks the compose size to the
	 * mode raster carried in the DeckLink backend's customWidth/customHeight with
	 * resMode Custom. Force Custom so a hand-edited/corrupt resMode can't make the
	 * manager compose at the canvas/output size while the backend opens its
	 * video_t at the SDI raster — which would drop every frame on the dimension
	 * guard. Applied here (not in OutputBackendSettings::from_obs_data) because it
	 * couples the DeckLink hardware settings to the DeckLink common settings, and
	 * only this scope sees both. No-op when DeckLink is compiled out (no entry) or
	 * no device is selected. */
	if (!s.decklink.deviceHash.empty()) {
		auto it = s.backends.find(OutputBackendKind::Decklink);
		if (it != s.backends.end())
			it->second.resMode = OutputResolutionMode::Custom;
	}
	/* Retain, verbatim, any persisted backend sub-object whose kind this build's
	 * registry doesn't know (e.g. "spout" on a macOS build) so to_obs_data can
	 * write it back unchanged instead of silently dropping it. The consumed keys
	 * are the registry ids (read above) plus "decklinkHw"/"ajaHw" (always read);
	 * every other object-typed top-level key is an out-of-build backend to
	 * preserve. */
	std::set<std::string> consumed;
	for (const auto &desc : output_backend_registry())
		consumed.insert(desc.id);
	consumed.insert("decklinkHw");
	consumed.insert("ajaHw");
	for (obs_data_item_t *item = obs_data_first(data); item; obs_data_item_next(&item)) {
		const char *key = obs_data_item_get_name(item);
		if (!key || obs_data_item_gettype(item) != OBS_DATA_OBJECT || consumed.count(key))
			continue;
		obs_data_t *sub = obs_data_item_get_obj(item);
		if (sub) {
			s.unknownBackends[key] = obs_data_get_json(sub);
			obs_data_release(sub);
		}
	}
	return s;
}

std::pair<uint32_t, uint32_t> resolve_output_dimensions(const OutputBackendSettings &s)
{
	uint32_t w = 0, h = 0;

	if (s.resMode == OutputResolutionMode::Custom) {
		w = s.customWidth;
		h = s.customHeight;
	} else {
		if (s.resMode == OutputResolutionMode::ObsStreamRescale) {
			uint32_t rw = 0, rh = 0;
			if (obs_stream_rescale_dimensions(rw, rh)) {
				w = rw;
				h = rh;
			}
			/* Rescale turned off in OBS since this was picked — fall back to
			 * the global OBS output (scaled) resolution below. */
		} else if (s.resMode == OutputResolutionMode::ObsRecordRescale) {
			uint32_t rw = 0, rh = 0;
			if (obs_record_rescale_dimensions(rw, rh)) {
				w = rw;
				h = rh;
			}
			/* Fall back to the global OBS output resolution below. */
		}

		if (w == 0 || h == 0) {
			struct obs_video_info ovi;
			if (!obs_get_video_info(&ovi))
				return {0, 0}; /* No video info: signal "skip" (reconcile drops w/h==0). */

			if (s.resMode == OutputResolutionMode::ObsOutput ||
			    s.resMode == OutputResolutionMode::ObsStreamRescale ||
			    s.resMode == OutputResolutionMode::ObsRecordRescale) {
				w = ovi.output_width;
				h = ovi.output_height;
			} else {
				w = ovi.base_width; /* CanvasBase */
				h = ovi.base_height;
			}
		}
	}

	/* S3 hardening: clamp every RESOLVED size to [kMinDim,kMaxDim]. Custom is
	 * already clamped at deserialization, but the OBS rescale exports
	 * (RescaleRes=NNNNxNNNN from the profile ini) and the canvas/output dims
	 * bypassed it and flow straight into gs_texrender_create. The {0,0} "no
	 * video info" bail above returns earlier and is intentionally left
	 * unclamped so reconcile still skips the backend. */
	if (w < amv::limits::kMinDim)
		w = amv::limits::kMinDim;
	else if (w > amv::limits::kMaxDim)
		w = amv::limits::kMaxDim;
	if (h < amv::limits::kMinDim)
		h = amv::limits::kMinDim;
	else if (h > amv::limits::kMaxDim)
		h = amv::limits::kMaxDim;
	return {w, h};
}
