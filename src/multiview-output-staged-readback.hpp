/*
OBS Advanced Multiview - staged GPU->CPU readback helper (issue #11 / #16)

Shared double-buffered (ping-pong) GPU->CPU staging readback used by the NDI and
DeckLink output backends. Both stage the composed multiview texture into a
staging surface, then map + hand the mapped BGRA pixels to a backend-specific
consumer (NDI: send_video; DeckLink: video_output_lock_frame + memcpy). This
helper owns the identical part - the two staging surfaces, the ping-pong index,
the resize/allocation logic and the deduplicated failure warnings - so each
backend keeps only its own tail (how a mapped frame is transmitted).

All methods run on the OBS graphics thread (the sole owner of the staging
surfaces). The helper itself touches no OBS output, no network and no lock: it
only stages/maps GPU memory and calls the caller-supplied consumer inline while
the surface is mapped (the DeckLink consumer takes its own video_t mutex, exactly
as before - that lock stays in the backend, not here).

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include <graphics/graphics.h>
#include <util/base.h>
#include <plugin-support.h>

#include <cstdint>

/* Double-buffered GPU->CPU staging readback. Graphics-thread only.
 *
 * Owns two BGRA staging surfaces. In double-buffer mode they are ping-ponged
 * (stage frame N into one, map+consume frame N-1 from the other, so the map
 * never stalls the graphics thread, at the cost of +1 frame of latency); in
 * synchronous mode only stage_[0] is used (stage+map+consume the same frame,
 * lowest latency but the map blocks on the GPU copy). `logTag` is a
 * process-lifetime string literal used verbatim as the log prefix, so each
 * backend keeps its own tag. */
class StagedReadback {
public:
	explicit StagedReadback(const char *logTag) : logTag_(logTag) {}

