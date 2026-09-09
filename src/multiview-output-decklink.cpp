/*
OBS Advanced Multiview - DeckLink output backend (issue #16)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#ifdef AMV_ENABLE_DECKLINK_OUTPUT

#include "multiview-output-decklink.hpp"
#include "multiview-output-staged-readback.hpp"
#include "amv-frontend-cache.hpp"
#include "amv-logging.hpp"

#include <obs.h>
#include <obs.hpp>
#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/audio-io.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QObject>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace {

/* After a failed start (FPS mismatch, device busy, invalid mode) wait this long
 * before the graphics thread re-dispatches a create task, so a persistent
 * failure can't spam the UI queue + logs every frame. */
constexpr uint64_t kCooldownNs = 3ULL * 1000000000ULL; /* 3 s */

/* Internal lifecycle of the OBS output / video_t / hardware (distinct from the
 * C++ lifetime of the backend object, which exists while the backend is
 * enabled). Idle -> Starting (UI create task in flight) -> Running; any teardown
 * returns to Idle. */
enum class DeckState { Idle, Starting, Running };

/* One live output unit: the decklink output, the BGRA video_t feeding it, an
 * optional self-owned silent audio_t (audioMode == None), the mode raster, and
 * the backing string for video_output_info.name (libobs stores the pointer, so
 * the string must outlive the video_t — it lives here and dies with the unit).
 *
 * Ownership: built on the UI thread, installed into SharedState under its mutex,
 * and destroyed ONLY via close_output_instance on the UI thread (never in a
 * destructor — close order matters and must not run on the graphics thread). */
struct OutputInstance {
	obs_output_t *out = nullptr;
	video_t *video = nullptr;
	audio_t *silent_audio = nullptr; /* owned iff non-null (close on teardown) */
	std::string video_name;
	uint32_t w = 0, h = 0;
};

/* Cross-thread state shared between the backend (graphics thread) and in-flight
 * UI tasks. Held by a shared_ptr so a create task that outlives the backend
 * (backend torn down while Starting) still has a valid object to report into /
 * self-clean against. A single mutex guards every field below. */
struct SharedState {
	std::mutex mtx;
	std::unique_ptr<OutputInstance> inst; /* installed output; null unless Running */
	DeckState state = DeckState::Idle;
	uint64_t cooldown_until_ns = 0;
	bool cancelled = false;   /* backend stop()ed: create task must self-clean, no more work */
	bool unavailable = false; /* decklink_output not registered: stop retrying */

	/* Fast, lock-free gate for the per-frame hot path (wants_frame /
	 * submit_frame). Authoritative frame feeding still takes mtx + null-checks
	 * inst; this only avoids wasted GPU work when not running. */
	std::atomic<bool> active{false};
};

/* Desired DeckLink config snapshot (graphics-thread-only on the backend; copied
 * into a create task). Any change triggers a full output restart. */
struct DeckCfg {
	std::string deviceHash;
	long long modeId = 0;
	int keyer = 0;
	bool forceSdr = false;
	OutputAudioMode audioMode = OutputAudioMode::FollowStreaming;
	int audioTrack = 1;

	bool operator==(const DeckCfg &o) const
	{
		return deviceHash == o.deviceHash && modeId == o.modeId && keyer == o.keyer && forceSdr == o.forceSdr &&
		       audioMode == o.audioMode && audioTrack == o.audioTrack;
	}
	bool operator!=(const DeckCfg &o) const { return !(*this == o); }
};

struct CreateParam {
	std::shared_ptr<SharedState> sh;
	DeckCfg cfg;
	std::string uid; /* unique suffix for the output/video_t name */
};

/* ---- UI-thread helpers (run only on OBS_TASK_UI) ---- */

