/*
OBS Advanced Multiview - multiview output layer (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-output.hpp"
#include "multiview-instance.hpp" /* signal_provider_supported_on_platform */
#include "amv-logging.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#ifdef AMV_ENABLE_SPOUT_OUTPUT
#include "multiview-output-spout.hpp"
#endif

MultiviewOutputManager::MultiviewOutputManager() = default;

MultiviewOutputManager::~MultiviewOutputManager()
{
	shutdown_graphics();
}

bool MultiviewOutputManager::spout_supported()
{
#ifdef AMV_ENABLE_SPOUT_OUTPUT
	/* Reuse the existing Spout platform gate (Windows-only) already used by
	 * the Spout *input* provider. The D3D11-renderer check happens on the
	 * graphics thread inside the backend. Note: unlike input, output does
	 * NOT require obs-spout2 — we send via our own SpoutDX. */
	return signal_provider_supported_on_platform(SignalProviderType::Spout);
#else
	return false;
#endif
}

bool MultiviewOutputManager::set_spout_enabled(bool enabled)
{
	if (enabled == spout_enabled())
		return spout_enabled();

#ifdef AMV_ENABLE_SPOUT_OUTPUT
	if (enabled) {
		if (!spout_supported()) {
			obs_log(LOG_WARNING, "[multiview-output] Spout output unavailable on this platform");
			return false;
		}
		auto backend = create_spout_output_backend();
		IMultiviewOutputBackend *raw = backend.get();
		/* backends_ is read by render_and_dispatch on the graphics thread.
		 * This runs on the UI thread, so serialize the mutation against
		 * the render loop by taking the graphics lock (the render-loop
		 * draw callbacks hold it for the duration of a frame). */
		obs_enter_graphics();
		backends_.push_back(std::move(backend));
		obs_leave_graphics();
		spout_ = raw;
		obs_log(LOG_INFO, "[multiview-output] Spout output enabled");
		return true;
	}

	/* Disable: stop the sender and drop it, all under the graphics lock so
	 * a concurrent render_and_dispatch never sees a half-erased vector. */
	obs_enter_graphics();
	if (spout_)
		spout_->stop();
	for (auto it = backends_.begin(); it != backends_.end(); ++it) {
		if (it->get() == spout_) {
			backends_.erase(it);
			break;
		}
	}
	obs_leave_graphics();
	spout_ = nullptr;
	obs_log(LOG_INFO, "[multiview-output] Spout output disabled");
	return false;
#else
	return false;
#endif
}

gs_texture_t *MultiviewOutputManager::render_and_dispatch(const std::string &name, uint32_t w, uint32_t h,
							  const std::function<void()> &draw)
{
	if (backends_.empty() || w == 0 || h == 0 || !draw)
		return nullptr;

	if (!texrender_)
		texrender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!texrender_)
		return nullptr;

	gs_texrender_reset(texrender_);
	if (!gs_texrender_begin(texrender_, w, h))
		return nullptr;

	/* Establish an ambient viewport/projection matching the texrender's
	 * pixel space. Most draw_grid helpers wrap their own startRegion (which
	 * pushes viewport+ortho), but render_safe_area draws against the ambient
	 * projection. gs_texrender_begin does NOT set an ortho, so without this
	 * the display's physical-pixel ortho would leak in and map the safe-area
	 * guides at the wrong scale (issue #11: shrank to the top-left quarter). */
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	/* Opaque black background; draw_grid paints gutter/cells over it. */
	struct vec4 clear_color;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	draw();

	gs_texrender_end(texrender_);

	gs_texture_t *tex = gs_texrender_get_texture(texrender_);
	if (tex) {
		for (auto &backend : backends_)
			backend->submit_frame(name, tex, w, h);
	}
	return tex;
}

void MultiviewOutputManager::shutdown_graphics()
{
	if (backends_.empty() && !texrender_)
		return;

	obs_enter_graphics();
	for (auto &backend : backends_)
		backend->stop();
	if (texrender_) {
		gs_texrender_destroy(texrender_);
		texrender_ = nullptr;
	}
	obs_leave_graphics();

	backends_.clear();
	spout_ = nullptr;
}
