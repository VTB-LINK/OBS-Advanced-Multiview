/*
OBS Advanced Multiview - AJA output backend (issue #18)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#ifdef AMV_ENABLE_AJA_OUTPUT

#include "multiview-output-aja.hpp"
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
#include <utility>

namespace {

/* OBS aja_output property ids, from obs-studio/plugins/aja/aja-ui-props.hpp.
 * Replicated so we can drive the aja_output property machinery (list enumeration
 * / validity checks) and build its create settings WITHOUT any NTV2 SDK header. */
constexpr const char *kAjaPropDevice = "ui_prop_device";
constexpr const char *kAjaPropOutput = "ui_prop_output";
constexpr const char *kAjaPropVideoFormat = "ui_prop_vid_fmt";
constexpr const char *kAjaPropPixelFormat = "ui_prop_pix_fmt";
constexpr const char *kAjaPropSDITransport = "ui_prop_sdi_transport";
constexpr const char *kAjaPropSDITransport4K = "ui_prop_sdi_transport_4k";
constexpr const char *kAjaPropOutputID = "aja_output_id";

/* After a failed start (channel busy, invalid selection, canvas fps changed) wait
 * this long before the graphics thread re-dispatches a create task, so a
 * persistent failure can't spam the UI queue + logs every frame. */
constexpr uint64_t kCooldownNs = 3ULL * 1000000000ULL; /* 3 s */

/* Internal lifecycle of the OBS output / video_t / hardware (distinct from the C++
 * lifetime of the backend object). Idle -> Starting (UI create task in flight) ->
 * Running; any teardown returns to Idle. */
enum class AjaState { Idle, Starting, Running };

/* One live output unit: the aja output, the BGRA video_t feeding it (opened at the
 * CANVAS size — AJA cannot report its SDI raster before create), an optional
 * self-owned silent audio_t (audioMode == None), the compose size the video_t was
 * opened at, the CardManager channel-owner string, and the backing string for
 * video_output_info.name (libobs stores the pointer, so it must outlive the
 * video_t — it lives here and dies with the unit).
 *
 * Ownership: built on the UI thread, installed into SharedState under its mutex,
 * and destroyed ONLY via close_output_instance on the UI thread (never in a
 * destructor — close order matters and must not run on the graphics thread). */
struct OutputInstance {
	obs_output_t *out = nullptr;
	video_t *video = nullptr;
	audio_t *silent_audio = nullptr; /* owned iff non-null (close on teardown) */
	std::string video_name;
	std::string owner; /* kUIPropAJAOutputID — process-unique channel owner (§6) */
	uint32_t w = 0, h = 0;
};

/* Cross-thread state shared between the backend (graphics thread) and in-flight UI
 * tasks. Held by a shared_ptr so a create task that outlives the backend (backend
 * torn down while Starting) still has a valid object to report into / self-clean
 * against. A single mutex guards every field below. */
struct SharedState {
	std::mutex mtx;
	std::unique_ptr<OutputInstance> inst; /* installed output; null unless Running */
	AjaState state = AjaState::Idle;
	uint64_t cooldown_until_ns = 0;
	bool cancelled = false;   /* backend stop()ed: create task must self-clean, no more work */
	bool unavailable = false; /* aja_output not registered: stop retrying */

	/* Fast, lock-free gate for the per-frame hot path (wants_frame / submit_frame).
	 * Authoritative frame feeding still takes mtx + null-checks inst; this only
	 * avoids wasted GPU work when not running. */
	std::atomic<bool> active{false};
};

/* Desired AJA config snapshot (graphics-thread-only on the backend; copied into a
 * create task). Any change triggers a full output restart (aja_output's update() is
 * a no-op, so every setting is fixed at create). Includes the resolved compose
 * dimensions so a canvas resize restarts the fixed-size video_t. */
struct AjaCfg {
	std::string cardID;
	long long ioSelect = kAjaIoSelectionInvalid;
	long long videoFormat = kAjaVideoFormatUnknown;
	long long pixelFormat = 0;
	long long sdiTransport = 0;
	long long sdi4kTransport = 1;
	OutputAudioMode audioMode = OutputAudioMode::FollowStreaming;
	int audioTrack = 1;
	uint32_t w = 0, h = 0; /* resolved compose (canvas) size */

