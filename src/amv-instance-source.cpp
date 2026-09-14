/*
OBS Advanced Multiview - hidden nested-source obs type `amv_instance_source`
(issue #20 P2).

Registers a real obs_source that renders ANOTHER AMV instance's already-published
composited picture as a per-cell source. It carries OBS_SOURCE_CAP_DISABLED so it
never appears in OBS's "Add Source" list, yet the AmvInstance signal provider can
still create it privately (obs_source_create_private). The rest of the runtime
(refresh_cell, the health supervisor, draw_cells) treats it as an ordinary
external private source — no special casing there.

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

Resolution R (P2): fixed to the OBS canvas base size (design §3.4 "manual =
canvas"); the three-tier follow-screen / follow-window modes are P3. Picture mode
(P2): always the full composition; the grid-only switch is P3. get_width/get_height
always report R so draw_cells' letterbox math is stable regardless of whether the
target currently has a frame (target-missing -> Lost is decided by the AmvInstance
provider's probe_health, not by a shrinking source size).

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "multiview-instance.hpp"
#include "multiview-window.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <algorithm>
#include <string>

namespace {

struct AmvInstanceSource {
	obs_source_t *self = nullptr;
	/* Target instance UUID + picture mode, parsed once at create. Our private
	 * source is never obs_source_update'd after creation (a cell edit recreates
	 * it via refresh_cell), so these are effectively immutable and safe to read
	 * lock-free on the graphics thread. update() re-parses defensively. */
	std::string target_uuid;
	AmvInstanceCore::ConsumerPictureMode mode = AmvInstanceCore::ConsumerPictureMode::Full;
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
}

/* Resolve R = OBS canvas base size (P2 fixed policy). Returns false if the video
 * pipeline is unavailable or reports a zero canvas. */
bool canvas_dims(uint32_t &w, uint32_t &h)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return false;
	/* Clamp to the same bound the consumer compose applies in make_consumer_key,
	 * so get_width/get_height and the demanded/published texture size agree even on
	 * a canvas larger than the cap (issue #20). */
	w = std::min(ovi.base_width, AmvInstanceCore::kConsumerMaxDim);
	h = std::min(ovi.base_height, AmvInstanceCore::kConsumerMaxDim);
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

uint32_t amv_src_get_width(void *)
{
	uint32_t w = 0, h = 0;
	if (canvas_dims(w, h))
		return w;
	return 1920; /* defensive fallback so letterbox math never divides by zero */
}

uint32_t amv_src_get_height(void *)
{
	uint32_t w = 0, h = 0;
	if (canvas_dims(w, h))
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
	if (!canvas_dims(w, h))
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
