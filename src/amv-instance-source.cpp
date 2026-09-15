/*
OBS Advanced Multiview - hidden nested-source obs type `amv_instance_source`
(issue #20 P2 + P3).

Registers a real obs_source that renders ANOTHER AMV instance's already-published
composited picture as a per-cell source. It carries OBS_SOURCE_CAP_DISABLED so it
never appears in OBS's "Add Source" list, yet the AmvInstance signal provider can
still create it privately (obs_source_create_private). The rest of the runtime
(refresh_cell, the health supervisor, draw_cells) treats it as an ordinary
external private source — no special casing there, except the one graphics-thread
render-target hand-off described below.

Data flow (all on the OBS graphics thread):
  - video_render resolves the target core from the graphics-lock-maintained
    snapshot (multiview_pull_target_graphics — never g_registry_mutex, which would
    invert graphics->registry and deadlock), records demand for the (R, mode)
    picture on the target, samples its published FRONT texture, and blits it.
  - The target's own frames are composed independently by plugin-main's render
    driver (compose_consumer_frames), so we only ever read a completed FRONT.
    That is the "read the previous frame" model that makes cycles / self-reference
    safe by construction (design §3.1 / §3.5): we NEVER synchronously trigger the
    target's composition here, NEVER take the target's source_mutex_, and NEVER
    take g_registry_mutex.

Resolution R (P3, design §3.3/§3.4) is per-source and one of three modes:
  - FollowWindow (default): R = the pixel size of the render target the current
    pass draws into — the pulling window's display size, the output raster, or the
    outer nested compose R. draw_cells hands us that size via
    amv_instance_source_note_render_target() just before it reads our
    get_width/get_height, so the target composes at the real size and aspect of
    the window that is showing it (a large, window-level picture scaled down into
    the small cell — NOT the tiny cell size, which would blow up the target's UI).
    We deliberately do NOT use gs_get_width/gs_get_height: those report the
    D3D11 SWAP CHAIN size and log an error + return 0 in the offscreen output /
    headless-consumer passes (obs main-rendered runs with a texrender bound, no
    swap chain), so they are neither correct nor quiet there.
  - FollowScreen: R = the primary display resolution (mirrored from the Qt UI
    thread via amv_primary_screen_size).
  - Manual: R = a fixed external-output preset (canvas / output / rescale /
    custom), via resolve_output_dimensions.
R is always clamped to AmvInstanceCore::kConsumerMaxDim so get_width/get_height,
the demanded picture, and the published texture all agree (make_consumer_key
clamps identically). get_width/get_height report R so draw_cells' letterbox math
stays consistent with the composed picture — one aspect-preserving downscale, no
second scale, no forced canvas-aspect box.

Picture mode: "full" (default; the target's full composition with labels / VU /
overlays) or "grid" (per-cell pictures only). Both parsed once at create.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "multiview-instance.hpp"
#include "multiview-window.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <algorithm>
#include <atomic>
#include <string>

namespace {

struct AmvInstanceSource {
	obs_source_t *self = nullptr;
	/* Target instance UUID + picture mode + resolution policy, parsed once at
	 * create. Our private source is never obs_source_update'd after creation (a
	 * cell edit recreates it via refresh_cell), so these are effectively
	 * immutable and safe to read lock-free on the graphics thread. update()
	 * re-parses defensively. */
	std::string target_uuid;
	AmvInstanceCore::ConsumerPictureMode mode = AmvInstanceCore::ConsumerPictureMode::Full;
	amv_nested::ResMode res_mode = amv_nested::ResMode::FollowWindow;
	OutputResolutionMode manual_res = OutputResolutionMode::CanvasBase;
	uint32_t manual_w = 1920; /* Custom preset width */
	uint32_t manual_h = 1080; /* Custom preset height */

	/* FollowWindow only: the render-target pixel size of the pass currently
	 * drawing this source, pushed each frame by draw_cells (graphics thread)
	 * before it reads get_width/get_height. Atomic because an off-thread
	 * obs_source_get_width query could race the graphics-thread write; a stale
	 * value is harmless (it only shifts R by one frame). Zero until the first
	 * draw -> resolve() falls back to the canvas. */
	std::atomic<uint32_t> rt_w{0};
	std::atomic<uint32_t> rt_h{0};
};

AmvInstanceCore::ConsumerPictureMode parse_mode(obs_data_t *settings)
{
	const char *m = settings ? obs_data_get_string(settings, amv_nested::kPictureModeKey) : nullptr;
	if (m && *m && std::string(m) == "grid")
		return AmvInstanceCore::ConsumerPictureMode::GridOnly;
	return AmvInstanceCore::ConsumerPictureMode::Full;
}