	bool operator==(const AjaCfg &o) const
	{
		return cardID == o.cardID && ioSelect == o.ioSelect && videoFormat == o.videoFormat &&
		       pixelFormat == o.pixelFormat && sdiTransport == o.sdiTransport &&
		       sdi4kTransport == o.sdi4kTransport && audioMode == o.audioMode && audioTrack == o.audioTrack &&
		       w == o.w && h == o.h;
	}
	bool operator!=(const AjaCfg &o) const { return !(*this == o); }
};

struct CreateParam {
	std::shared_ptr<SharedState> sh;
	AjaCfg cfg;
	std::string uid; /* unique suffix; the owner id and video_t name derive from it */
};

/* ---- UI-thread helpers (run only on OBS_TASK_UI / a queued qApp meta-call) ---- */

/* Close one output unit in dependency-safe order: stop + release the output FIRST
 * (obs_output_stop -> aja_output_stop releases the AJA channel and ends data
 * capture; obs_output_release -> aja_output_destroy joins the output thread), THEN
 * close the video_t (now nothing reads it), THEN the silent audio. These calls
 * join threads / touch hardware, so this must run on the UI thread, never the
 * graphics thread. Safe on a partially built unit (any handle may be null). */
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
	/* inst (and its video_name/owner backing strings) freed here, after the
	 * video_t is closed. */
}

/* Mark the shared state Idle with a retry cooldown after a failed create. */
void mark_idle_cooldown(SharedState &sh)
{
	std::lock_guard<std::mutex> lk(sh.mtx);
	sh.state = AjaState::Idle;
	sh.active.store(false);
	sh.cooldown_until_ns = os_gettime_ns() + kCooldownNs;
}

/* Digital-silence generator for the None audio mode. libobs pre-zeroes the mix
 * buffers before each call, so emitting them verbatim (return true) produces
 * continuous silence at the output's cadence — keeping the AV output's audio path
 * fed (OBS_OUTPUT_AV requires a non-null audio source). */
bool silent_audio_input(void *, uint64_t, uint64_t end_ts, uint64_t *new_ts, uint32_t, struct audio_output_data *)
{
	*new_ts = end_ts;
	return true;
}

bool open_silent_audio(audio_t **out)
{
	struct audio_output_info oi = {};
	oi.name = "amv-aja-silent";
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

/* UI thread: true iff the configured card / io / videoFormat / pixelFormat are all
 * currently offered by OBS's aja_output for this device at the canvas frame rate.
 * We drive OBS's own property machinery exactly as its settings dialog does: query
 * the aja_output properties, write the device (+ a scratch owner id, io, vf) into a
 * scratch obs_data, fire the device's modified callback (which enumerates the card
 * and fills the io / videoFormat[fps-filtered] / pixelFormat lists), then scan for
 * membership. This is the AJA analogue of decklink_mode_valid, and — because the
 * videoFormat list is fps-filtered — it also transparently refuses a saved
 * videoFormat invalidated by a canvas-fps change (rather than creating a drifting
 * output). Membership only (not the disabled/in-use state): whether the channel is
 * actually free is left to AcquireOutputSelection inside obs_output_create, the
 * authoritative arbiter, so a transient CardManager state can't cause a false
 * refusal. UI-thread only (the aja callbacks enumerate hardware via CardManager). */
bool aja_selection_valid(const AjaCfg &cfg, const std::string &ownerId)
{
	if (cfg.cardID.empty() || cfg.ioSelect == kAjaIoSelectionInvalid || cfg.videoFormat == kAjaVideoFormatUnknown)
		return false;

	obs_properties_t *props = obs_get_output_properties("aja_output");
	if (!props)
		return false;

	bool ok = false;
	obs_property_t *deviceProp = obs_properties_get(props, kAjaPropDevice);
	obs_property_t *ioProp = obs_properties_get(props, kAjaPropOutput);
	obs_property_t *vfProp = obs_properties_get(props, kAjaPropVideoFormat);
	obs_property_t *pfProp = obs_properties_get(props, kAjaPropPixelFormat);
	if (deviceProp && ioProp && vfProp && pfProp) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, kAjaPropDevice, cfg.cardID.c_str());
		obs_data_set_string(settings, kAjaPropOutputID, ownerId.c_str());
		obs_data_set_int(settings, kAjaPropOutput, cfg.ioSelect);
		obs_data_set_int(settings, kAjaPropVideoFormat, cfg.videoFormat);
		/* Runs aja_output_device_changed: fills io / videoFormat / pixelFormat. */
		obs_property_modified(deviceProp, settings);

		auto list_has_int = [](obs_property_t *p, long long v) {
			const size_t n = obs_property_list_item_count(p);
			for (size_t i = 0; i < n; i++)
				if (obs_property_list_item_int(p, i) == v)
					return true;
			return false;
		};
		ok = list_has_int(ioProp, cfg.ioSelect) && list_has_int(vfProp, cfg.videoFormat) &&
		     list_has_int(pfProp, cfg.pixelFormat);
	}

	obs_properties_destroy(props);
	return ok;
}

