/*
OBS Advanced Multiview - NDI output backend (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#ifdef AMV_ENABLE_NDI_OUTPUT

#include "multiview-output-ndi.hpp"
#include "amv-frontend-cache.hpp"
#include "multiview-ndi-runtime.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <media-io/audio-io.h>

/* <cstddef> for NULL used by the NDI headers' default args (clang/gcc). */
#include <cstddef>

#include <Processing.NDI.Lib.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

class NdiOutputBackend : public IMultiviewOutputBackend {
public:
	~NdiOutputBackend() override { stop(); }

	const char *kind() const override { return "ndi"; }

	void submit_frame(const std::string &name, gs_texture_t *tex, uint32_t w, uint32_t h, int fpsDivisor) override
	{
		if (!tex || w == 0 || h == 0)
			return;

		/* Acquire the NDI runtime on first use; stay dormant if it can't
		 * be loaded (NdiRuntime::acquire logs the reason once). */
		if (!runtime_) {
			runtime_ = NdiRuntime::acquire();
			if (!runtime_)
				return;
		}

		if (!sender_ || name != current_name_) {
			recreate_sender(name);
			if (!sender_)
				return;
		}

		if (!ensure_stage(w, h))
			return;

		/* GPU -> CPU readback. gs_stage_texture queues the copy; the map
		 * blocks until it lands (a ~1-frame stall — double-buffering is a
		 * noted hardening follow-up). */
		gs_stage_texture(stage_, tex);

		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(stage_, &data, &linesize)) {
			if (!warned_map_failed_) {
				obs_log(LOG_WARNING, "[multiview-output/ndi] gs_stagesurface_map failed");
				warned_map_failed_ = true;
			}
			return;
		}

		NDIlib_video_frame_v2_t frame = {};
		frame.xres = (int)w;
		frame.yres = (int)h;
		frame.FourCC = NDIlib_FourCC_video_type_BGRA;

		/* OBS has one global frame rate; output rescale doesn't change it.
		 * The manager halves our actual cadence at fpsDivisor==2 by skipping
		 * compose passes, so declare the true sent rate (OBS fps / divisor) —
		 * otherwise NDI receivers (e.g. Studio Monitor) report the nominal
		 * full rate while frames arrive at half. */
		const int divisor = (fpsDivisor >= 1) ? fpsDivisor : 1;
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) && ovi.fps_den != 0) {
			frame.frame_rate_N = (int)ovi.fps_num;
			frame.frame_rate_D = (int)ovi.fps_den * divisor;
		} else {
			frame.frame_rate_N = 30;
			frame.frame_rate_D = divisor;
		}

		frame.frame_format_type = NDIlib_frame_format_type_progressive;
		frame.timecode = NDIlib_send_timecode_synthesize;
		frame.p_data = data;
		frame.line_stride_in_bytes = (int)linesize;

		/* Video sends on the graphics thread; sender_ is only ever mutated
		 * here (same thread), and NDI permits concurrent video/audio sends,
		 * so no lock is needed on this hot path. */
		runtime_->lib()->send_send_video_v2(sender_, &frame);

		gs_stagesurface_unmap(stage_);
		active_ = true;
		warned_map_failed_ = false;
	}

	void configure_audio(const OutputBackendSettings &cfg) override
	{
		const bool changed = cfg.audioMode != want_audio_mode_ || cfg.audioTrackIndex != want_audio_track_;
		want_audio_mode_ = cfg.audioMode;
		want_audio_track_ = cfg.audioTrackIndex;

		/* Reconnect immediately on a settings change or first connect; for
		 * FollowStreaming, also re-poll the streaming track periodically so we
		 * track the user re-assigning it mid-session (the per-frame frontend
		 * call is throttled; Manual needs no frontend call at all). */
		bool resolve = changed || !audio_connected_;
		if (cfg.audioMode == OutputAudioMode::FollowStreaming && (audio_resolve_counter_++ % 30) == 0)
			resolve = true;
		if (!resolve)
			return;

		const size_t desired = resolve_mix_index();
		if (!audio_connected_ || desired != connected_mix_) {
			disconnect_audio();
			connect_audio(desired);
		}
	}

	void stop() override
	{
		/* Stop audio first so no capture callback fires past sender teardown,
		 * then destroy the sender BEFORE releasing the runtime handle (the last
		 * handle release calls NDIlib_destroy, which requires all senders
		 * gone). */
		disconnect_audio();

		{
			std::lock_guard<std::mutex> lock(sender_mutex_);
			if (sender_ && runtime_) {
				runtime_->lib()->send_destroy(sender_);
				obs_log(LOG_INFO, "[multiview-output/ndi] sender released ('%s')",
					current_name_.c_str());
			}
			sender_ = nullptr;
			/* Drop the runtime ref under the lock too: the audio callback reads
			 * runtime_ while holding sender_mutex_, so releasing it here avoids
			 * a race (and keeps NDIlib_destroy after the sender is gone). */
			runtime_.reset();
		}

		if (stage_) {
			gs_stagesurface_destroy(stage_);
			stage_ = nullptr;
			stage_w_ = stage_h_ = 0;
		}

		active_ = false;
		current_name_.clear();
		warned_map_failed_ = false;
		warned_create_failed_ = false;
		warned_stage_failed_ = false;
	}

	bool is_active() const override { return active_; }

