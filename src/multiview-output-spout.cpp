/*
OBS Advanced Multiview - Spout output backend (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#ifdef AMV_ENABLE_SPOUT_OUTPUT

#include "multiview-output-spout.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/graphics.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>

#include <SpoutDX.h>

#include <string>

namespace {

/* Spout shares B8G8R8A8 textures to match the manager's GS_BGRA texrender. */
constexpr DXGI_FORMAT kSpoutFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

class SpoutOutputBackend : public IMultiviewOutputBackend {
public:
	~SpoutOutputBackend() override { stop(); }

	const char *kind() const override { return "spout"; }

	void submit_frame(const std::string &name, gs_texture_t *tex, uint32_t w, uint32_t h, int fpsDivisor) override
	{
		/* Spout shares a bare GPU texture with no frame-rate metadata. */
		(void)fpsDivisor;

		if (!tex || w == 0 || h == 0)
			return;

		/* Spout is a DirectX 11 shared-texture mechanism. If OBS is
		 * running its OpenGL renderer (rare on Windows), we can't share
		 * the texture; warn once and stay dormant. */
		if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
			if (!warned_no_d3d11_) {
				obs_log(LOG_WARNING,
					"[multiview-output/spout] disabled: OBS is not using the Direct3D 11 renderer");
				warned_no_d3d11_ = true;
			}
			return;
		}

		if (!ensure_open())
			return;

		auto *d3dtex = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(tex));
		if (!d3dtex)
			return;

		if (name != current_name_) {
			spout_.SetSenderName(name.c_str());
			current_name_ = name;
			amv_log_detailed(LOG_INFO, "[multiview-output/spout] sender name set to '%s'", name.c_str());
		}

		if (spout_.SendTexture(d3dtex))
			active_ = true;
	}

	void stop() override
	{
		if (opened_) {
			spout_.ReleaseSender();
			spout_.CloseDirectX11();
			opened_ = false;
			obs_log(LOG_INFO, "[multiview-output/spout] sender released ('%s')", current_name_.c_str());
		}
		active_ = false;
		current_name_.clear();
		warned_no_d3d11_ = false;
		warned_open_failed_ = false;
	}

	bool is_active() const override { return active_; }

private:
	bool ensure_open()
	{
		if (opened_)
			return true;

		/* Share OBS's own D3D11 device so SendTexture's GPU copy stays
		 * on a single device/context (we're on the graphics thread, so
		 * OBS isn't touching the immediate context concurrently). */
		auto *dev = static_cast<ID3D11Device *>(gs_get_device_obj());
		if (!dev) {
			/* ensure_open() runs every frame while enabled — log the
			 * failure only once so a persistent failure can't spam. */
			if (!warned_open_failed_) {
				obs_log(LOG_WARNING, "[multiview-output/spout] gs_get_device_obj returned null");
				warned_open_failed_ = true;
			}
			return false;
		}
		if (!spout_.OpenDirectX11(dev)) {
			if (!warned_open_failed_) {
				obs_log(LOG_WARNING, "[multiview-output/spout] OpenDirectX11 failed");
				warned_open_failed_ = true;
			}
			return false;
		}
		spout_.SetSenderFormat(kSpoutFormat);
		opened_ = true;
		warned_open_failed_ = false;
		return true;
	}

	spoutDX spout_;
	std::string current_name_;
	bool opened_ = false;
	bool active_ = false;
	bool warned_no_d3d11_ = false;
	bool warned_open_failed_ = false;
};

} /* anonymous namespace */

std::unique_ptr<IMultiviewOutputBackend> create_spout_output_backend()
{
	return std::make_unique<SpoutOutputBackend>();
}

#endif /* AMV_ENABLE_SPOUT_OUTPUT */