/* UI thread: build + start the output, then install it into the shared state
 * (unless the backend was torn down meanwhile, in which case self-clean). Every
 * early-out closes any partially built unit and sets a retry cooldown.
 *
 * Ordering rationale (AJA-specific): aja_output_create is where the card CHANNEL is
 * acquired (AcquireOutputSelection) and the output thread is spawned. aja_output_
 * destroy does NOT release the channel — only aja_output_stop does, and that runs
 * only once the output has gone active (obs_output_destroy calls actual_stop only
 * when active()). So we do every OTHER fallible step (video_t, audio) BEFORE
 * obs_output_create: a failure there holds no channel. The single irreducible leak
 * window is a post-create obs_output_start failure (documented at that site). */
void create_task(void *param)
{
	std::unique_ptr<CreateParam> p(static_cast<CreateParam *>(param));
	SharedState &sh = *p->sh;
	const AjaCfg &cfg = p->cfg;
	const std::string owner = "amv-aja-" + p->uid;

	/* aja_output not registered (no AJA module/card): stop retrying. */
	if (obs_get_output_flags("aja_output") == 0) {
		std::lock_guard<std::mutex> lk(sh.mtx);
		sh.state = AjaState::Idle;
		sh.active.store(false);
		sh.unavailable = true;
		return;
	}

	/* Backend stop()ed before this task ran: skip the heavy card work entirely. */
	{
		std::lock_guard<std::mutex> lk(sh.mtx);
		if (sh.cancelled) {
			sh.state = AjaState::Idle;
			sh.active.store(false);
			return;
		}
	}

	/* Cheap field gate (mirrors the graphics-thread gate; re-guard here). */
	if (cfg.cardID.empty() || cfg.ioSelect == kAjaIoSelectionInvalid || cfg.videoFormat == kAjaVideoFormatUnknown ||
	    cfg.w == 0 || cfg.h == 0) {
		mark_idle_cooldown(sh);
		return;
	}

	/* Authoritative validity: device must currently offer this io/videoFormat/
	 * pixelFormat at the canvas fps (also catches a canvas-fps change that
	 * invalidated the saved videoFormat). Refuse rather than occupy a channel with
	 * a doomed config + spam logs. */
	if (!aja_selection_valid(cfg, owner)) {
		obs_log(LOG_WARNING,
			"[multiview-output/aja] configured device/io/format not currently valid (card missing, or canvas fps changed); refusing to create");
		mark_idle_cooldown(sh);
		return;
	}

	auto inst = std::make_unique<OutputInstance>();
	inst->video_name = owner; /* the video_t name and channel owner share the uid */
	inst->owner = owner;
	inst->w = cfg.w;
	inst->h = cfg.h;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || ovi.fps_num == 0 || ovi.fps_den == 0) {
		close_output_instance(std::move(inst));
		mark_idle_cooldown(sh);
		return;
	}

	/* video_t at the CANVAS size in BGRA (§3). aja_output_start installs a video
	 * conversion to the SDI raster + UYVY/BGR3, so libobs's raw scaler does the
	 * resize + reformat; raw_video then reads exactly GetTotalRasterBytes(). We tag
	 * the source honestly as 709/FULL (the composed BGRA is full-range); the aja
	 * conversion targets 709/PARTIAL and the scaler converts the range. Exact color
	 * fidelity is a known AJA risk (aja-output.cpp "colors are off" TODO), to verify
	 * on real hardware. */
	struct video_output_info voi = {};
	voi.name = inst->video_name.c_str();
	voi.format = VIDEO_FORMAT_BGRA;
	voi.width = inst->w;
	voi.height = inst->h;
	voi.fps_num = ovi.fps_num;
	voi.fps_den = ovi.fps_den;
	voi.cache_size = 4;
	voi.colorspace = VIDEO_CS_709;
	voi.range = VIDEO_RANGE_FULL;
	if (video_output_open(&inst->video, &voi) != VIDEO_OUTPUT_SUCCESS || !inst->video) {
		obs_log(LOG_WARNING, "[multiview-output/aja] video_output_open failed");
		inst->video = nullptr; /* failure may leave it dangling; null before close */
		close_output_instance(std::move(inst));
		mark_idle_cooldown(sh);
		return;
	}

	/* Resolve the audio the output pulls (OBS_OUTPUT_AV requires audio != NULL). */
	audio_t *audio = nullptr;
	size_t mixerIdx = 0;
	if (cfg.audioMode == OutputAudioMode::None) {
		if (!open_silent_audio(&inst->silent_audio) || !inst->silent_audio) {
			obs_log(LOG_WARNING, "[multiview-output/aja] silent audio open failed");
			close_output_instance(std::move(inst));
			mark_idle_cooldown(sh);
			return;
		}
		audio = inst->silent_audio;
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
			 * from the main-thread frontend cache (safe here on the UI thread;
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

	/* Create the output LAST (this acquires the AJA channel, spawns the output
	 * thread, and waits for a vertical interrupt — the reason create must run on the
	 * UI thread, never the graphics thread). A doomed config returns nullptr SAFELY
	 * (no crash), unlike DeckLink. */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, kAjaPropDevice, cfg.cardID.c_str());
	obs_data_set_string(settings, kAjaPropOutputID, owner.c_str());
	obs_data_set_int(settings, kAjaPropOutput, cfg.ioSelect);
	obs_data_set_int(settings, kAjaPropVideoFormat, cfg.videoFormat);
	obs_data_set_int(settings, kAjaPropPixelFormat, cfg.pixelFormat);
	obs_data_set_int(settings, kAjaPropSDITransport, cfg.sdiTransport);
	obs_data_set_int(settings, kAjaPropSDITransport4K, cfg.sdi4kTransport);

	inst->out = obs_output_create("aja_output", (inst->video_name + "-out").c_str(), settings, nullptr);
	if (!inst->out) {
		/* nullptr => invalid selection OR the IOSelection/channel is already held
		 * by another owner (another AMV output/instance, OBS's own aja output, or
		 * an external app). AcquireOutputSelection is the authoritative arbiter; no
		 * channel is held on this path. */
		obs_log(LOG_WARNING,
			"[multiview-output/aja] obs_output_create failed for card '%s' io %lld — the AJA channel is likely already in use, or the selection is invalid",
			cfg.cardID.c_str(), cfg.ioSelect);
		close_output_instance(std::move(inst)); /* out is null: closes video_t/audio only */
		mark_idle_cooldown(sh);
		return;
	}

	obs_output_set_media(inst->out, inst->video, audio);
	obs_output_set_mixer(inst->out, mixerIdx);

	if (!obs_output_start(inst->out)) {
		const char *lastErr = obs_output_get_last_error(inst->out);
		/* Irreducible leak window: the channel was acquired at create, but the
		 * output never went active, so obs_output_stop/release below runs
		 * aja_output_destroy WITHOUT aja_output_stop — the channel stays owned until
		 * OBS restarts. We minimize the odds by validating the selection first
		 * (aja_selection_valid); a start failure after that is rare (routing /
		 * begin_data_capture). Logged clearly; still SAFE (no crash, no graphics-
		 * thread stall). Real-hardware hardening item (§8). */
		obs_log(LOG_WARNING,
			"[multiview-output/aja] obs_output_start failed for card '%s' io %lld (last error: %s); the acquired channel may remain held until restart",
			cfg.cardID.c_str(), cfg.ioSelect, (lastErr && *lastErr) ? lastErr : "none reported");
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
			sh.state = AjaState::Running;
			sh.active.store(true);
			obs_log(LOG_INFO, "[multiview-output/aja] output started (%ux%u) on card '%s' io %lld", w, h,
				cfg.cardID.c_str(), cfg.ioSelect);
			return;
		}
	}
	/* Cancelled mid-start: tear down (UI thread). The output is active now, so this
	 * stop+release runs aja_output_stop and releases the channel cleanly. */
	close_output_instance(std::move(inst));
}