private:
	/* ---- video sender ---- */

	void recreate_sender(const std::string &name)
	{
		/* Mutated on the graphics thread but read by the audio capture thread,
		 * so swap under the lock. */
		std::lock_guard<std::mutex> lock(sender_mutex_);

		if (sender_) {
			runtime_->lib()->send_destroy(sender_);
			sender_ = nullptr;
		}

		NDIlib_send_create_t desc = {};
		desc.p_ndi_name = name.c_str();
		desc.p_groups = nullptr;
		/* OBS's render loop drives our cadence; don't let NDI rate-limit us. */
		desc.clock_video = false;
		desc.clock_audio = false;

		sender_ = runtime_->lib()->send_create(&desc);
		if (sender_) {
			current_name_ = name;
			warned_create_failed_ = false;
			amv_log_detailed(LOG_INFO, "[multiview-output/ndi] sender created '%s'", name.c_str());
		} else if (!warned_create_failed_) {
			obs_log(LOG_WARNING, "[multiview-output/ndi] send_create failed for '%s'", name.c_str());
			warned_create_failed_ = true;
		}
	}

	bool ensure_stage(uint32_t w, uint32_t h)
	{
		if (stage_ && stage_w_ == w && stage_h_ == h)
			return true;

		if (stage_) {
			gs_stagesurface_destroy(stage_);
			stage_ = nullptr;
		}

		stage_ = gs_stagesurface_create(w, h, GS_BGRA);
		if (!stage_) {
			stage_w_ = stage_h_ = 0;
			if (!warned_stage_failed_) {
				obs_log(LOG_WARNING, "[multiview-output/ndi] gs_stagesurface_create(%ux%u) failed", w,
					h);
				warned_stage_failed_ = true;
			}
			return false;
		}

		stage_w_ = w;
		stage_h_ = h;
		warned_stage_failed_ = false;
		return true;
	}

	/* ---- audio capture ---- */

	/* Resolve the OBS mixer track index (0..5) for the configured source. */
	size_t resolve_mix_index() const
	{
		if (want_audio_mode_ == OutputAudioMode::ManualTrack) {
			int idx = want_audio_track_;
			if (idx < 1)
				idx = 1;
			else if (idx > 6)
				idx = 6;
			return (size_t)(idx - 1);
		}

		/* FollowStreaming: lowest set bit of the streaming output's mixer mask
		 * (single-track semantics, matching the VU meter's AutoFollowStreaming).
		 * Read from the main-thread-updated frontend cache — never call
		 * obs_frontend_* on the render/graphics thread (issue #10 isolation F2). */
		uint32_t mask = amv_frontend::streaming_mixers();
		for (int i = 0; i < 6; i++) {
			if (mask & (1u << i))
				return (size_t)i;
		}
		return 0; /* Track 1 fallback */
	}

	void connect_audio(size_t mix)
	{
		audio_t *audio = obs_get_audio();
		struct obs_audio_info oai;
		if (!audio || !obs_get_audio_info(&oai))
			return;

		/* Request OBS's native planar float at the canvas rate/layout — no
		 * resampling, just per-channel planes we repack for NDI's FLTP. */
		struct audio_convert_info conv = {};
		conv.samples_per_sec = oai.samples_per_sec;
		conv.format = AUDIO_FORMAT_FLOAT_PLANAR;
		conv.speakers = oai.speakers;

		/* Cache the format we asked OBS to deliver. The capture callback sizes
		 * each frame from these rather than re-querying obs_get_audio_info on
		 * the audio thread: the conversion above fixes the delivered layout for
		 * the life of the connection, so a mid-session settings change must not
		 * make the callback read a different channel count than is delivered.
		 * Set before connect so the audio thread observes them. */
		connected_channels_ = (int)get_audio_channels(oai.speakers);
		connected_sample_rate_ = (int)oai.samples_per_sec;

		audio_output_connect(audio, mix, &conv, audio_capture, this);
		audio_handle_ = audio;
		connected_mix_ = mix;
		audio_connected_ = true;
		amv_log_detailed(LOG_INFO, "[multiview-output/ndi] audio capture connected (mixer track %zu)", mix + 1);
	}

	void disconnect_audio()
	{
		if (audio_connected_ && audio_handle_)
			audio_output_disconnect(audio_handle_, connected_mix_, audio_capture, this);
		audio_connected_ = false;
		connected_mix_ = (size_t)-1;
		audio_handle_ = nullptr;
	}

	static void audio_capture(void *param, size_t mix_idx, struct audio_data *data)
	{
		(void)mix_idx;
		static_cast<NdiOutputBackend *>(param)->on_audio(data);
	}

	/* OBS audio thread. */
	void on_audio(struct audio_data *data)
	{
		if (!data || data->frames == 0)
			return;

		std::lock_guard<std::mutex> lock(sender_mutex_);
		if (!sender_ || !runtime_)
			return;

		const int channels = connected_channels_;
		if (channels <= 0)
			return;

		const int frames = (int)data->frames;
		const size_t stride = (size_t)frames * sizeof(float);

		/* NDI FLTP wants contiguous per-channel planes; OBS gives separate
		 * plane pointers, so repack into one buffer (reused across frames). */
		audio_buffer_.resize(stride * (size_t)channels);
		for (int ch = 0; ch < channels; ch++) {
			uint8_t *dst = audio_buffer_.data() + (size_t)ch * stride;
			if (data->data[ch])
				std::memcpy(dst, data->data[ch], stride);
			else
				std::memset(dst, 0, stride);
		}

		NDIlib_audio_frame_v3_t af = {};
		af.sample_rate = connected_sample_rate_;
		af.no_channels = channels;
		af.no_samples = frames;
		af.timecode = NDIlib_send_timecode_synthesize;
		af.FourCC = NDIlib_FourCC_audio_type_FLTP;
		af.p_data = audio_buffer_.data();
		af.channel_stride_in_bytes = (int)stride;

		runtime_->lib()->send_send_audio_v3(sender_, &af);
	}

	std::shared_ptr<NdiRuntime> runtime_;

	/* sender_ is created/destroyed on the graphics thread and read on the audio
	 * capture thread; sender_mutex_ guards its validity across the two. */
	std::mutex sender_mutex_;
	NDIlib_send_instance_t sender_ = nullptr;

	gs_stagesurf_t *stage_ = nullptr;
	uint32_t stage_w_ = 0, stage_h_ = 0;
	std::string current_name_;
	bool active_ = false;
	bool warned_map_failed_ = false;
	bool warned_create_failed_ = false;
	bool warned_stage_failed_ = false;

	/* Audio capture state (audio_buffer_ is touched only in on_audio). */
	audio_t *audio_handle_ = nullptr;
	bool audio_connected_ = false;
	size_t connected_mix_ = (size_t)-1;
	int connected_channels_ = 0;    /* set at connect; read on the audio thread */
	int connected_sample_rate_ = 0; /* set at connect; read on the audio thread */
	OutputAudioMode want_audio_mode_ = OutputAudioMode::FollowStreaming;
	int want_audio_track_ = 1;
	uint64_t audio_resolve_counter_ = 0;
	std::vector<uint8_t> audio_buffer_;
};

} /* anonymous namespace */

std::unique_ptr<IMultiviewOutputBackend> create_ndi_output_backend()
{
	return std::make_unique<NdiOutputBackend>();
}

#endif /* AMV_ENABLE_NDI_OUTPUT */
