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

#include <obs.h>
#include <obs-data.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <cstdio>
#include <cstring>

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
	default:
		return "followStreaming";
	}
}

static OutputAudioMode output_audio_mode_from_str(const char *s)
{
	if (s && strcmp(s, "manualTrack") == 0)
		return OutputAudioMode::ManualTrack;
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
	 * shared texture. Match the dialog spinbox lower bound (16) and cap at
	 * 16384 (>8K, well within texture-size limits). */
	constexpr uint32_t kMinDim = 16, kMaxDim = 16384;
	if (s.customWidth < kMinDim)
		s.customWidth = kMinDim;
	else if (s.customWidth > kMaxDim)
		s.customWidth = kMaxDim;
	if (s.customHeight < kMinDim)
		s.customHeight = kMinDim;
	else if (s.customHeight > kMaxDim)
		s.customHeight = kMaxDim;
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
	if (s.audioTrackIndex < 1)
		s.audioTrackIndex = 1;
	else if (s.audioTrackIndex > 6)
		s.audioTrackIndex = 6;
	return s;
}

obs_data_t *InstanceOutputSettings::to_obs_data() const
{
	obs_data_t *data = obs_data_create();
	obs_data_t *sp = spout.to_obs_data();
	obs_data_set_obj(data, "spout", sp);
	obs_data_release(sp);
	obs_data_t *nd = ndi.to_obs_data();
	obs_data_set_obj(data, "ndi", nd);
	obs_data_release(nd);
	return data;
}

InstanceOutputSettings InstanceOutputSettings::from_obs_data(obs_data_t *data)
{
	InstanceOutputSettings s;
	if (!data)
		return s;
	obs_data_t *sp = obs_data_get_obj(data, "spout");
	s.spout = OutputBackendSettings::from_obs_data(sp);
	obs_data_release(sp);
	obs_data_t *nd = obs_data_get_obj(data, "ndi");
	s.ndi = OutputBackendSettings::from_obs_data(nd);
	obs_data_release(nd);
	return s;
}

std::pair<uint32_t, uint32_t> resolve_output_dimensions(const OutputBackendSettings &s)
{
	if (s.resMode == OutputResolutionMode::Custom)
		return {s.customWidth, s.customHeight};

	if (s.resMode == OutputResolutionMode::ObsStreamRescale) {
		uint32_t w = 0, h = 0;
		if (obs_stream_rescale_dimensions(w, h))
			return {w, h};
		/* Rescale turned off in OBS since this was picked — fall back to
		 * the global OBS output (scaled) resolution below. */
	} else if (s.resMode == OutputResolutionMode::ObsRecordRescale) {
		uint32_t w = 0, h = 0;
		if (obs_record_rescale_dimensions(w, h))
			return {w, h};
		/* Fall back to the global OBS output resolution below. */
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return {0, 0};

	if (s.resMode == OutputResolutionMode::ObsOutput || s.resMode == OutputResolutionMode::ObsStreamRescale ||
	    s.resMode == OutputResolutionMode::ObsRecordRescale)
		return {ovi.output_width, ovi.output_height};

	return {ovi.base_width, ovi.base_height}; /* CanvasBase */
}