class AjaOutputBackend : public IMultiviewOutputBackend {
public:
	AjaOutputBackend() : sh_(std::make_shared<SharedState>()) {}
	~AjaOutputBackend() override { stop(); }

	const char *kind() const override { return "aja"; }

	/* SDI is a continuous feed: always want a frame while running. */
	bool wants_frame() override { return sh_->active.load(); }

	/* Graphics-thread: cache the AJA hardware settings into the desired config.
	 * Called every frame during reconcile BEFORE configure_audio, so the
	 * create/restart decision there sees these fields already set. Pure setter. */
	void configure_aja(const AjaBackendSettings &hw) override
	{
		want_cfg_.cardID = hw.cardID;
		want_cfg_.ioSelect = hw.ioSelect;
		want_cfg_.videoFormat = hw.videoFormat;
		want_cfg_.pixelFormat = hw.pixelFormat;
		want_cfg_.sdiTransport = hw.sdiTransport;
		want_cfg_.sdi4kTransport = hw.sdi4kTransport;
	}

	/* Graphics-thread reconcile entry (every frame while enabled, AFTER
	 * configure_aja). Caches audio + the resolved compose size, then drives the
	 * lifecycle by dispatching UI tasks — never touches the output/video_t/hardware
	 * directly. */
	void configure_audio(const OutputBackendSettings &cfg) override
	{
		want_cfg_.audioMode = cfg.audioMode;
		want_cfg_.audioTrack = cfg.audioTrackIndex;
		/* AJA composes at the canvas size (no hardware raster lock, unlike
		 * DeckLink). Track the resolved compose dimensions in the desired config so
		 * a canvas resize restarts the fixed-size video_t. Uses the SAME
		 * resolve_output_dimensions the manager uses for submit dims, so the two
		 * never diverge (the feed_frame dimension guard therefore never trips except
		 * for the brief transient across a resize, which triggers this restart). */
		auto dims = resolve_output_dimensions(cfg);
		want_cfg_.w = dims.first;
		want_cfg_.h = dims.second;

		std::unique_ptr<OutputInstance> toClose;
		CreateParam *toCreate = nullptr;

		/* M2: liveness self-heal. If the underlying output goes inactive while we
		 * still report Running, tear it down so the Idle path rebuilds after the
		 * cooldown. CAVEAT: OBS's aja output does NOT signal-stop on card-pull /
		 * DMA failure (it just logs and keeps running), so obs_output_active usually
		 * stays true through a hardware loss — this reliably catches only genuine
		 * error-stops. Full card-pull recovery is a real-hardware hardening item
		 * (§8). obs_output_active is a lock-free atomic read taken OUTSIDE our mutex
		 * (mtx_ never nests an OBS lock). */
		{
			obs_output_t *liveOut = nullptr;
			{
				std::lock_guard<std::mutex> lk(sh_->mtx);
				if (sh_->cancelled || sh_->unavailable)
					return;
				if (sh_->state == AjaState::Running && sh_->inst)
					liveOut = sh_->inst->out;
			}
			if (liveOut && !obs_output_active(liveOut)) {
				std::lock_guard<std::mutex> lk(sh_->mtx);
				if (sh_->state == AjaState::Running && sh_->inst && sh_->inst->out == liveOut) {
					toClose = std::move(sh_->inst);
					sh_->state = AjaState::Idle;
					sh_->active.store(false);
					sh_->cooldown_until_ns = os_gettime_ns() + kCooldownNs;
					obs_log(LOG_WARNING,
						"[multiview-output/aja] output stopped underneath us; tearing down to rebuild");
				}
			}
		}

		/* Skip the create / config-change-restart decision if we just self-healed. */
		if (!toClose) {
			std::lock_guard<std::mutex> lk(sh_->mtx);
			switch (sh_->state) {
			case AjaState::Running:
				/* A config change (any field, incl. compose size / audio)
				 * restarts the whole output: aja_output's update() is a no-op,
				 * so everything is fixed at create. */
				if (want_cfg_ != applied_cfg_) {
					toClose = std::move(sh_->inst);
					sh_->state = AjaState::Idle;
					sh_->active.store(false);
				}
				break;
			case AjaState::Idle:
				/* Don't even dispatch a create task without a plausibly valid
				 * selection + a resolved compose size; create_task re-validates
				 * against the live device lists. */
				if (!want_cfg_.cardID.empty() && want_cfg_.ioSelect != kAjaIoSelectionInvalid &&
				    want_cfg_.videoFormat != kAjaVideoFormatUnknown && want_cfg_.w != 0 &&
				    want_cfg_.h != 0 && os_gettime_ns() >= sh_->cooldown_until_ns) {
					sh_->state = AjaState::Starting;
					applied_cfg_ = want_cfg_;
					toCreate = new CreateParam{sh_, want_cfg_, next_uid()};
				}
				break;
			case AjaState::Starting:
				break; /* create task in flight; re-check when it completes */
			}
		}

		/* H1: heavy teardown (obs_output_stop/release joins the output thread +
		 * releases the channel) runs on the UI thread via a queued qApp meta-call,
		 * never inline — a QueuedConnection always posts a QMetaCallEvent, so this is
		 * safe both here (graphics thread) and on the main-thread stop() path (under
		 * the OBS graphics lock). The exit / module-unload path flushes any pending
		 * close via drain_deferred_output_teardowns() (plugin-main.cpp), so the unit
		 * + its hardware are never leaked. */
		if (toClose) {
			OutputInstance *raw = toClose.release();
			QMetaObject::invokeMethod(
				qApp, [raw]() { close_output_instance(std::unique_ptr<OutputInstance>(raw)); },
				Qt::QueuedConnection);
		}
		/* create_task is only ever reached from the graphics thread, so obs_queue_
		 * task(OBS_TASK_UI) is a cross-thread post. */
		if (toCreate)
			obs_queue_task(OBS_TASK_UI, &create_task, toCreate, false);
	}