/* Close one output unit in the dependency-safe order (§3.2): stop + release the
 * output FIRST (this joins the decklink capture thread, which detaches from our
 * video_t), THEN close the video_t (now nobody reads it), THEN the silent audio.
 * obs_output_stop/release are non-blocking-unsafe (they join/Deactivate), so
 * this must run on the UI thread, never the graphics thread. */
void close_output_instance(std::unique_ptr<OutputInstance> inst)
{
	if (!inst)
		return;
	if (inst->out) {
		obs_output_stop(inst->out);
		obs_output_release(inst->out);
	}
	if (inst->video)
		video_output_close(inst->video);
	if (inst->silent_audio)
		audio_output_close(inst->silent_audio);
	/* inst (and its video_name backing string) freed here, after the video_t
	 * is closed. */
}

/* Mark the shared state Idle with a retry cooldown after a failed create. */
void mark_idle_cooldown(SharedState &sh)
{
	std::lock_guard<std::mutex> lk(sh.mtx);
	sh.state = DeckState::Idle;
	sh.active.store(false);
	sh.cooldown_until_ns = os_gettime_ns() + kCooldownNs;
}

/* Digital-silence generator for the None audio mode. libobs pre-zeroes the mix
 * buffers before each call, so emitting them verbatim (return true) produces
 * continuous silence at the output's cadence — which keeps the decklink audio
 * path fed (AV output requires a non-null audio source). */
bool silent_audio_input(void *, uint64_t, uint64_t end_ts, uint64_t *new_ts, uint32_t, struct audio_output_data *)
{
	*new_ts = end_ts;
	return true;
}

bool open_silent_audio(audio_t **out)
{
	struct audio_output_info oi = {};
	oi.name = "amv-decklink-silent";
	struct obs_audio_info aoi;
	if (obs_get_audio_info(&aoi)) {
		oi.samples_per_sec = aoi.samples_per_sec;
		oi.speakers = aoi.speakers;
	} else {
		oi.samples_per_sec = 48000;
		oi.speakers = SPEAKERS_STEREO;
	}
	oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
	oi.input_callback = silent_audio_input;
	oi.input_param = nullptr;
	return audio_output_open(out, &oi) == AUDIO_OUTPUT_SUCCESS;
}

/* H2: true iff `modeId` is one of the output modes `deviceHash` currently offers
 * at the canvas frame rate. decklink_output_create dereferences the
 * DeckLinkDeviceMode FindOutputMode returns WITHOUT a null check
 * (obs-studio/plugins/decklink/decklink-output.cpp:32-36), so a modeId absent
 * from the device's map (stale / cross-machine / hand-edited) crashes the OBS
 * process from inside obs_output_create. We drive OBS's own decklink_output
 * property machinery (exactly as the settings dialog does): query properties,
 * write the device into a scratch obs_data, fire the device's modified callback
 * (which fills the fps-filtered mode list), then scan the items. UI-thread only
 * (obs_properties_* touch the module's global device enumerator). */
bool decklink_mode_valid(const std::string &deviceHash, long long modeId)
{
	if (deviceHash.empty())
		return false;

	obs_properties_t *props = obs_get_output_properties("decklink_output");
	if (!props)
		return false;

	bool found = false;
	obs_property_t *deviceProp = obs_properties_get(props, "device_hash");
	obs_property_t *modeProp = obs_properties_get(props, "mode_id");
	if (deviceProp && modeProp) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "device_hash", deviceHash.c_str());
		/* Runs decklink_output_device_changed: fills mode_id filtered by fps. */
		obs_property_modified(deviceProp, settings);

		const size_t count = obs_property_list_item_count(modeProp);
		for (size_t i = 0; i < count; i++) {
			if (obs_property_list_item_int(modeProp, i) == modeId) {
				found = true;
				break;
			}
		}
	}

	obs_properties_destroy(props);
	return found;
}

/* UI thread: build + start the output, then install it into the shared state
 * (unless the backend was torn down meanwhile, in which case self-clean). Every
 * early-out closes any partially built unit and sets a retry cooldown. */
