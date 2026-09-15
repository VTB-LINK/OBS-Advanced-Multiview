/*
OBS Advanced Multiview - per-instance core: nested-AMV-source consumer service
(issue #20 P1).

A core publishes its own composited picture as a double-buffered GPU texture,
keyed by (width, height, picture mode), so OTHER instances (or itself) can sample
it as a per-cell source WITHOUT synchronously re-entering this core's
composition. Consumers read the last published FRONT while the current frame is
composed into BACK — the "read the previous frame" model that makes cycles, self-
reference and deep chains safe by construction (no recursion, no cycle detection,
no render-depth cap). See docs/issue-20-nested-amv-source-design.md §3.1 / §3.5.

Threading contract (AGENTS §2): all three public entry points here
(note_consumer_demand, compose_consumer_frames, get_consumer_front) run ONLY on
the OBS graphics thread and are the SOLE accessors of consumer_targets_, so that
map deliberately needs NO source_mutex_. A consumer's sample path must never take
a SECOND core's source_mutex_ (never nest two cores' mutexes), which is exactly
why the demand/read calls here are lock-free map touches. draw_cells (invoked by
compose_one_consumer_target) still takes THIS core's own source_mutex_ internally
— the one legal graphics-lock -> source_mutex_ nesting.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-logging.hpp"

#include <obs.hpp>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>
#include <string>
#include <utility>

AmvInstanceCore::ConsumerKey AmvInstanceCore::make_consumer_key(uint32_t w, uint32_t h, ConsumerPictureMode mode) const
{
	/* Clamp the requested size identically for demand and read so both resolve
	 * to the same key. The clamp is a defensive ceiling against an absurd
	 * request; a real consumer requests a cell/screen-sized picture. */
	ConsumerKey key;
	key.w = std::min(w, kConsumerMaxDim);
	key.h = std::min(h, kConsumerMaxDim);
	key.mode = mode;
	return key;
}

void AmvInstanceCore::note_consumer_demand(uint32_t w, uint32_t h, ConsumerPictureMode mode)
{
	/* Graphics thread only. Cheap: record that this (w, h, mode) picture is
	 * wanted this frame; compose_consumer_frames() does the actual GPU work
	 * later this frame. */
	if (w == 0 || h == 0)
		return;
	const ConsumerKey key = make_consumer_key(w, h, mode);
	auto it = consumer_targets_.find(key);
	if (it == consumer_targets_.end()) {
		/* Bounded: never let a runaway set of requested sizes (e.g. an
		 * un-quantized follow-window request in a later phase) make one core
		 * allocate unbounded VRAM. Existing keys keep updating; new keys are
		 * dropped at the cap (the consumer degrades to Lost for that size). */
		if (consumer_targets_.size() >= kMaxConsumerTargets)
			return;
		it = consumer_targets_.emplace(key, ConsumerTarget{}).first;
	}
	it->second.last_demand_ns = os_gettime_ns();
}

void AmvInstanceCore::compose_one_consumer_target(const ConsumerKey &key, ConsumerTarget &tgt)
{
	/* Graphics thread, graphics context already active (driven from the main-
	 * rendered callback, like render_output_only). Composes THIS target into its
	 * back buffer and swaps it to front. */
	const uint32_t w = key.w;
	const uint32_t h = key.h;
	if (w == 0 || h == 0)
		return;

	if (!tgt.back)
		tgt.back = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!tgt.back)
		return;

	/* Recompute the layout for this target's size from the current layout_ (the
	 * same unsynchronized read the output pass uses — see render_output_only).
	 * Cheap for the one or two keys a core publishes; a later phase can cache. */
	consumer_engine_.set_layout(layout_);
	consumer_engine_.set_viewport((int)w, (int)h);
	consumer_engine_.compute();

	gs_texrender_reset(tgt.back);
	if (!gs_texrender_begin(tgt.back, w, h))
		return;

	/* Ambient viewport/projection matching the texrender pixel space — mirrors
	 * MultiviewOutputManager::render_one_resolution so helpers that draw against
	 * the ambient projection (e.g. render_safe_area in Full mode) are correct. */
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	struct vec4 clear_color;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	/* diag=false: this is an offscreen pass (like the output pass); it must not
	 * inflate the per-cell render_calls counter or the one-shot [fill] logs. */
	draw_cells(consumer_engine_.cells(), 0, 0, (int)w, (int)h, /*diag=*/false, key.mode);

	gs_texrender_end(tgt.back);

	/* Publish: swap back -> front. The old front becomes next frame's back and
	 * is only reset at that next swap point, so a consumer reading the (now)
	 * front this frame never observes a reset in progress — including a self-
	 * referencing instance reading its own previous front while this composes. */
	std::swap(tgt.front, tgt.back);
	tgt.width = w;
	tgt.height = h;
	tgt.front_valid = true;
}