	void submit_frame(const std::string &name, gs_texture_t *tex, uint32_t w, uint32_t h, int fpsDivisor) override
	{
		(void)name;
		(void)fpsDivisor; /* aja runs at full canvas fps (fpsDivisor fixed 1) */
		if (!tex || w == 0 || h == 0)
			return;
		if (!sh_->active.load())
			return;

		/* Double-buffered readback (SDI is a continuous feed, so +1 frame of latency
		 * is far safer than a synchronous stall of the main program). The helper owns
		 * the ping-pong + staging surfaces; we only copy the mapped BGRA frame into
		 * the output's video_t (feed_frame, which takes the shared mutex). */
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
			sh_->state = AjaState::Idle;
			sh_->active.store(false);
		}
		/* H1: dispatch the heavy teardown off the current stack, never inline (per
		 * the IMultiviewOutputBackend::stop() thread contract, this may run on the
		 * graphics thread OR the main thread under the OBS graphics lock). A queued
		 * qApp meta-call always posts, so the graphics lock is released before the
		 * close runs; the exit path flushes it via drain_deferred_output_teardowns(). */
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

	/* AJA cannot report its SDI raster before create, so it composes at the canvas
	 * size and has no authoritative override — return false so the manager uses
	 * resolve_output_dimensions (which we track in configure_audio and open the
	 * video_t at, so submit dims and video_t size agree). See §3.4. */
	bool compose_size(uint32_t &w, uint32_t &h) const override
	{
		(void)w;
		(void)h;
		return false;
	}

private:
	/* Copy one mapped BGRA staging frame into the output's video_t. Holds the shared
	 * mutex across lock_frame -> memcpy -> unlock_frame so a concurrent teardown
	 * can't close the video_t mid-copy. */
	void feed_frame(uint8_t *src, uint32_t src_linesize, uint32_t w, uint32_t h)
	{
		std::lock_guard<std::mutex> lk(sh_->mtx);
		if (!sh_->inst || !sh_->inst->video)
			return;
		if (w != sh_->inst->w || h != sh_->inst->h) {
			/* Compose size != the video_t size the unit was opened at. Must not
			 * memcpy mismatched buffers; drop. This is the brief transient across a
			 * canvas resize — configure_audio detects the new size and restarts. */
			if (!warned_dim_mismatch_) {
				obs_log(LOG_WARNING,
					"[multiview-output/aja] frame %ux%u != output size %ux%u, dropping (restarting)",
					w, h, sh_->inst->w, sh_->inst->h);
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
	AjaCfg want_cfg_;
	AjaCfg applied_cfg_;

	/* Graphics-thread-only GPU->CPU staging readback (always double-buffered). */
	StagedReadback readback_{"[multiview-output/aja]"};

	bool warned_dim_mismatch_ = false;
};

} /* anonymous namespace */

std::unique_ptr<IMultiviewOutputBackend> create_aja_output_backend()
{
	return std::make_unique<AjaOutputBackend>();
}

#endif /* AMV_ENABLE_AJA_OUTPUT */