void load_settings(AmvInstanceSource *s, obs_data_t *settings)
{
	const char *uuid = settings ? obs_data_get_string(settings, amv_nested::kTargetUuidKey) : nullptr;
	s->target_uuid = (uuid && *uuid) ? uuid : "";
	s->mode = parse_mode(settings);

	const char *rm = settings ? obs_data_get_string(settings, amv_nested::kResModeKey) : nullptr;
	s->res_mode = amv_nested::res_mode_from_string(rm);

	const char *mrm = settings ? obs_data_get_string(settings, amv_nested::kManualResModeKey) : nullptr;
	s->manual_res = output_res_mode_from_str(mrm);

	if (settings && obs_data_has_user_value(settings, amv_nested::kManualCustomWKey))
		s->manual_w = (uint32_t)obs_data_get_int(settings, amv_nested::kManualCustomWKey);
	if (settings && obs_data_has_user_value(settings, amv_nested::kManualCustomHKey))
		s->manual_h = (uint32_t)obs_data_get_int(settings, amv_nested::kManualCustomHKey);
}

/* FollowWindow R is snapped up to this quantum so a live window resize (the
 * render-target size changes every frame) does not mint a brand-new consumer key
 * each frame. A brand-new key has no composed front yet (the target composes it
 * next frame), so an un-quantized resize would blank the nested cell for the whole
 * drag; quantizing keeps the key stable within a band, limiting the churn to a
 * one-frame blip at each band crossing (issue #20 P3 audit). */
constexpr uint32_t kFollowWindowQuantum = 128;

/* R = OBS canvas base size, the fallback used when a follow-mode size is not yet
 * known (before the first draw / no video pipeline). Returns false on a zero /
 * unavailable canvas. */
bool canvas_dims(uint32_t &w, uint32_t &h)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return false;
	w = std::min(ovi.base_width, AmvInstanceCore::kConsumerMaxDim);
	h = std::min(ovi.base_height, AmvInstanceCore::kConsumerMaxDim);
	return w > 0 && h > 0;
}

/* Resolve the composition resolution R for this source per its resolution mode.
 * All three modes clamp to kConsumerMaxDim so demand / read / advertised size all
 * agree. Returns false only if even the canvas fallback is unavailable. */
bool resolve_dims(AmvInstanceSource *s, uint32_t &w, uint32_t &h)
{
	if (!s)
		return false;
	w = 0;
	h = 0;
	switch (s->res_mode) {
	case amv_nested::ResMode::Manual: {
		OutputBackendSettings tmp;
		tmp.resMode = s->manual_res;
		tmp.customWidth = s->manual_w;
		tmp.customHeight = s->manual_h;
		const auto d = resolve_output_dimensions(tmp);
		w = d.first;
		h = d.second;
		break;
	}
	case amv_nested::ResMode::FollowScreen: {
		uint32_t sw = 0, sh = 0;
		if (amv_primary_screen_size(sw, sh) && sw > 0 && sh > 0) {
			w = sw;
			h = sh;
		} else if (!canvas_dims(w, h)) {
			return false;
		}
		break;
	}
	case amv_nested::ResMode::FollowWindow:
	default: {
		const uint32_t rw = s->rt_w.load(std::memory_order_relaxed);
		const uint32_t rh = s->rt_h.load(std::memory_order_relaxed);
		if (rw > 0 && rh > 0) {
			/* Snap up to a coarse quantum so a window resize keeps the same
			 * consumer key across most frames (see kFollowWindowQuantum). */
			w = ((rw + kFollowWindowQuantum - 1) / kFollowWindowQuantum) * kFollowWindowQuantum;
			h = ((rh + kFollowWindowQuantum - 1) / kFollowWindowQuantum) * kFollowWindowQuantum;
		} else if (!canvas_dims(w, h)) {
			/* Before the first draw, or an offscreen pass that never
			 * handed us a size: fall back to the canvas. */
			return false;
		}
		break;
	}
	}
	w = std::min(w, AmvInstanceCore::kConsumerMaxDim);
	h = std::min(h, AmvInstanceCore::kConsumerMaxDim);
	return w > 0 && h > 0;
}

const char *amv_src_get_name(void *)
{
	/* Not shown in the Add Source list (CAP_DISABLED); used only for logs. */
	return "AMV Instance (nested)";
}

void *amv_src_create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new AmvInstanceSource();
	s->self = source;
	load_settings(s, settings);
	return s;
}

void amv_src_destroy(void *data)
{
	delete static_cast<AmvInstanceSource *>(data);
}

