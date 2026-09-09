/*
OBS Advanced Multiview - multiview output layer (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-output.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <set>
#include <vector>

#ifdef AMV_ENABLE_SPOUT_OUTPUT
#include "multiview-output-spout.hpp"
#endif

#ifdef AMV_ENABLE_NDI_OUTPUT
#include "multiview-output-ndi.hpp"
#include "multiview-ndi-runtime.hpp"
#endif

#ifdef AMV_ENABLE_DECKLINK_OUTPUT
#include "multiview-output-decklink.hpp"
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

bool MultiviewOutputManager::ndi_supported()
{
#ifdef AMV_ENABLE_NDI_OUTPUT
	return NdiRuntime::available();
#else
	return false;
#endif
}

bool MultiviewOutputManager::decklink_supported()
{
#ifdef AMV_ENABLE_DECKLINK_OUTPUT
	/* obs_output_create returns a lazy object even for an unregistered id, so
	 * probe the registered output flags: non-zero => the OBS DeckLink plugin is
	 * present and "decklink_output" is usable. */
	return obs_get_output_flags("decklink_output") != 0;
#else
	return false;
#endif
}

bool MultiviewOutputManager::backend_available(Kind k)
{
	switch (k) {
	case Kind::Spout:
		return spout_supported();
	case Kind::Ndi:
		return ndi_supported();
	case Kind::Decklink:
		return decklink_supported();
	}
	return false;
}

const char *MultiviewOutputManager::kind_name(Kind k)
{
	switch (k) {
	case Kind::Spout:
		return "Spout";
	case Kind::Ndi:
		return "NDI";
	case Kind::Decklink:
		return "DeckLink";
	}
	return "?";
}

std::unique_ptr<IMultiviewOutputBackend> MultiviewOutputManager::create_backend(Kind k)
{
#ifdef AMV_ENABLE_SPOUT_OUTPUT
	if (k == Kind::Spout)
		return create_spout_output_backend();
#endif
#ifdef AMV_ENABLE_NDI_OUTPUT
	if (k == Kind::Ndi)
		return create_ndi_output_backend();
#endif
#ifdef AMV_ENABLE_DECKLINK_OUTPUT
	if (k == Kind::Decklink)
		return create_decklink_output_backend();
#endif
	(void)k;
	return nullptr;
}

void MultiviewOutputManager::reconcile(BackendEntry &e, const OutputBackendSettings &s, Kind kind)
{
	const bool want = s.enabled && backend_available(kind);

	if (want && !e.backend) {
		e.backend = create_backend(kind);
		e.frame = 0;
		if (e.backend)
			obs_log(LOG_INFO, "[multiview-output] %s output enabled", kind_name(kind));
	} else if (!want && e.backend) {
		e.backend->stop();
		e.backend.reset();
		e.frame = 0;
		obs_log(LOG_INFO, "[multiview-output] %s output disabled", kind_name(kind));
	}

	e.enabled = (e.backend != nullptr);
	if (e.enabled) {
		auto dims = resolve_output_dimensions(s);
		e.w = dims.first;
		e.h = dims.second;
		/* M1: a backend with an authoritative compose size (DeckLink locked to
		 * its hardware mode raster once Running) overrides the persisted snapshot,
		 * so a stale/mismatched customWidth/customHeight can't silently drop every
		 * frame. Not-yet-Running backends report false and keep the resolve value. */
		uint32_t bw = 0, bh = 0;
		if (e.backend->compose_size(bw, bh) && bw > 0 && bh > 0) {
			e.w = bw;
			e.h = bh;
		}
		e.fpsDivisor = (s.fpsDivisor == 2) ? 2 : 1;
		/* (Re)connect audio capture to the selected track (NDI only). */
		e.backend->configure_audio(s);
	}
}

gs_texrender_t *MultiviewOutputManager::get_texrender(uint64_t key)
{
	auto it = texrenders_.find(key);
	if (it != texrenders_.end())
		return it->second;
	gs_texrender_t *tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (tr)
		texrenders_[key] = tr;
	return tr;
}

