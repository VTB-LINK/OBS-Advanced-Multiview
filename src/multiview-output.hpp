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

	/* Issue #10 (#2): keep the sender alive/discoverable WITHOUT transmitting a
	 * frame. Called on the graphics thread every frame for every enabled backend,
	 * before any compose happens. NDI (re)creates its sender here so downstream
	 * receivers can still find and connect to it while we skip frames; Spout has
	 * no discovery cost and ignores it. Cheap when the sender already matches. */
	virtual void prepare(const std::string &name) { (void)name; }

	/* Issue #10 (#2): does this backend actually need a composed frame right now?
	 * Returns false to let the manager SKIP the GPU compose + submit for this
	 * backend's resolution this frame. NDI returns false when no receiver is
	 * connected (so an idle sender costs zero compose/readback/encode); Spout
	 * can't know its consumers and always wants the frame. Called on the graphics
	 * thread after prepare(). */
	virtual bool wants_frame() { return true; }

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

	/* Configure audio transmission from the backend's settings (audio source
	 * track). Called on the graphics thread during reconcile. Backends without
	 * an audio path (Spout) ignore it; NDI (re)connects its OBS audio capture
	 * when the selected track changes. */
	virtual void configure_audio(const OutputBackendSettings &cfg) { (void)cfg; }

	/* Issue #10: toggle GPU->CPU readback double-buffering. Called on the
	 * graphics thread during reconcile with the user's global setting. Only the
	 * NDI backend implements it (Spout has no readback). */
	virtual void set_double_buffer(bool enabled) { (void)enabled; }

	/* Issue #16 (M1): report the backend's authoritative compose size, if it has
	 * one. A DeckLink backend locked to a hardware mode raster overrides the
	 * persisted customWidth/customHeight snapshot once Running, so a stale or
	 * mismatched snapshot can't make the manager compose at the wrong size and
	 * silently drop every frame. Returns false (default) to let the manager use
	 * resolve_output_dimensions. Called on the graphics thread during reconcile. */
	virtual bool compose_size(uint32_t &w, uint32_t &h) const
	{
		(void)w;
		(void)h;
		return false;
	}

	/* Release the sender and all GPU/OS resources. Safe to call when never
	 * started.
	 *
	 * Thread contract (authoritative — every backend's stop() must obey this):
	 * stop() may run EITHER on the graphics thread (MultiviewOutputManager::
	 * reconcile(), every frame) OR on the main thread while the OBS graphics lock
	 * is held (teardown_locked via apply_output_settings / shutdown_graphics, or
	 * a backend destructor reached during that teardown). In BOTH cases the
	 * graphics lock is held, so stop() must NEVER inline a blocking, joining, or
	 * network-flushing teardown — e.g. NDIlib_send_destroy waiting on pending
	 * async frames, obs_output_stop joining a capture thread, audio_output_disconnect
	 * that could wait on an in-flight callback that itself blocks — because that
	 * would stall the live program render. Only fast GPU/OS resource release
	 * (staging surfaces, etc.) may run inline; defer the heavy teardown to the UI
	 * thread via QMetaObject::invokeMethod(qApp, ..., Qt::QueuedConnection), moving
	 * the handles it needs into a self-owning, this-free closure so the backend can
	 * be destroyed the moment stop() returns (a queued invokeMethod always posts a
	 * QMetaCallEvent, so the graphics lock is released before the closure runs, even
	 * when stop() is already on the main thread).
	 *
	 * C2: post it as a queued meta-call (not a QTimer/timer event) so the OBS exit /
	 * module-unload path can flush any still-pending teardown before qApp and the
	 * NDI runtime are destroyed — drain_deferred_output_teardowns() (plugin-main.cpp)
	 * runs QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall) after the cores
	 * are torn down. A dropped closure would leak the NDI sender / DeckLink output
	 * (and destroy the NDI runtime with a sender still alive). */
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
			const std::function<void(int w, int h)> &draw, bool ndiDoubleBuffer = true);

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

	/* Whether DeckLink output is possible here: the plugin was built with
	 * DeckLink support AND OBS's "decklink_output" type is registered
	 * (obs_get_output_flags != 0; the OBS DeckLink plugin is present). Used by
	 * the settings UI to enable/disable the DeckLink tab. */
	static bool decklink_supported();

private:
	enum class Kind { Spout, Ndi, Decklink };

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
	static const char *kind_name(Kind k);

	void reconcile(BackendEntry &e, const OutputBackendSettings &s, Kind kind);
	gs_texrender_t *get_texrender(uint64_t key);
	void render_one_resolution(const std::string &name, uint32_t w, uint32_t h,
				   const std::function<void(int w, int h)> &draw);

	BackendEntry spout_;
	BackendEntry ndi_;
	BackendEntry decklink_; /* issue #16 */
	std::map<uint64_t, gs_texrender_t *> texrenders_;
};