void amv_src_update(void *data, obs_data_t *settings)
{
	load_settings(static_cast<AmvInstanceSource *>(data), settings);
}

uint32_t amv_src_get_width(void *data)
{
	uint32_t w = 0, h = 0;
	if (resolve_dims(static_cast<AmvInstanceSource *>(data), w, h))
		return w;
	return 1920; /* defensive fallback so letterbox math never divides by zero */
}

uint32_t amv_src_get_height(void *data)
{
	uint32_t w = 0, h = 0;
	if (resolve_dims(static_cast<AmvInstanceSource *>(data), w, h))
		return h;
	return 1080;
}

void amv_src_video_render(void *data, gs_effect_t *effect)
{
	(void)effect; /* CUSTOM_DRAW: OBS passes null; we drive the default effect. */
	auto *s = static_cast<AmvInstanceSource *>(data);
	if (!s || s->target_uuid.empty())
		return;

	uint32_t w = 0, h = 0;
	if (!resolve_dims(s, w, h))
		return;

	/* Resolve the target on the graphics thread via the snapshot (no registry
	 * lock). Missing target -> draw nothing; the cell falls to Lost through the
	 * provider's probe_health verdict. */
	AmvInstanceCore *target = multiview_pull_target_graphics(s->target_uuid);
	if (!target)
		return;

	/* Record demand so the target composes this (R, mode) picture, then sample
	 * its last completed FRONT. Both are lock-free graphics-thread map touches
	 * on the TARGET's consumer state; we never take its source_mutex_. */
	target->note_consumer_demand(w, h, s->mode);
	AmvInstanceCore::ConsumerFrame front = target->get_consumer_front(w, h, s->mode);
	if (!front.texture)
		return; /* no completed frame yet -> nothing to draw this frame */

	/* Blit the FRONT into the source's [0,width]x[0,height] space (draw_cells
	 * has already set up the ortho/viewport mapping that into the cell's video
	 * rect). Same default-effect sprite idiom as the window compose blit. */
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
	gs_effect_set_texture(image, front.texture);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(front.texture, 0, front.width, front.height);
}

void amv_src_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, amv_nested::kTargetUuidKey, "");
	obs_data_set_default_string(settings, amv_nested::kPictureModeKey, "full");
	obs_data_set_default_string(settings, amv_nested::kResModeKey,
				    amv_nested::res_mode_to_string(amv_nested::ResMode::FollowWindow));
	obs_data_set_default_string(settings, amv_nested::kManualResModeKey,
				    output_res_mode_to_str(OutputResolutionMode::CanvasBase));
	obs_data_set_default_int(settings, amv_nested::kManualCustomWKey, 1920);
	obs_data_set_default_int(settings, amv_nested::kManualCustomHKey, 1080);
}

} // namespace

void register_amv_instance_source()
{
	struct obs_source_info info = {};
	info.id = amv_nested::kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* CAP_DISABLED: invisible in OBS's Add Source list, still create-able by the
	 * AmvInstance provider. CUSTOM_DRAW: we own the draw (no default async/effect
	 * path). VIDEO only: no audio (cross-instance metering is out of scope). */
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	info.get_name = amv_src_get_name;
	info.create = amv_src_create;
	info.destroy = amv_src_destroy;
	info.update = amv_src_update;
	info.get_defaults = amv_src_get_defaults;
	info.get_width = amv_src_get_width;
	info.get_height = amv_src_get_height;
	info.video_render = amv_src_video_render;
	info.icon_type = OBS_ICON_TYPE_UNKNOWN;

	obs_register_source(&info);
}

void amv_instance_source_note_render_target(obs_source_t *src, uint32_t w, uint32_t h)
{
	if (!src)
		return;
	/* Re-check identity: draw_cells may call this on a fallback source that was
	 * substituted for a lost target (still an AmvInstance cell). Only our own
	 * type carries an AmvInstanceSource payload; anything else is a no-op. */
	const char *id = obs_obj_get_id(src);
	if (!id || std::string(id) != amv_nested::kSourceId)
		return;
	auto *s = static_cast<AmvInstanceSource *>(obs_obj_get_data(src));
	if (!s)
		return;
	s->rt_w.store(w, std::memory_order_relaxed);
	s->rt_h.store(h, std::memory_order_relaxed);
}

bool amv_instance_source_resolve_dims(obs_source_t *src, uint32_t &w, uint32_t &h)
{
	if (!src)
		return false;
	const char *id = obs_obj_get_id(src);
	if (!id || std::string(id) != amv_nested::kSourceId)
		return false;
	return resolve_dims(static_cast<AmvInstanceSource *>(obs_obj_get_data(src)), w, h);
}