void create_task(void *param)
{
	std::unique_ptr<CreateParam> p(static_cast<CreateParam *>(param));
	SharedState &sh = *p->sh;
	const DeckCfg &cfg = p->cfg;

	/* obs_output_create returns a lazy object even for an unregistered id, so
	 * probe the registered flags (the manager already gates on this; guard
	 * again here). */
	if (obs_get_output_flags("decklink_output") == 0) {
		std::lock_guard<std::mutex> lk(sh.mtx);
		sh.state = DeckState::Idle;
		sh.active.store(false);
		sh.unavailable = true;
		return;
	}

	/* H2: mode_id is a hard precondition for obs_output_create. 0 is the
	 * "empty mode combo" default; any value absent from the device's current
	 * mode map crashes decklink_output_create (null DeckLinkDeviceMode deref).
	 * Refuse + cooldown rather than create. */
	if (cfg.modeId == 0) {
		obs_log(LOG_WARNING, "[multiview-output/decklink] mode_id unset (0); refusing to create");
		mark_idle_cooldown(sh);
		return;
	}
	if (!decklink_mode_valid(cfg.deviceHash, cfg.modeId)) {
		obs_log(LOG_WARNING,
			"[multiview-output/decklink] mode_id %lld not valid for this device/fps; refusing to create",
			cfg.modeId);
		mark_idle_cooldown(sh);
		return;
	}

	auto inst = std::make_unique<OutputInstance>();
	inst->video_name = "amv-decklink-" + p->uid;

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "device_hash", cfg.deviceHash.c_str());
	obs_data_set_int(settings, "mode_id", cfg.modeId);
	obs_data_set_int(settings, "keyer", cfg.keyer);
	obs_data_set_bool(settings, "force_sdr", cfg.forceSdr);

	inst->out = obs_output_create("decklink_output", (inst->video_name + "-out").c_str(), settings, nullptr);
	if (!inst->out) {
		obs_log(LOG_WARNING, "[multiview-output/decklink] obs_output_create failed");
		mark_idle_cooldown(sh);
		return;
	}

	/* The mode's exact raster is set by decklink_output_create as the output's
	 * video conversion when device+mode resolve; NULL => invalid selection on
	 * this machine. We open our video_t at that raster so the manager composes
	 * and feeds at native SDI size (zero scaling). */
	const struct video_scale_info *conv = obs_output_get_video_conversion(inst->out);
	if (!conv || conv->width == 0 || conv->height == 0) {
		obs_log(LOG_WARNING, "[multiview-output/decklink] invalid device/mode (no raster)");
		close_output_instance(std::move(inst));
		mark_idle_cooldown(sh);
		return;
	}
	inst->w = conv->width;
	inst->h = conv->height;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || ovi.fps_num == 0 || ovi.fps_den == 0) {
		close_output_instance(std::move(inst));
		mark_idle_cooldown(sh);
		return;
	}

	struct video_output_info voi = {};
	voi.name = inst->video_name.c_str();
	voi.format = VIDEO_FORMAT_BGRA;
	voi.width = inst->w;
	voi.height = inst->h;
	voi.fps_num = ovi.fps_num;
	voi.fps_den = ovi.fps_den;
	voi.cache_size = 4;
	/* M3: match the output's own conversion target (decklink_output picks
	 * 2100_PQ for an HDR device unless force_sdr, else 709) so SDR and HDR alike
	 * feed at the destination colorspace with zero per-frame conversion + correct
	 * color. Fall back to 709/FULL when the output left them at the enum default. */
	voi.colorspace = (conv->colorspace == VIDEO_CS_DEFAULT) ? VIDEO_CS_709 : conv->colorspace;
	voi.range = (conv->range == VIDEO_RANGE_DEFAULT) ? VIDEO_RANGE_FULL : conv->range;
	if (video_output_open(&inst->video, &voi) != VIDEO_OUTPUT_SUCCESS || !inst->video) {
		obs_log(LOG_WARNING, "[multiview-output/decklink] video_output_open failed");
		inst->video = nullptr;
		close_output_instance(std::move(inst));
		mark_idle_cooldown(sh);
		return;
	}

	/* Resolve the audio the output pulls (OBS_OUTPUT_AV requires audio != NULL). */
	audio_t *audio = nullptr;
	size_t mixerIdx = 0;
	if (cfg.audioMode == OutputAudioMode::None) {
		if (!open_silent_audio(&inst->silent_audio) || !inst->silent_audio) {
			obs_log(LOG_WARNING, "[multiview-output/decklink] silent audio open failed");
			close_output_instance(std::move(inst));
			mark_idle_cooldown(sh);
			return;
		}
		audio = inst->silent_audio;
		mixerIdx = 0;
	} else {
		audio = obs_get_audio();
		if (!audio) {
			close_output_instance(std::move(inst));
			mark_idle_cooldown(sh);
			return;
		}
		if (cfg.audioMode == OutputAudioMode::ManualTrack) {
			int t = cfg.audioTrack;
			if (t < 1)
				t = 1;
			else if (t > 6)
				t = 6;
			mixerIdx = (size_t)(t - 1);
		} else {
			/* FollowStreaming: lowest set mixer bit of the streaming output,
			 * from the main-thread frontend cache (safe on this UI thread;
			 * never call obs_frontend_* off the main thread). */
			uint32_t mask = amv_frontend::streaming_mixers();
			for (int i = 0; i < 6; i++) {
				if (mask & (1u << i)) {
					mixerIdx = (size_t)i;
					break;
				}
			}
		}
	}

	obs_output_set_media(inst->out, inst->video, audio);
	obs_output_set_mixer(inst->out, mixerIdx);

	if (!obs_output_start(inst->out)) {
		/* X1: the mode was already validated against this device at the canvas
		 * fps (decklink_mode_valid + non-zero mode_id above) and the raster
		 * resolved, so a start failure here is almost never a config error —
		 * it is overwhelmingly the DeckLink device being held by another output
		 * (a second AMV instance targeting the same device_hash, another OBS
		 * DeckLink output, or an external app). Log the device_hash and any
		 * output last-error so the hardware conflict is diagnosable instead of
		 * silent, and distinct from the "config error" refusals logged above
		 * (mode unset / mode invalid). */
		const char *lastErr = obs_output_get_last_error(inst->out);
		obs_log(LOG_WARNING,
			"[multiview-output/decklink] obs_output_start failed for device_hash '%s' — the DeckLink device is likely already in use by another output/instance or application (last error: %s)",
			cfg.deviceHash.c_str(), (lastErr && *lastErr) ? lastErr : "none reported");
		close_output_instance(std::move(inst));
		mark_idle_cooldown(sh);
		return;
	}

	/* Install unless the backend was stop()ed while we were starting. */
	{
		std::lock_guard<std::mutex> lk(sh.mtx);
		if (!sh.cancelled) {
			const uint32_t w = inst->w, h = inst->h;
			sh.inst = std::move(inst);
			sh.state = DeckState::Running;
			sh.active.store(true);
			obs_log(LOG_INFO, "[multiview-output/decklink] output started (%ux%u)", w, h);
			return;
		}
	}
	/* Cancelled mid-start: tear down what we built (UI thread, correct order). */
	close_output_instance(std::move(inst));
}