	/* Stage `tex` and, when a prior frame is ready (double-buffer) or the
	 * current one maps (synchronous), invoke consume(data, linesize) with the
	 * mapped BGRA pixels. Returns true iff a frame was actually mapped AND
	 * consumed on this call - the caller uses the return to set its "active" /
	 * "has transmitted a frame" state (the NDI backend does; DeckLink ignores it
	 * and tracks liveness on its own output state).
	 *
	 * consume has signature void(uint8_t *data, uint32_t linesize); the frame
	 * width/height are the caller's (captured in the lambda), matching the
	 * pre-helper code where submit_frame passed them straight through to
	 * send_video / feed_frame.
	 *
	 * `doubleBuffer` is the live per-frame setting. When it flips relative to the
	 * previous submit() the ping-pong is reset first (idx_=0, havePrev_=false) so
	 * a mode change cannot map/consume a stale or half-staged buffer - this
	 * absorbs the reset the NDI backend used to perform in set_double_buffer().
	 * Callers guard tex != null and w,h != 0 before calling (both did pre-helper),
	 * so this does not re-check them. */
	template<class Consume>
	bool submit(gs_texture_t *tex, uint32_t w, uint32_t h, bool doubleBuffer, Consume &&consume)
	{
		/* Mode flip: reset the ping-pong before staging, so the first frame
		 * after a flip only stages (no stale/half-staged send). This is the old
		 * NDI set_double_buffer() reset (stage_idx_=0, stage_have_prev_=false),
		 * relocated here and keyed off the observed mode change. */
		if (doubleBuffer != prevDoubleBuffer_) {
			idx_ = 0;
			havePrev_ = false;
			prevDoubleBuffer_ = doubleBuffer;
		}

		if (!ensure(w, h))
			return false;

		if (doubleBuffer) {
			/* Double-buffered: queue frame N's copy into stage_[idx_]
			 * (gs_stage_texture is async), then map+consume the OTHER surface
			 * staged on frame N-1 - the GPU has had a full frame to finish it,
			 * so the map doesn't stall the graphics thread. Advance the
			 * ping-pong whether or not the map succeeds. */
			gs_stage_texture(stage_[idx_], tex);
			const int prev = 1 - idx_;
			bool consumed = false;
			if (havePrev_) {
				uint8_t *data = nullptr;
				uint32_t linesize = 0;
				if (gs_stagesurface_map(stage_[prev], &data, &linesize)) {
					consume(data, linesize);
					gs_stagesurface_unmap(stage_[prev]);
					consumed = true;
					warnedMap_ = false;
				} else if (!warnedMap_) {
					obs_log(LOG_WARNING, "%s gs_stagesurface_map failed", logTag_);
					warnedMap_ = true;
				}
			}
			idx_ = prev;
			havePrev_ = true;
			return consumed;
		}

		/* Synchronous: stage + map + consume the SAME frame. The map blocks
		 * until the GPU copy lands (~1 frame stall on a slow GPU) but keeps the
		 * output lowest-latency and A/V in sync. */
		gs_stage_texture(stage_[0], tex);
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(stage_[0], &data, &linesize)) {
			if (!warnedMap_) {
				obs_log(LOG_WARNING, "%s gs_stagesurface_map failed", logTag_);
				warnedMap_ = true;
			}
			return false;
		}
		consume(data, linesize);
		gs_stagesurface_unmap(stage_[0]);
		warnedMap_ = false;
		return true;
	}

	/* Discard the frame staged for the next map WITHOUT freeing the surfaces, so
	 * the next double-buffered submit() re-primes (stages only, no consume)
	 * instead of mapping/consuming a stale copy. Surfaces stay allocated. Used by
	 * the NDI backend when a receiver disconnects to restart the ping-pong
	 * cleanly on reconnect - the exact old `stage_have_prev_ = false` there. */
	void restart() { havePrev_ = false; }

	/* Free both staging surfaces and reset the ping-pong. Idempotent; safe when
	 * never allocated. Deliberately does NOT clear the once-per-episode warning
	 * flags (see reset_warnings()): a resize is routed through here by ensure()
	 * and must not re-arm the map/create warnings mid-episode, matching the old
	 * destroy_stages(). */
	void destroy()
	{
		for (auto *&s : stage_) {
			if (s) {
				gs_stagesurface_destroy(s);
				s = nullptr;
			}
		}
		w_ = h_ = 0;
		idx_ = 0;
		/* No completed copy survives a teardown/resize, so the next frame must
		 * restart the ping-pong rather than map stale / wrong-sized pixels. */
		havePrev_ = false;
	}

	/* Re-arm the once-per-episode failure warnings. Called from the backends'
	 * stop() (a stop/start is an episode boundary), kept separate from destroy()
	 * because a resize also drives destroy() but must not re-arm them. */
	void reset_warnings()
	{
		warnedMap_ = false;
		warnedStage_ = false;
	}

private:
	/* Keep both surfaces allocated at w x h, (re)creating on a size change. The
	 * second surface is unused in synchronous mode but lets a flip to
	 * double-buffer proceed without a recreate. One extra BGRA staging surface is
	 * a few MB - negligible. */
	bool ensure(uint32_t w, uint32_t h)
	{
		if (stage_[0] && stage_[1] && w_ == w && h_ == h)
			return true;

		destroy();

		stage_[0] = gs_stagesurface_create(w, h, GS_BGRA);
		stage_[1] = gs_stagesurface_create(w, h, GS_BGRA);
		if (!stage_[0] || !stage_[1]) {
			destroy();
			if (!warnedStage_) {
				obs_log(LOG_WARNING, "%s gs_stagesurface_create(%ux%u) failed", logTag_, w, h);
				warnedStage_ = true;
			}
			return false;
		}

		w_ = w;
		h_ = h;
		warnedStage_ = false;
		return true;
	}

	gs_stagesurf_t *stage_[2] = {nullptr, nullptr};
	uint32_t w_ = 0, h_ = 0;
	int idx_ = 0;                  /* buffer to stage INTO this frame (double-buffer) */
	bool havePrev_ = false;        /* stage_[1 - idx_] holds last frame's completed copy */
	bool prevDoubleBuffer_ = true; /* last submit()'s mode, to detect a flip */
	bool warnedMap_ = false;
	bool warnedStage_ = false;
	const char *logTag_; /* process-lifetime string literal (log prefix) */
};
