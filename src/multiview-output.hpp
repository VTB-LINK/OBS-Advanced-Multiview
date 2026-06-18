/*
OBS Advanced Multiview - multiview output layer (issue #11)

Backend-agnostic transmission of the composed multiview frame, independent
of OBS's source/scene system (Approach B). Each enabled backend (Spout, and
later NDI) picks its own output resolution + frame-rate divisor; backends that
resolve to the same dimensions share a single offscreen render. The manager
self-reconciles from the instance's InstanceOutputSettings every frame, so the
window/UI only has to persist config and keep the manager alive.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "multiview-instance.hpp"

#include <obs.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

/* One output protocol (Spout / NDI / ...). All methods run on the OBS
 * graphics thread. */
class IMultiviewOutputBackend {
public:
	virtual ~IMultiviewOutputBackend() = default;

	/* Stable identifier for logs, e.g. "spout" / "ndi". */
	virtual const char *kind() const = 0;

	/* Transmit one frame. The backend lazily (re)creates its sender to
	 * match `name` and the texture's dimensions/format, then sends `tex`
	 * (a GS_BGRA texture owned by the manager's texrender). The backend
	 * must not retain `tex` past the call: Spout copies it on the GPU; NDI
	 * reads it back to CPU here.
	 *
	 * `fpsDivisor` is this backend's frame-rate divisor (1=full, 2=half) so
	 * the backend can declare its true sent rate (OBS fps / divisor). Spout
	 * carries no frame-rate metadata and ignores it. */
	virtual void submit_frame(const std::string &name, gs_texture_t *tex, uint32_t w, uint32_t h,
				  int fpsDivisor) = 0;

	/* Release the sender and all GPU/OS resources. Called on the graphics
	 * thread. Safe to call when never started. */
	virtual void stop() = 0;

	/* True once a sender is live and has transmitted at least one frame. */
	virtual bool is_active() const = 0;
};

/* Owns one offscreen render target per unique output resolution and the active
 * backends for one multiview instance. */
class MultiviewOutputManager {
public:
	MultiviewOutputManager();
	~MultiviewOutputManager();

	MultiviewOutputManager(const MultiviewOutputManager &) = delete;
	MultiviewOutputManager &operator=(const MultiviewOutputManager &) = delete;

	/* Graphics-thread, once per frame. Reconcile backends against `cfg`
	 * (create/stop Spout per cfg.spout.enabled; NDI inert for now), then for
	 * each UNIQUE enabled output resolution that is due this frame: render the
	 * grid into that resolution's texrender via `draw(w,h)` (which paints the
	 * composition mapped to 0,0,w,h) and submit to each backend at that
	 * resolution. Per-backend fpsDivisor (1=full, 2=half) gates submit AND the
	 * render itself — a resolution with no backend due this frame is skipped
	 * entirely, so half-rate halves the compose cost, not just the send. */
	void render_all(const std::string &name, const InstanceOutputSettings &cfg,
			const std::function<void(int w, int h)> &draw);

	/* Stop all backends + destroy all texrenders. Caller must hold the OBS
	 * graphics context (used by apply_output_settings under obs_enter_graphics). */
	void teardown_locked();

	/* teardown_locked() wrapped in obs_enter_graphics — safe from the UI
	 * thread (window close / destroy). */
	void shutdown_graphics();

	/* Whether Spout output is even possible here. Reuses the existing Spout
	 * platform detection (Windows-only); the D3D11-renderer check is deferred
	 * to the backend on the graphics thread. */
	static bool spout_supported();

	/* Whether NDI output is possible here: the plugin was built with NDI
	 * support AND the NDI runtime DLL can be located/loaded. Used by the
	 * settings UI to enable/disable the NDI tab. */
	static bool ndi_supported();

private:
	enum class Kind { Spout, Ndi };

	/* One backend slot. `enabled` + resolved {w,h} + fpsDivisor are refreshed
	 * from cfg each frame by reconcile(); `frame` advances once per frame and
	 * drives the divisor. */
	struct BackendEntry {
		std::unique_ptr<IMultiviewOutputBackend> backend;
		bool enabled = false;
		uint32_t w = 0, h = 0;
		int fpsDivisor = 1;
		uint64_t frame = 0;
	};

	static uint64_t res_key(uint32_t w, uint32_t h) { return ((uint64_t)w << 32) | (uint64_t)h; }
	static bool backend_available(Kind k);
	static std::unique_ptr<IMultiviewOutputBackend> create_backend(Kind k);

	void reconcile(BackendEntry &e, const OutputBackendSettings &s, Kind kind);
	gs_texrender_t *get_texrender(uint64_t key);
	void render_one_resolution(const std::string &name, uint32_t w, uint32_t h,
				   const std::function<void(int w, int h)> &draw);

	BackendEntry spout_;
	BackendEntry ndi_; /* inert until the NDI backend lands (Phase 3) */
	std::map<uint64_t, gs_texrender_t *> texrenders_;
};