class DeckLinkOutputBackend : public IMultiviewOutputBackend {
public:
	DeckLinkOutputBackend() : sh_(std::make_shared<SharedState>()) {}
	~DeckLinkOutputBackend() override { stop(); }

	const char *kind() const override { return "decklink"; }

	/* SDI is a continuous feed: always want a frame while running. */
	bool wants_frame() override { return sh_->active.load(); }

	/* Graphics-thread: cache the DeckLink hardware settings into the desired
	 * config. Called every frame during reconcile BEFORE configure_audio (the
	 * manager guarantees the order), so the create/restart decision in
	 * configure_audio sees these fields already set — equivalent to the pre-A3
	 * single call that filled all six DeckCfg fields at once. Pure setter: no
	 * lifecycle work happens here. */
	void configure_decklink(const DeckLinkBackendSettings &hw) override
	{
		want_cfg_.deviceHash = hw.deviceHash;
		want_cfg_.modeId = hw.modeId;
		want_cfg_.keyer = hw.keyer;
		want_cfg_.forceSdr = hw.forceSdr;
	}

	/* Graphics-thread reconcile entry (called every frame while enabled, AFTER
	 * configure_decklink). Caches the desired audio config and drives the internal
	 * lifecycle by dispatching UI tasks — never touches the output/video_t/
	 * hardware directly. */
	void configure_audio(const OutputBackendSettings &cfg) override
	{
		want_cfg_.audioMode = cfg.audioMode;
		want_cfg_.audioTrack = cfg.audioTrackIndex;

		std::unique_ptr<OutputInstance> toClose;
		CreateParam *toCreate = nullptr;

		/* M2: liveness self-heal. If the underlying output died (SDI cable
		 * pulled, error-stop, device lost) obs_output_active goes false while we
		 * still report Running and silently drop every composed frame. Detect it
		 * and tear the dead unit down so the Idle path rebuilds after the
		 * cooldown — the DeckLink equivalent of NDI's send_get_no_connections
		 * gate. configure_audio/stop/submit_frame all run on this graphics thread
		 * and a Running inst is only cleared here, so reading its out pointer
		 * under the mutex is safe; obs_output_active is a lock-free atomic read
		 * called OUTSIDE our mutex (mtx_ never nests an OBS lock). Evaluated
		 * before the config-change restart below. */
		{
			obs_output_t *liveOut = nullptr;
			{
				std::lock_guard<std::mutex> lk(sh_->mtx);
				if (sh_->cancelled || sh_->unavailable)
					return;
				if (sh_->state == DeckState::Running && sh_->inst)
					liveOut = sh_->inst->out;
			}
			if (liveOut && !obs_output_active(liveOut)) {
				std::lock_guard<std::mutex> lk(sh_->mtx);
				if (sh_->state == DeckState::Running && sh_->inst && sh_->inst->out == liveOut) {
					toClose = std::move(sh_->inst);
					sh_->state = DeckState::Idle;
					sh_->active.store(false);
					sh_->cooldown_until_ns = os_gettime_ns() + kCooldownNs;
					obs_log(LOG_WARNING,
						"[multiview-output/decklink] output stopped underneath us; tearing down to rebuild");
				}
			}
		}

		/* Skip the create / config-change-restart decision if we just self-healed
		 * (state is Idle with a fresh cooldown; the rebuild fires once it expires). */
		if (!toClose) {
			std::lock_guard<std::mutex> lk(sh_->mtx);
			switch (sh_->state) {
			case DeckState::Running:
				/* A config change restarts the whole output (mode raster,
				 * keyer, HDR and audio routing are all fixed at create). */
				if (want_cfg_ != applied_cfg_) {
					toClose = std::move(sh_->inst);
					sh_->state = DeckState::Idle;
					sh_->active.store(false);
				}
				break;
			case DeckState::Idle:
				/* H2: mode_id is a hard precondition (0 or an invalid id crashes
				 * obs_output_create), so don't even dispatch a create task without
				 * a real mode — create_task re-validates the id against the device. */
				if (!want_cfg_.deviceHash.empty() && want_cfg_.modeId != 0 &&
				    os_gettime_ns() >= sh_->cooldown_until_ns) {
					sh_->state = DeckState::Starting;
					applied_cfg_ = want_cfg_;
					toCreate = new CreateParam{sh_, want_cfg_, next_uid()};
				}
				break;
			case DeckState::Starting:
				break; /* create task in flight; re-check when it completes */
			}
		}

		/* H1: heavy output teardown (obs_output_stop/release joins the capture
		 * thread + Deactivates hardware, ~tens of ms) runs on the UI thread via
		 * QMetaObject::invokeMethod(qApp, ..., Qt::QueuedConnection) rather than
		 * obs_queue_task(OBS_TASK_UI). This call site is on the graphics thread (so
		 * obs_queue_task here would be async anyway), but the same close must also
		 * be non-inline on the main-thread teardown paths (stop(), below) — a queued
		 * invokeMethod always posts a QMetaCallEvent, never runs inline even when the
		 * caller is already on the target thread, so both paths share one safe close
		 * dispatch. Posting a QMetaCallEvent (rather than a QTimer/timer event) also
		 * lets the exit / module-unload path flush any still-pending close via
		 * drain_deferred_output_teardowns() (plugin-main.cpp), so the OutputInstance
		 * and its hardware are never leaked when the event loop stops early. The
		 * create task is only ever reached from the graphics thread, so its
		 * obs_queue_task stays a cross-thread QueuedConnection. */
		if (toClose) {
			OutputInstance *raw = toClose.release();
			QMetaObject::invokeMethod(
				qApp, [raw]() { close_output_instance(std::unique_ptr<OutputInstance>(raw)); },
				Qt::QueuedConnection);
		}
		if (toCreate)
			obs_queue_task(OBS_TASK_UI, &create_task, toCreate, false);
	}

