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

MultiviewOutputManager::MultiviewOutputManager()
{
	/* One live slot per built backend, in registry order. Every reconcile/
	 * render/teardown/shutdown path iterates backends_, so a backend that is in
	 * the registry is covered by all of them or by none — never a subset. */
	const auto &registry = output_backend_registry();
	backends_.reserve(registry.size());
	for (const auto &desc : registry) {
		BackendEntry e;
		e.kind = desc.kind;
		backends_.push_back(std::move(e));
	}
}

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

const std::vector<OutputBackendDesc> &output_backend_registry()
{
	/* Program-lifetime static table. Each descriptor's available/create point at
	 * the existing per-backend gates and factories, so the behavior of "is this
	 * backend possible" and "make one" is unchanged — only the dispatch moves
	 * from three switches into this table. Conditionally compiled: a backend
	 * whose feature is off is simply not registered (matching the old inert
	 * named member, which was always present but could never be created). */
	static const std::vector<OutputBackendDesc> registry = [] {
		std::vector<OutputBackendDesc> r;
#ifdef AMV_ENABLE_SPOUT_OUTPUT
		r.push_back({OutputBackendKind::Spout, "spout", "Spout", &MultiviewOutputManager::spout_supported,
			     &create_spout_output_backend, /*supportsAudio=*/false});
#endif
#ifdef AMV_ENABLE_NDI_OUTPUT
		r.push_back({OutputBackendKind::Ndi, "ndi", "NDI", &MultiviewOutputManager::ndi_supported,
			     &create_ndi_output_backend, /*supportsAudio=*/true});
#endif
#ifdef AMV_ENABLE_DECKLINK_OUTPUT
		r.push_back({OutputBackendKind::Decklink, "decklink", "DeckLink",
			     &MultiviewOutputManager::decklink_supported, &create_decklink_output_backend,
			     /*supportsAudio=*/true});
#endif
		return r;
	}();
	return registry;
}

/* The descriptor for a live slot's kind. backends_ is built from the registry,
 * so every live slot's kind resolves in the loop below; the fallback is
 * unreachable on any real path (an empty registry yields no slots to reconcile,
 * so desc_for is never called). Degrade to a program-lifetime empty descriptor
 * rather than registry.front() so a future misuse — or an all-backends-compiled-
 * out build — can never dereference past the end of an empty vector. */
static const OutputBackendDesc &desc_for(OutputBackendKind kind)
{
	const auto &registry = output_backend_registry();
	for (const auto &d : registry)
		if (d.kind == kind)
			return d;
	static const OutputBackendDesc kUnknown{};
	return kUnknown;
}

void MultiviewOutputManager::reconcile(BackendEntry &e, const InstanceOutputSettings &cfg)
{
	const OutputBackendSettings &s = cfg.at(e.kind);
	const OutputBackendDesc &desc = desc_for(e.kind);
	const bool want = s.enabled && desc.available();

	if (want && !e.backend) {
		e.backend = desc.create();
		e.frame = 0;
		if (e.backend)
			obs_log(LOG_INFO, "[multiview-output] %s output enabled", desc.displayName);
	} else if (!want && e.backend) {
		e.backend->stop();
		e.backend.reset();
		e.frame = 0;
		obs_log(LOG_INFO, "[multiview-output] %s output disabled", desc.displayName);
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
		/* Push the DeckLink hardware settings BEFORE configure_audio: the
		 * DeckLink backend caches both into one DeckCfg and drives its
		 * create/restart decision from configure_audio, which must therefore see
		 * the hardware fields already set. Called unconditionally on every
		 * backend (no manager-side kind switch); non-DeckLink backends default it
		 * to a no-op, matching the set_double_buffer pattern. */
		e.backend->configure_decklink(cfg.decklink);
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
	for (BackendEntry &e : backends_) {
		if (e.enabled && e.w == w && e.h == h && (e.frame % e.fpsDivisor) == 0 && e.backend &&
		    e.backend->wants_frame())
			e.backend->submit_frame(name, tex, w, h, e.fpsDivisor);
	}
}

void MultiviewOutputManager::render_all(const std::string &name, const InstanceOutputSettings &cfg,
					const std::function<void(int w, int h)> &draw, bool ndiDoubleBuffer)
{
	if (!draw)
		return;

	for (BackendEntry &e : backends_)
		reconcile(e, cfg);

	/* Push the user's global NDI readback double-buffer choice to the backend
	 * (graphics thread). Cheap to set every frame. Only NDI honors it; Spout has
	 * no readback and DeckLink always double-buffers internally (never stall the
	 * main program), so both implement set_double_buffer as a no-op — iterating
	 * every backend here is behaviorally identical to the old NDI-only call. */
	for (BackendEntry &e : backends_) {
		if (e.backend)
			e.backend->set_double_buffer(ndiDoubleBuffer);
	}

	/* #2: give every enabled backend a chance to (re)create its sender so it
	 * stays discoverable even on frames we skip. NDI needs this so receivers can
	 * connect while idle; Spout's prepare() is a no-op. Done before any compose. */
	for (BackendEntry &e : backends_) {
		if (e.enabled && e.backend)
			e.backend->prepare(name);
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
	for (BackendEntry &e : backends_) {
		if (!e.enabled || e.w == 0 || e.h == 0)
			continue;
		live_res.insert(res_key(e.w, e.h));
		if ((e.frame % e.fpsDivisor) == 0 && e.backend && e.backend->wants_frame())
			due_res.insert(res_key(e.w, e.h));
	}

	for (uint64_t key : due_res) {
		uint32_t w = (uint32_t)(key >> 32);
		uint32_t h = (uint32_t)(key & 0xffffffffu);
		render_one_resolution(name, w, h, draw);
	}

	/* Advance frame counters for all enabled backends. */
	for (BackendEntry &e : backends_) {
		if (e.enabled)
			e.frame++;
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
	/* Stop + release every backend in registry order (Spout, NDI, DeckLink),
	 * then clear its enabled flag — identical to the old three explicit blocks. */
	for (BackendEntry &e : backends_) {
		if (e.backend) {
			e.backend->stop();
			e.backend.reset();
		}
	}
	for (BackendEntry &e : backends_)
		e.enabled = false;

	for (auto &kv : texrenders_)
		gs_texrender_destroy(kv.second);
	texrenders_.clear();
}

void MultiviewOutputManager::shutdown_graphics()
{
	bool any_backend = false;
	for (const BackendEntry &e : backends_) {
		if (e.backend) {
			any_backend = true;
			break;
		}
	}
	if (!any_backend && texrenders_.empty())
		return;

	obs_enter_graphics();
	teardown_locked();
	obs_leave_graphics();
}