void AmvInstanceCore::compose_consumer_frames()
{
	/* Driven once per frame on the graphics thread. No demand -> empty map ->
	 * no-op, so an un-referenced core costs nothing here (the P1 steady state:
	 * nothing calls note_consumer_demand until P2 wires the nested source). */
	if (consumer_targets_.empty())
		return;

	const uint64_t now = os_gettime_ns();

	/* GC targets nobody has demanded within the TTL: free their texrenders (the
	 * graphics context is active on this callback). */
	for (auto it = consumer_targets_.begin(); it != consumer_targets_.end();) {
		if (now - it->second.last_demand_ns > kConsumerGcTtlNs) {
			if (it->second.front)
				gs_texrender_destroy(it->second.front);
			if (it->second.back)
				gs_texrender_destroy(it->second.back);
			it = consumer_targets_.erase(it);
		} else {
			++it;
		}
	}

	/* Compose the actively-demanded targets. A target that is idle but still
	 * within the (longer) GC TTL keeps its last front as a warm cache. */
	for (auto &kv : consumer_targets_) {
		if (now - kv.second.last_demand_ns > kConsumerComposeWindowNs)
			continue;
		compose_one_consumer_target(kv.first, kv.second);
	}
}

AmvInstanceCore::ConsumerFrame AmvInstanceCore::get_consumer_front(uint32_t w, uint32_t h, ConsumerPictureMode mode)
{
	/* Graphics thread only; deliberately takes no lock (see file header).
	 *
	 * Broadcast-grade "never falsely Lost": prefer the exact (w, h, mode) front,
	 * but on an exact miss fall back to the largest completed front of the SAME
	 * picture mode (any size). A consumer's requested size drifts for a frame or
	 * two during churn — a freshly (re)created source before its first render-
	 * target hand-off (canvas fallback), a window resize crossing the follow-
	 * window quantum, the consumer_targets_ cap dropping a just-requested key, or
	 * a Full<->Grid switch — and the exact key composes within a frame and takes
	 * over. All of this core's fronts share the canvas aspect and the consumer
	 * stretches whatever size it gets to fill its advertised size R (a clean
	 * resample, not a distortion), so returning a same-mode front keeps the nested
	 * cell showing the picture through that churn instead of flashing SIGNAL LOST.
	 * Only when this core publishes NO completed front for the mode at all does the
	 * consumer fall through to Lost. */
	ConsumerFrame out;
	if (w == 0 || h == 0)
		return out;

	const ConsumerKey key = make_consumer_key(w, h, mode);
	auto it = consumer_targets_.find(key);
	if (it != consumer_targets_.end() && it->second.front_valid && it->second.front) {
		out.texture = gs_texrender_get_texture(it->second.front);
		out.width = it->second.width;
		out.height = it->second.height;
		return out;
	}

	/* Exact miss: the largest completed front of the same mode (most detail when
	 * scaled into the cell). Read-only scan of a map of at most kMaxConsumerTargets
	 * entries; never composes, never touches back buffers. */
	const ConsumerTarget *best = nullptr;
	uint64_t best_area = 0;
	for (auto &kv : consumer_targets_) {
		if (kv.first.mode != mode || !kv.second.front_valid || !kv.second.front)
			continue;
		const uint64_t area = (uint64_t)kv.second.width * (uint64_t)kv.second.height;
		if (!best || area > best_area) {
			best = &kv.second;
			best_area = area;
		}
	}
	if (best) {
		out.texture = gs_texrender_get_texture(best->front);
		out.width = best->width;
		out.height = best->height;
		return out;
	}

	/* True miss: this core has no completed front for `mode` at all (target not
	 * composing this mode yet, or just deleted) -> the consumer falls through to
	 * Lost. Detailed-logs diagnostic (gated): log the requested (w, h, mode)
	 * against every key held so the Lost can be traced. Throttled to one line per
	 * 500 ms so a per-frame miss cannot flood the log. */
	static uint64_t s_diag_ns = 0;
	const uint64_t now = os_gettime_ns();
	if (now - s_diag_ns > 500'000'000ULL) {
		s_diag_ns = now;
		std::string keys;
		for (auto &kv : consumer_targets_)
			keys += "(" + std::to_string(kv.first.w) + "x" + std::to_string(kv.first.h) + "," +
				(kv.first.mode == ConsumerPictureMode::GridOnly ? "grid" : "full") +
				",fv=" + (kv.second.front_valid ? "1" : "0") + ") ";
		amv_log_detailed(LOG_INFO, "[consumer] %s get_consumer_front MISS want=(%ux%u,%s) keys=[%s]",
				 log_prefix().c_str(), w, h, mode == ConsumerPictureMode::GridOnly ? "grid" : "full",
				 keys.c_str());
	}
	return out;
}

void AmvInstanceCore::release_consumer_targets()
{
	/* Called from the destructor on the UI thread AFTER the core has been removed
	 * from the render driver (four-phase teardown), so no compose is in flight
	 * and no future compose will reference these. gs_texrender_destroy needs the
	 * graphics context, which the destructor does not already hold. */
	if (consumer_targets_.empty())
		return;
	obs_enter_graphics();
	for (auto &kv : consumer_targets_) {
		if (kv.second.front)
			gs_texrender_destroy(kv.second.front);
		if (kv.second.back)
			gs_texrender_destroy(kv.second.back);
	}
	obs_leave_graphics();
	consumer_targets_.clear();
}