	void submit_frame(const std::string &name, gs_texture_t *tex, uint32_t w, uint32_t h, int fpsDivisor) override
	{
		(void)name;
		(void)fpsDivisor; /* decklink runs at full canvas fps (fpsDivisor fixed 1) */
		if (!tex || w == 0 || h == 0)
			return;
		if (!sh_->active.load())
			return;

		/* Double-buffered readback (SDI is a continuous monitoring/feed output, so
		 * +1 frame of latency is fine and far safer than a synchronous stall of the
		 * main program). The helper owns the ping-pong + staging surfaces; we only
		 * copy the mapped BGRA frame into the output's video_t (feed_frame, which
		 * takes the shared mutex). The submit() return is unused: liveness is
		 * tracked by the output state (sh_->active), not by whether a frame mapped. */
		readback_.submit(tex, w, h, /*doubleBuffer=*/true,
				 [&](uint8_t *data, uint32_t linesize) { feed_frame(data, linesize, w, h); });
	}

	void stop() override
	{
		readback_.destroy(); /* graphics resources: direct on the graphics thread */

		std::unique_ptr<OutputInstance> toClose;
		{
			std::lock_guard<std::mutex> lk(sh_->mtx);
			sh_->cancelled = true; /* any in-flight create task will self-clean */
			toClose = std::move(sh_->inst);
			sh_->state = DeckState::Idle;
			sh_->active.store(false);
		}
		/* H1: dispatch the heavy teardown off the current stack, never inline.
		 * Per the IMultiviewOutputBackend::stop() thread contract, stop() may be
		 * reached on the graphics thread (MultiviewOutputManager::reconcile) OR on
		 * the main thread under the OBS graphics lock (apply_output_settings /
		 * shutdown_graphics / destructor); the graphics lock is held either way, so
		 * obs_output_stop/release (which join the capture thread + Deactivate the
		 * hardware) must not run synchronously here — a queued QMetaObject::
		 * invokeMethod always posts the close to run after the graphics lock is
		 * released. On the exit / module-unload path the posted QMetaCallEvent is
		 * flushed by drain_deferred_output_teardowns() (plugin-main.cpp) so the
		 * OutputInstance is never leaked with its hardware left active. */
		if (toClose) {
			OutputInstance *raw = toClose.release();
			QMetaObject::invokeMethod(
				qApp, [raw]() { close_output_instance(std::unique_ptr<OutputInstance>(raw)); },
				Qt::QueuedConnection);
		}

		readback_.reset_warnings();
		warned_dim_mismatch_ = false;
	}

