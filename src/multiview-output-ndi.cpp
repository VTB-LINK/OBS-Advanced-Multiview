/*
OBS Advanced Multiview - NDI output backend (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#ifdef AMV_ENABLE_NDI_OUTPUT

#include "multiview-output-ndi.hpp"
#include "multiview-ndi-runtime.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/graphics.h>

#include <Processing.NDI.Lib.h>

#include <memory>
#include <string>

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

		/* Synchronous send: NDI copies the buffer before returning, so it is
		 * safe to unmap immediately. (The async variant would require the
		 * mapped pointer to stay valid past this call.) */
		runtime_->lib()->send_send_video_v2(sender_, &frame);

		gs_stagesurface_unmap(stage_);
		active_ = true;
		warned_map_failed_ = false;
	}

	void stop() override
	{
		/* Destroy the sender BEFORE releasing the runtime handle: the last
		 * handle release calls NDIlib_destroy, which requires all senders
		 * gone. */
		if (sender_ && runtime_) {
			runtime_->lib()->send_destroy(sender_);
			obs_log(LOG_INFO, "[multiview-output/ndi] sender released ('%s')", current_name_.c_str());
		}
		sender_ = nullptr;

		if (stage_) {
			gs_stagesurface_destroy(stage_);
			stage_ = nullptr;
			stage_w_ = stage_h_ = 0;
		}

		runtime_.reset();
		active_ = false;
		current_name_.clear();
		warned_map_failed_ = false;
		warned_create_failed_ = false;
		warned_stage_failed_ = false;
	}

	bool is_active() const override { return active_; }

private:
	void recreate_sender(const std::string &name)
	{
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

	std::shared_ptr<NdiRuntime> runtime_;
	NDIlib_send_instance_t sender_ = nullptr;
	gs_stagesurf_t *stage_ = nullptr;
	uint32_t stage_w_ = 0, stage_h_ = 0;
	std::string current_name_;
	bool active_ = false;
	bool warned_map_failed_ = false;
	bool warned_create_failed_ = false;
	bool warned_stage_failed_ = false;
};

} /* anonymous namespace */

std::unique_ptr<IMultiviewOutputBackend> create_ndi_output_backend()
{
	return std::make_unique<NdiOutputBackend>();
}

#endif /* AMV_ENABLE_NDI_OUTPUT */