void MultiviewOutputManager::render_one_resolution(const std::string &name, uint32_t w, uint32_t h,
						   const std::function<void(int w, int h)> &draw)
{
	gs_texrender_t *tr = get_texrender(res_key(w, h));
	if (!tr)
		return;

	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return;

	/* Establish an ambient viewport/projection matching the texrender's pixel
	 * space. Most draw_grid helpers wrap their own startRegion, but
	 * render_safe_area draws against the ambient projection and
	 * gs_texrender_begin sets no ortho (issue #11). */
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	struct vec4 clear_color;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	draw((int)w, (int)h);

	gs_texrender_end(tr);

	gs_texture_t *tex = gs_texrender_get_texture(tr);
	if (!tex)
		return;

	/* Submit to every enabled backend at THIS resolution that is due this
	 * frame (frame % fpsDivisor == 0) AND wants a frame (#2: an NDI backend with
	 * no receiver is skipped even when a co-resolution backend forced the
	 * compose). */
	BackendEntry *entries[] = {&spout_, &ndi_, &decklink_};
	for (BackendEntry *e : entries) {
		if (e->enabled && e->w == w && e->h == h && (e->frame % e->fpsDivisor) == 0 && e->backend &&
		    e->backend->wants_frame())
			e->backend->submit_frame(name, tex, w, h, e->fpsDivisor);
	}
}

void MultiviewOutputManager::render_all(const std::string &name, const InstanceOutputSettings &cfg,
					const std::function<void(int w, int h)> &draw, bool ndiDoubleBuffer)
{
	if (!draw)
		return;

	reconcile(spout_, cfg.spout, Kind::Spout);
	reconcile(ndi_, cfg.ndi, Kind::Ndi);
	reconcile(decklink_, cfg.decklink, Kind::Decklink);

	/* Push the user's global NDI readback double-buffer choice to the backend
	 * (graphics thread; no-op on Spout). Cheap to set every frame. DeckLink
	 * always double-buffers internally (never stall the main program), so it
	 * ignores this too. */
	if (ndi_.backend)
		ndi_.backend->set_double_buffer(ndiDoubleBuffer);

	BackendEntry *entries[] = {&spout_, &ndi_, &decklink_};

	/* #2: give every enabled backend a chance to (re)create its sender so it
	 * stays discoverable even on frames we skip. NDI needs this so receivers can
	 * connect while idle; Spout's prepare() is a no-op. Done before any compose. */
	for (BackendEntry *e : entries) {
		if (e->enabled && e->backend)
			e->backend->prepare(name);
	}

	/* Unique resolutions that have at least one backend DUE this frame.
	 * Resolutions whose backends are all off-beat (half-rate, odd frame) are
	 * skipped entirely — that is what makes half-rate halve the compose cost.
	 *
	 * #2: a backend is "due" only if it also WANTS a frame right now
	 * (wants_frame()). An NDI sender with no receiver wants nothing, so its
	 * resolution drops out of due_res and we skip the whole compose+readback+
	 * encode. It STAYS in live_res (keyed on `enabled`, not wants_frame), so its
	 * texrender is never GC'd — reconnect reuses the existing target with zero
	 * realloc spike. */
	std::set<uint64_t> due_res;
	std::set<uint64_t> live_res;
	for (BackendEntry *e : entries) {
		if (!e->enabled || e->w == 0 || e->h == 0)
			continue;
		live_res.insert(res_key(e->w, e->h));
		if ((e->frame % e->fpsDivisor) == 0 && e->backend && e->backend->wants_frame())
			due_res.insert(res_key(e->w, e->h));
	}

	for (uint64_t key : due_res) {
		uint32_t w = (uint32_t)(key >> 32);
		uint32_t h = (uint32_t)(key & 0xffffffffu);
		render_one_resolution(name, w, h, draw);
	}

	/* Advance frame counters for all enabled backends. */
	for (BackendEntry *e : entries) {
		if (e->enabled)
			e->frame++;
	}

	/* GC texrenders no longer matching any live resolution (resolution change
	 * or all-disabled). */
	for (auto it = texrenders_.begin(); it != texrenders_.end();) {
		if (live_res.find(it->first) == live_res.end()) {
			gs_texrender_destroy(it->second);
			it = texrenders_.erase(it);
		} else {
			++it;
		}
	}
}

void MultiviewOutputManager::teardown_locked()
{
	if (spout_.backend) {
		spout_.backend->stop();
		spout_.backend.reset();
	}
	if (ndi_.backend) {
		ndi_.backend->stop();
		ndi_.backend.reset();
	}
	if (decklink_.backend) {
		decklink_.backend->stop();
		decklink_.backend.reset();
	}
	spout_.enabled = false;
	ndi_.enabled = false;
	decklink_.enabled = false;

	for (auto &kv : texrenders_)
		gs_texrender_destroy(kv.second);
	texrenders_.clear();
}

void MultiviewOutputManager::shutdown_graphics()
{
	if (!spout_.backend && !ndi_.backend && !decklink_.backend && texrenders_.empty())
		return;

	obs_enter_graphics();
	teardown_locked();
	obs_leave_graphics();
}