	bool is_active() const override { return sh_->active.load(); }

	/* M1: the running output's mode raster is authoritative over the persisted
	 * customWidth/customHeight snapshot. Reporting it lets the manager compose at
	 * exactly the SDI raster, so a stale/mismatched snapshot (hand-edited resMode,
	 * cross-machine config, driver change) can't silently drop every frame on the
	 * feed_frame dimension guard. Returns false until Running so the manager falls
	 * back to resolve_output_dimensions. Graphics-thread (reconcile); the mutex
	 * matches every other inst access. */
	bool compose_size(uint32_t &w, uint32_t &h) const override
	{
		std::lock_guard<std::mutex> lk(sh_->mtx);
		if (sh_->inst && sh_->inst->w > 0 && sh_->inst->h > 0) {
			w = sh_->inst->w;
			h = sh_->inst->h;
			return true;
		}
		return false;
	}

private:
	/* Copy one mapped BGRA staging frame into the output's video_t. Holds the
	 * shared mutex across lock_frame -> memcpy -> unlock_frame so a concurrent
	 * teardown can't close the video_t mid-copy (§3.2). */
	void feed_frame(uint8_t *src, uint32_t src_linesize, uint32_t w, uint32_t h)
	{
		std::lock_guard<std::mutex> lk(sh_->mtx);
		if (!sh_->inst || !sh_->inst->video)
			return;
		if (w != sh_->inst->w || h != sh_->inst->h) {
			/* Compose size != mode raster: must not memcpy mismatched buffers.
			 * Drop; the dialog locks the compose size to the raster, so this is
			 * a defensive guard (e.g. a mid-flight resize). */
			if (!warned_dim_mismatch_) {
				obs_log(LOG_WARNING,
					"[multiview-output/decklink] frame %ux%u != mode raster %ux%u, dropping", w, h,
					sh_->inst->w, sh_->inst->h);
				warned_dim_mismatch_ = true;
			}
			return;
		}
		warned_dim_mismatch_ = false;

		struct video_frame vf;
		if (!video_output_lock_frame(sh_->inst->video, &vf, 1, os_gettime_ns()))
			return; /* ring full: drop this frame (non-blocking) */

		size_t rowbytes = (size_t)w * 4;
		if (rowbytes > src_linesize)
			rowbytes = src_linesize;
		if (rowbytes > (size_t)vf.linesize[0])
			rowbytes = (size_t)vf.linesize[0];
		for (uint32_t y = 0; y < h; y++)
			memcpy(vf.data[0] + (size_t)y * vf.linesize[0], src + (size_t)y * src_linesize, rowbytes);

		video_output_unlock_frame(sh_->inst->video);
	}

	static std::string next_uid()
	{
		static std::atomic<uint64_t> counter{0};
		return std::to_string(counter.fetch_add(1));
	}

	std::shared_ptr<SharedState> sh_;

	/* Graphics-thread-only config tracking. */
	DeckCfg want_cfg_;
	DeckCfg applied_cfg_;

	/* Graphics-thread-only GPU->CPU staging readback (always double-buffered).
	 * Owns the staging surfaces + ping-pong + map/create warnings; we supply only
	 * the feed_frame tail (which takes sh_->mtx). */
	StagedReadback readback_{"[multiview-output/decklink]"};

	bool warned_dim_mismatch_ = false;
};

} /* anonymous namespace */

std::unique_ptr<IMultiviewOutputBackend> create_decklink_output_backend()
{
	return std::make_unique<DeckLinkOutputBackend>();
}

#endif /* AMV_ENABLE_DECKLINK_OUTPUT */
