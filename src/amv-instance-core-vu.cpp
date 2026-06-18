/*
OBS Advanced Multiview - VU Meter rendering and audio metering
Split from multiview-window.cpp for maintainability.
All functions remain members of AmvInstanceCore.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-frontend-cache.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

/* Practical silence level in dB (below minimum display range) */
#define VU_SILENCE_DB -200.0f

static const char *default_vu_font_face()
{
#ifdef _WIN32
	return "Arial";
#elif __APPLE__
	return "Helvetica";
#else
	return "Monospace";
#endif
}

/* ---- helpers (same as OBS internal, duplicated for compilation unit) ---- */

static inline void startRegion(int vX, int vY, int vCX, int vCY, float oL, float oR, float oT, float oB)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vX, vY, vCX, vCY);
	gs_ortho(oL, oR, oT, oB, -100.0f, 100.0f);
}

static inline void endRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}
/* ---- dB Scale Label Sources ---- */

void AmvInstanceCore::release_scale_label_sources()
{
	scale_label_cache_.clear();
}

void AmvInstanceCore::rebuild_scale_label_sources()
{
	release_scale_label_sources();

	/* Collect all unique dB tick values needed across cells */
	struct ScaleTextNeed {
		float db = 0.0f;
		int dbTenths = 0;
		std::string fontFamily;
	};
	std::vector<ScaleTextNeed> allTicks;
	for (size_t i = 0; i < effective_visuals_.size(); i++) {
		const VuMeterSettings &vm = effective_visuals_[i].vuMeter;
		if (!vm.enabled || !vm.scaleEnabled || !vm.scaleShowLabels)
			continue;

		std::string tickStr = vm.scaleTicks;
		if (tickStr.empty())
			tickStr = "-60,-40,-20,-9,0";

		size_t pos = 0;
		while (pos < tickStr.size()) {
			size_t comma = tickStr.find(',', pos);
			if (comma == std::string::npos)
				comma = tickStr.size();
			std::string token = tickStr.substr(pos, comma - pos);
			pos = comma + 1;
			try {
				float val = std::stof(token);
				if (val >= -96.0f && val <= 0.0f) {
					ScaleTextNeed need;
					need.db = val;
					need.dbTenths = (int)(val * 10.0f + (val < 0 ? -0.5f : 0.5f));
					need.fontFamily = vm.fontFamily;
					allTicks.push_back(std::move(need));
				}
			} catch (...) {
			}
		}
	}

	/* Deduplicate and cap at 20 unique entries */
	std::sort(allTicks.begin(), allTicks.end(), [](const ScaleTextNeed &a, const ScaleTextNeed &b) {
		if (a.dbTenths != b.dbTenths)
			return a.dbTenths < b.dbTenths;
		return a.fontFamily < b.fontFamily;
	});
	allTicks.erase(std::unique(allTicks.begin(), allTicks.end(),
				   [](const ScaleTextNeed &a, const ScaleTextNeed &b) {
					   return a.dbTenths == b.dbTenths && a.fontFamily == b.fontFamily;
				   }),
		       allTicks.end());
	if (allTicks.size() > 20)
		allTicks.resize(20);

	if (allTicks.empty())
		return;

	/* Create one text source per unique dB value */
	for (const auto &need : allTicks) {
		float db = need.db;
		int dbTenths = need.dbTenths;

		/* Format label text: integer if whole, one decimal otherwise */
		char buf[16];
		if (db == (int)db)
			snprintf(buf, sizeof(buf), "%d", (int)db);
		else
			snprintf(buf, sizeof(buf), "%.1f", db);

		std::string srcName = "adv_mv_scale_" + uuid_ + "_" + std::to_string(dbTenths);

#ifdef _WIN32
		obs_data_t *fontObj = obs_data_create();
		obs_data_set_int(fontObj, "size", 24);
		obs_data_set_string(fontObj, "face",
				    need.fontFamily.empty() ? default_vu_font_face() : need.fontFamily.c_str());
		obs_data_set_int(fontObj, "flags", 0);

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "text", buf);
		obs_data_set_obj(settings, "font", fontObj);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "opacity", 100);
		obs_data_set_bool(settings, "outline", false);

		obs_source_t *src = obs_source_create_private("text_gdiplus", srcName.c_str(), settings);
		obs_data_release(settings);
		obs_data_release(fontObj);
#else
		obs_data_t *fontObj = obs_data_create();
		obs_data_set_int(fontObj, "size", 24);
		obs_data_set_string(fontObj, "face",
				    need.fontFamily.empty() ? default_vu_font_face() : need.fontFamily.c_str());
		obs_data_set_int(fontObj, "flags", 0);

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "text", buf);
		obs_data_set_obj(settings, "font", fontObj);
		obs_data_set_int(settings, "color1", 0xFFFFFFFF);
		obs_data_set_int(settings, "color2", 0xFFFFFFFF);
		obs_data_set_bool(settings, "outline", false);
		obs_data_set_bool(settings, "drop_shadow", false);

		obs_source_t *src = obs_source_create_private("text_ft2_source_v2", srcName.c_str(), settings);
		obs_data_release(settings);
		obs_data_release(fontObj);
#endif

		if (src) {
			ScaleLabelEntry entry;
			entry.dbTenths = dbTenths;
			entry.fontFamily = need.fontFamily;
			entry.source = src;
			entry.width = obs_source_get_width(src);
			entry.height = obs_source_get_height(src);
			scale_label_cache_.push_back(std::move(entry));
			obs_source_release(src);
		}
	}
}

/* ---- VU Meter ---- */

void AmvInstanceCore::volmeter_callback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
					const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS])
{
	UNUSED_PARAMETER(inputPeak);
	auto *sv = static_cast<SingleVolmeter *>(data);
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		sv->magnitude[i] = magnitude[i];
		sv->peak[i] = peak[i];
	}
	sv->last_callback_ns = os_gettime_ns();
}

/* "mute" signal handler. Fires on Mixer mute toggle (user_muted, set via
 * obs_source_set_muted). Note: this does NOT include PTT/PTM transient mute
 * (push_to_mute_enabled), which OBS handles by zeroing volmeter audio buffers
 * before the volmeter callback. UI mute on the other hand leaves the volmeter
 * data unchanged, so we must zero the display ourselves to keep WYSIWYG with
 * what audience hears. */
void AmvInstanceCore::source_mute_callback(void *data, calldata_t *cd)
{
	auto *sv = static_cast<SingleVolmeter *>(data);
	bool muted = calldata_bool(cd, "muted");
	sv->user_muted.store(muted, std::memory_order_relaxed);
}

/* "audio_mixers" signal handler. Fires when a source's enabled mixer track
 * bitmask changes. Since this affects which sources are visible (sources with
 * audio_mixers & active_track_bit == 0 are excluded), request a deferred
 * rebuild — actual rebuild happens on the next render frame via
 * check_active_track_change() to coalesce bursts and stay on the render thread. */
void AmvInstanceCore::source_audio_mixers_callback(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	auto *self = static_cast<AmvInstanceCore *>(data);
	self->volmeters_rebuild_requested_.store(true, std::memory_order_release);
}

/* Compute the active mixer track bit based on VuMeterSettings.
 *
 * For VuMeterTrackMode::Manual: bit = 1 << (manualTrackIndex - 1).
 * For VuMeterTrackMode::AutoFollowStreaming: read OBS streaming output's
 *   mixer mask and pick the first set bit (single-track semantics per design).
 *
 * Reads the instance-level VuMeterSettings from the config. Note: cell-level
 * override of trackMode is deferred to M2.6; v1 uses one bit for the whole
 * window so all cells share the same "what audience hears" semantics.
 *
 * Fallback chain when AutoFollow can't resolve a mask:
 *   streaming output missing OR mixers == 0 → Track 1 (bit 0).
 * Per OBS UI invariant, Settings → Output → Streaming Audio Track requires
 * at least 1 of 6 selected, so mixers == 0 should be unreachable in practice. */
uint32_t AmvInstanceCore::compute_active_track_bit()
{
	MultiviewInstance *inst = config_ ? config_->find_instance(uuid_) : nullptr;

	/* Read instance-level VuMeterSettings directly. Track selection is a
	 * window-wide knob in v1 (per-cell trackMode override is deferred);
	 * resolving the full effective settings would only end up reading these
	 * same two fields from the instance layer. */
	VuMeterSettings vm;
	if (inst)
		vm = inst->visualSettings.vuMeter;

	if (vm.trackMode == VuMeterTrackMode::Manual) {
		int idx = vm.manualTrackIndex;
		if (idx < 1)
			idx = 1;
		if (idx > 6)
			idx = 6;
		return 1u << (idx - 1);
	}

	/* AutoFollow: streaming output mixer mask, read from the main-thread-updated
	 * frontend cache (never call obs_frontend_* on the render thread — issue #10
	 * isolation F2). The cache reads obs_output_get_mixers live, so mid-stream
	 * track edits are still followed. */
	uint32_t mask = amv_frontend::streaming_mixers();
	if (mask == 0)
		return 0x1; /* Track 1 fallback */
	/* Lowest set bit only (single track semantics) */
	return mask & (~mask + 1);
}

/* Helper: collect all audio sources within a scene into a vector.
 *
 * Recursively descends into:
 *   - Groups (via obs_sceneitem_group_enum_items)
 *   - Nested scenes (Scene sources whose underlying source IS a scene)
 *
 * Skips invisible sceneitems (OBS mutes audio when sceneitem is hidden).
 * Dedupes by source pointer so that the same source referenced from multiple
 * places in the tree is only attached once per cell.
 *
 * Filters out sources whose audio_mixers bitmask does not intersect
 * track_bit. This implements "what audience hears" semantics — sources that
 * don't route to the active streaming track contribute 0 to PGM, so they're
 * excluded from the multiview VU meter.
 *
 * MAX_DEPTH guards against pathological nesting (OBS prevents true cycles
 * at scene-link time, but defense in depth is cheap).
 */
struct AudioCollectCtx {
	std::vector<obs_source_t *> sources;
	uint32_t track_bit = 0xFFFFFFFF; /* default: no filtering */
	int depth = 0;
	bool depth_exceeded = false; /* set when MAX_DEPTH was reached at least once */
	static constexpr int MAX_DEPTH = 8;
};

static bool collect_audio_sources_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (!obs_sceneitem_visible(item))
		return true;
	obs_source_t *itemSrc = obs_sceneitem_get_source(item);
	if (!itemSrc)
		return true;

	auto *ctx = static_cast<AudioCollectCtx *>(param);
	if (ctx->depth >= AudioCollectCtx::MAX_DEPTH) {
		ctx->depth_exceeded = true;
		return true;
	}

	/* Recurse into groups: groups themselves do not produce audio,
	 * their child items do. */
	if (obs_sceneitem_is_group(item)) {
		ctx->depth++;
		obs_sceneitem_group_enum_items(item, collect_audio_sources_cb, ctx);
		ctx->depth--;
		return true;
	}

	/* Recurse into nested scene sources: scene sources do not directly
	 * produce audio (OBS_SOURCE_AUDIO flag is on their inner items). */
	obs_scene_t *nested = obs_scene_from_source(itemSrc);
	if (nested) {
		ctx->depth++;
		obs_scene_enum_items(nested, collect_audio_sources_cb, ctx);
		ctx->depth--;
		return true;
	}

	uint32_t flags = obs_source_get_output_flags(itemSrc);
	if (flags & OBS_SOURCE_AUDIO) {
		/* Track filter: skip sources that don't route to the active streaming
		 * mixer track. obs_source_get_audio_mixers returns a 6-bit mask;
		 * bit i = Track (i+1). Result 0 means "not in any track" which OBS
		 * uses for sources fed exclusively into spectrum-analyzer style filters. */
		uint32_t am = obs_source_get_audio_mixers(itemSrc);
		if ((am & ctx->track_bit) == 0)
			return true;
		/* Dedup: same source can legitimately appear multiple times
		 * across nested scenes; we only want one volmeter per source. */
		for (auto *existing : ctx->sources) {
			if (existing == itemSrc)
				return true;
		}
		ctx->sources.push_back(obs_source_get_ref(itemSrc));
	}
	return true; /* continue enumeration */
}

/* Collect audio-producing sources reachable from `src` (a scene, group, or
 * audio-bearing source). Track filtering applied via `track_bit`. Returns
 * true if recursion hit MAX_DEPTH at any point — the caller is expected to
 * log a context-rich warning (instance + cell), since logging here would
 * lose the cell index and would fire repeatedly under 1 Hz polling. */
static bool collect_audio_sources(obs_source_t *src, std::vector<obs_source_t *> &out, uint32_t track_bit)
{
	if (!src)
		return false;

	uint32_t flags = obs_source_get_output_flags(src);
	if (flags & OBS_SOURCE_AUDIO) {
		/* Source itself produces audio (e.g. media source assigned
		 * directly to a cell). Apply track filter consistently. */
		uint32_t am = obs_source_get_audio_mixers(src);
		if ((am & track_bit) == 0)
			return false;
		out.push_back(obs_source_get_ref(src));
		return false;
	}

	/* Interpret as scene or group and collect audio sources inside. */
	obs_scene_t *scene = obs_scene_from_source(src);
	if (!scene)
		scene = obs_group_from_source(src);
	if (!scene)
		return false;

	AudioCollectCtx ctx;
	ctx.track_bit = track_bit;
	obs_scene_enum_items(scene, collect_audio_sources_cb, &ctx);
	for (auto *s : ctx.sources)
		out.push_back(s);
	return ctx.depth_exceeded;
}

void AmvInstanceCore::rebuild_volmeters()
{
	struct CellVolmeterBallistics {
		bool valid = false;
		float displayMagnitude = VU_SILENCE_DB;
		float displayPeak = VU_SILENCE_DB;
		uint64_t last_render_ns = 0;
		float holdPeak = VU_SILENCE_DB;
		uint64_t holdSetAtNs = 0;
		float channelDisplayMagnitude[MAX_AUDIO_CHANNELS];
		float channelDisplayPeak[MAX_AUDIO_CHANNELS];
		uint64_t channelLastRenderNs[MAX_AUDIO_CHANNELS];
		float channelHoldPeak[MAX_AUDIO_CHANNELS];
		uint64_t channelHoldSetAtNs[MAX_AUDIO_CHANNELS];
	};

	/* Rebuilding obs_volmeter attachments is sometimes necessary for PGM/PRVW
	 * scene switches, but the visual ballistic state belongs to the cell, not
	 * to the transient OBS volmeter objects. Preserve it across rebuilds so an
	 * unrelated cell does not visibly drop to -inf for a frame after a scene
	 * click changes the Program/Preview bus. */
	std::vector<CellVolmeterBallistics> previousBallistics;
	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);
		previousBallistics.resize(cell_volmeters_.size());
		for (size_t i = 0; i < cell_volmeters_.size(); i++) {
			CellVolmeter *cellVm = cell_volmeters_[i];
			if (!cellVm)
				continue;
			auto &snap = previousBallistics[i];
			snap.valid = true;
			snap.displayMagnitude = cellVm->displayMagnitude;
			snap.displayPeak = cellVm->displayPeak;
			snap.last_render_ns = cellVm->last_render_ns;
			snap.holdPeak = cellVm->holdPeak;
			snap.holdSetAtNs = cellVm->holdSetAtNs;
			for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
				snap.channelDisplayMagnitude[ch] = cellVm->channelDisplayMagnitude[ch];
				snap.channelDisplayPeak[ch] = cellVm->channelDisplayPeak[ch];
				snap.channelLastRenderNs[ch] = cellVm->channelLastRenderNs[ch];
				snap.channelHoldPeak[ch] = cellVm->channelHoldPeak[ch];
				snap.channelHoldSetAtNs[ch] = cellVm->channelHoldSetAtNs[ch];
			}
		}
	}

	auto restore_ballistics = [&previousBallistics](size_t index, CellVolmeter *cellVm) {
		if (!cellVm || index >= previousBallistics.size())
			return;
		const auto &snap = previousBallistics[index];
		if (!snap.valid)
			return;
		cellVm->displayMagnitude = snap.displayMagnitude;
		cellVm->displayPeak = snap.displayPeak;
		cellVm->last_render_ns = snap.last_render_ns;
		cellVm->holdPeak = snap.holdPeak;
		cellVm->holdSetAtNs = snap.holdSetAtNs;
		for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
			cellVm->channelDisplayMagnitude[ch] = snap.channelDisplayMagnitude[ch];
			cellVm->channelDisplayPeak[ch] = snap.channelDisplayPeak[ch];
			cellVm->channelLastRenderNs[ch] = snap.channelLastRenderNs[ch];
			cellVm->channelHoldPeak[ch] = snap.channelHoldPeak[ch];
			cellVm->channelHoldSetAtNs[ch] = snap.channelHoldSetAtNs[ch];
		}
	};

	release_volmeters();

	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	/* Refresh the active track bit before collecting sources so that
	 * track-filtered enumeration uses the up-to-date mask. */
	current_track_bit_ = compute_active_track_bit();

	/* Phase 3 / M6.6: external cells now always meter their own private
	 * source (trackMode is the internal-cell filter only \u2014 see comment
	 * in the per-cell loop below). The instance-level trackMode is only
	 * read by the internal-cell branch via `vm.trackMode`; we no longer
	 * need a snapshot here. */

	size_t count = cell_sources_.size();
	cell_volmeters_.resize(count, nullptr);

	has_pgm_cell_ = false;
	has_prvw_cell_ = false;

	/* Aggregate counters and per-cell breakdown for a single summary log
	 * line at the end. Per-source LOG_INFO would spam the OBS log under
	 * the 1Hz active-source polling, especially on dense grids with
	 * frequent scene mutations. */
	int cells_with_meters = 0;
	int total_attached = 0;
	std::string cells_breakdown; /* "c0:pgm=2 c2:scene=1 ..." */
	cells_breakdown.reserve(64);

	/* Cells whose recursive scene walk hit MAX_DEPTH, aggregated so we
	 * emit a single warning per rebuild rather than one per cell. */
	std::string depth_warn_cells;

	/* Resolve instance once for log prefix. Falls back gracefully when
	 * the instance has been deleted but the window is mid-teardown. */
	MultiviewInstance *log_inst = config_ ? config_->find_instance(uuid_) : nullptr;
	const std::string &inst_name = log_inst ? log_inst->name : std::string();
	std::string short_uuid = uuid_.size() > 8 ? uuid_.substr(0, 8) : uuid_;

	for (size_t i = 0; i < count; i++) {
		const auto &cs = cell_sources_[i];
		if (cs.type.empty() && cs.provider_type == SignalProviderType::Unknown)
			continue;

		/* Phase 3 / M6 step 8: external-provider cell routing.
		 *
		 * External cells own a `private_source` and are NOT walked as a
		 * scene tree — the private source IS the audio object. Attach the
		 * cell's single volmeter directly to it. The instance trackMode
		 * dictates whether external metering is honored:
		 *   - Auto / ExternalSource: enable direct external metering
		 *   - AutoFollowStreaming / Manual: external cells skip VU entirely
		 *     (legacy modes never knew about provider sources; keeping VU
		 *     off avoids surprising users who upgrade configs).
		 *
		 * Spout has no audio; the provider models it as silence rather
		 * than an error. We do not attach a volmeter for Spout cells \u2014 the
		 * UI VU bar simply stays silent at -inf dB. */
		if (cs.provider_type != SignalProviderType::Unknown && !signal_provider_is_internal(cs.provider_type)) {
			/* Phase 3 / M6.6 fix: trackMode is the *internal* track
			 * filter (which OBS streaming/manual track internal cells
			 * follow). External cells use their own private audio
			 * path with no notion of streaming tracks, so trackMode
			 * is orthogonal for them. The previous gate skipped
			 * external cells under AutoFollowStreaming / Manual which
			 * matches the legacy default and silently broke external
			 * VU. */
			if (cs.provider_type == SignalProviderType::Spout)
				continue;
			obs_source_t *priv = cs.private_source.Get();
			if (!priv)
				continue;

			auto *cellVm = new CellVolmeter();
			restore_ballistics(i, cellVm);
			cellVm->meters.reserve(1);

			auto sv = std::make_unique<SingleVolmeter>();
			for (int c = 0; c < MAX_AUDIO_CHANNELS; c++) {
				sv->magnitude[c] = VU_SILENCE_DB;
				sv->peak[c] = VU_SILENCE_DB;
			}
			sv->volmeter = obs_volmeter_create(OBS_FADER_LOG);
			if (!sv->volmeter) {
				delete cellVm;
				continue;
			}
			const char *pname = obs_source_get_name(priv);
			sv->name = pname ? pname : "";
			sv->user_muted.store(obs_source_muted(priv), std::memory_order_relaxed);
			sv->source_weak = OBSGetWeakRef(priv);

			SingleVolmeter *svPtr = sv.get();
			obs_volmeter_add_callback(svPtr->volmeter, volmeter_callback, svPtr);
			obs_volmeter_attach_source(svPtr->volmeter, priv);
			svPtr->channels = obs_volmeter_get_nr_channels(svPtr->volmeter);

			signal_handler_t *sh = obs_source_get_signal_handler(priv);
			if (sh) {
				signal_handler_connect(sh, "mute", source_mute_callback, svPtr);
				signal_handler_connect(sh, "audio_mixers", source_audio_mixers_callback, this);
			}

			cellVm->meters.push_back(std::move(sv));
			cell_volmeters_[i] = cellVm;
			cells_with_meters++;
			total_attached++;

			char cellEntry[64];
			snprintf(cellEntry, sizeof(cellEntry), "%sc%zu:%s=1", cells_breakdown.empty() ? "" : " ", i,
				 signal_provider_to_string(cs.provider_type));
			cells_breakdown += cellEntry;
			continue;
		}

		obs_source_t *cellSrc = nullptr;
		bool isPgm = false;

		if (cs.type == "pgm") {
			/* current_program_scene() hands back an OBSSourceAutoRelease; take an
			 * OWNED +1 (obs_source_get_ref is null-safe) so the obs_source_release
			 * below is balanced. Assigning the AutoRelease straight into a raw
			 * pointer would drop the ref immediately and the release would then
			 * over-decrement a live OBS scene (issue #10 F2 regression). */
			cellSrc = obs_source_get_ref(amv_frontend::current_program_scene());
			isPgm = true;
			has_pgm_cell_ = true;
		} else if (cs.type == "prvw") {
			cellSrc = obs_source_get_ref(amv_frontend::current_preview_scene());
			if (!cellSrc) {
				/* Studio Mode disabled: PRVW has no separate scene, so it
				 * 100% mirrors PGM (matches render() fallback). Treat the
				 * cell as PGM for VU purposes — also enables the global
				 * channel 1..5 sweep below. */
				cellSrc = obs_source_get_ref(amv_frontend::current_program_scene());
				isPgm = (cellSrc != nullptr);
				if (isPgm)
					has_pgm_cell_ = true;
			}
			has_prvw_cell_ = true;
		} else {
			OBSSourceAutoRelease strong = OBSGetStrongRef(cs.weak_ref);
			if (strong) {
				cellSrc = obs_source_get_ref(strong);
			}
		}

		if (!cellSrc)
			continue;

		/* Collect all audio sources from the cell's source/scene, filtered
		 * by the active mixer track bit (per-cell semantics are identical
		 * in v1: instance-level setting drives all cells uniformly). */
		std::vector<obs_source_t *> audioSources;
		bool depth_exceeded = collect_audio_sources(cellSrc, audioSources, current_track_bit_);
		if (depth_exceeded) {
			char cellTag[32];
			snprintf(cellTag, sizeof(cellTag), "%sc%zu", depth_warn_cells.empty() ? "" : ",", i);
			depth_warn_cells += cellTag;
		}
		obs_source_release(cellSrc);

		/* For PGM cells, also include global audio devices (Desktop Audio, Mic/Aux)
		 * which are always mixed into the program output but not part of any scene.
		 * OBS exposes audio devices on output channels 1..5 (channel 0 is the
		 * current scene). Channels 6+ are unused by stock OBS. Apply the same
		 * track filter so sources with audio_mixers & active_bit == 0 are hidden. */
		if (isPgm) {
			for (int ch = 1; ch <= 5; ch++) {
				obs_source_t *globalSrc = obs_get_output_source(ch);
				if (!globalSrc)
					continue;
				uint32_t am = obs_source_get_audio_mixers(globalSrc);
				if ((am & current_track_bit_) == 0) {
					obs_source_release(globalSrc);
					continue;
				}
				/* Avoid duplicates: check if already collected */
				bool dup = false;
				for (auto *existing : audioSources) {
					if (existing == globalSrc) {
						dup = true;
						break;
					}
				}
				if (!dup) {
					audioSources.push_back(globalSrc); /* already has +1 ref */
				} else {
					obs_source_release(globalSrc);
				}
			}
		}

		if (audioSources.empty())
			continue;

		auto *cellVm = new CellVolmeter();
		restore_ballistics(i, cellVm);
		cellVm->meters.reserve(audioSources.size());

		for (auto *audioSrc : audioSources) {
			auto sv = std::make_unique<SingleVolmeter>();
			for (int c = 0; c < MAX_AUDIO_CHANNELS; c++) {
				sv->magnitude[c] = VU_SILENCE_DB;
				sv->peak[c] = VU_SILENCE_DB;
			}
			sv->volmeter = obs_volmeter_create(OBS_FADER_LOG);
			if (!sv->volmeter) {
				obs_source_release(audioSrc);
				continue;
			}
			const char *name = obs_source_get_name(audioSrc);
			sv->name = name ? name : "";

			/* Seed initial mute state from current source value; the "mute"
			 * signal will keep it in sync after connection. */
			sv->user_muted.store(obs_source_muted(audioSrc), std::memory_order_relaxed);
			sv->source_weak = OBSGetWeakRef(audioSrc);

			SingleVolmeter *svPtr = sv.get();
			obs_volmeter_add_callback(svPtr->volmeter, volmeter_callback, svPtr);
			obs_volmeter_attach_source(svPtr->volmeter, audioSrc);
			svPtr->channels = obs_volmeter_get_nr_channels(svPtr->volmeter);

			/* Subscribe to per-source signals so we react to:
			 *   - mute/unmute (UI Mixer toggle)              → zero VU
			 *   - audio_mixers change (Source > Advanced > Tracks)
			 *     → schedule rebuild so source enters/leaves visibility */
			signal_handler_t *sh = obs_source_get_signal_handler(audioSrc);
			if (sh) {
				signal_handler_connect(sh, "mute", source_mute_callback, svPtr);
				signal_handler_connect(sh, "audio_mixers", source_audio_mixers_callback, this);
			}

			cellVm->meters.push_back(std::move(sv));
			total_attached++;
			obs_source_release(audioSrc);
		}

		if (cellVm->meters.empty()) {
			delete cellVm;
			continue;
		}
		cell_volmeters_[i] = cellVm;
		cells_with_meters++;

		/* Append compact per-cell entry to the rebuild summary. cs.type
		 * is one of "pgm" / "prvw" / "scene" / "source". */
		char cellEntry[48];
		snprintf(cellEntry, sizeof(cellEntry), "%sc%zu:%s=%zu", cells_breakdown.empty() ? "" : " ", i,
			 cs.type.c_str(), cellVm->meters.size());
		cells_breakdown += cellEntry;
	}

	/* Drain any rebuild flag set during construction (e.g. by audio_mixers
	 * signal firing while we were attaching) — we just rebuilt, so it's
	 * already absorbed. */
	volmeters_rebuild_requested_.store(false, std::memory_order_relaxed);

	/* Refresh the active-source snapshot used by the polling pathway in
	 * check_active_track_change(). Recompute (instead of populating from
	 * the loop above) to ensure it reflects the same dedup+filter logic
	 * the poll uses, so subsequent equality compares are stable. */
	collect_active_source_pointers(last_active_sources_, current_track_bit_);
	/* Reset poll timestamp so the first poll after a rebuild waits the
	 * full interval (no immediate redundant rebuild). */
	last_track_poll_ns_ = os_gettime_ns();

	/* Track current PGM/PRVW scenes for change detection */
	if (has_pgm_cell_) {
		OBSSourceAutoRelease pgm = amv_frontend::current_program_scene();
		last_pgm_scene_ = pgm ? OBSGetWeakRef(pgm) : nullptr;
	}
	if (has_prvw_cell_) {
		OBSSourceAutoRelease prvw = amv_frontend::current_preview_scene();
		last_prvw_scene_ = prvw ? OBSGetWeakRef(prvw) : nullptr;
	}

	/* Single summary line — replaces the per-source LOG_INFO that used to
	 * fire inside the attach loop. With 1Hz active-source polling triggering
	 * rebuilds on any scene-tree mutation, per-source logs flooded the OBS
	 * log; one line per rebuild is sufficient for diagnostics. The instance
	 * prefix `[name(uuid8)]` lets logs stay readable when several Multiview
	 * windows are open simultaneously.
	 *
	 * Phase 3 hardening tail: rebuilds still fire on every scene-tree edit,
	 * which can be busy during normal mixing; gate the summary behind
	 * Detailed logs so steady-state operation does not spam the log. */
	amv_log_detailed(LOG_INFO, "[%s(%s)] VU meters rebuilt: cells=%d sources=%d track_bit=0x%x%s%s",
			 inst_name.empty() ? "?" : inst_name.c_str(), short_uuid.empty() ? "?" : short_uuid.c_str(),
			 cells_with_meters, total_attached, current_track_bit_, cells_breakdown.empty() ? "" : " | ",
			 cells_breakdown.c_str());

	/* Aggregated MAX_DEPTH warning (one line per rebuild listing every
	 * affected cell) — keeps signal-to-noise high when a deeply nested
	 * scene appears in many cells of the same window. */
	if (!depth_warn_cells.empty()) {
		obs_log(LOG_WARNING,
			"[%s(%s)] VU meter scene walk hit MAX_DEPTH=%d in cells [%s]; some nested sources may be skipped",
			inst_name.empty() ? "?" : inst_name.c_str(), short_uuid.empty() ? "?" : short_uuid.c_str(),
			AudioCollectCtx::MAX_DEPTH, depth_warn_cells.c_str());
	}
}

void AmvInstanceCore::check_scene_change_for_volmeters()
{
	bool needRebuild = false;

	if (has_pgm_cell_) {
		OBSSourceAutoRelease pgm = amv_frontend::current_program_scene();
		OBSWeakSource currentWeak = pgm ? OBSGetWeakRef(pgm) : nullptr;
		if (currentWeak != last_pgm_scene_)
			needRebuild = true;
	}
	if (!needRebuild && has_prvw_cell_) {
		OBSSourceAutoRelease prvw = amv_frontend::current_preview_scene();
		OBSWeakSource currentWeak = prvw ? OBSGetWeakRef(prvw) : nullptr;
		if (currentWeak != last_prvw_scene_)
			needRebuild = true;
	}

	if (needRebuild)
		rebuild_volmeters();
}

/* Combined poll + deferred-rebuild handler called once per render frame.
 *
 * Triggers (in priority order):
 *   1. volmeters_rebuild_requested_ flag set by source_audio_mixers_callback
 *      from any thread (signal fires when a source's Track 1..6 checkboxes
 *      change). We absorb the flag and rebuild on the render thread.
 *      NOTE: this signal only fires for sources we already attached. Sources
 *      that were filtered out (audio_mixers & active_bit == 0) have no
 *      subscriber, so the polling pathway below is required for them.
 *   2. ~1Hz polling of the active source pointer set. Catches:
 *        - newly-visible sources (Mic just got Track 1 ticked)
 *        - newly-added sceneitems (added a nested scene with audio)
 *        - newly-removed sceneitems
 *        - scene tree edits anywhere in the recursion path
 *      Cheap: O(N_audio_sources) tree walk, no allocations beyond the
 *      candidate vector, no signal subscriptions to manage.
 *   3. AutoFollow streaming-track mask change (Settings → Output → Streaming
 *      Audio Track has no event), checked alongside the source set poll. */
void AmvInstanceCore::check_active_track_change()
{
	const uint64_t now_ns = os_gettime_ns();
	/* Phase 3 / M5.4 hardening: throttle to at most ~4 Hz. Repeated delete +
	 * restore of a nested scene with audio sources fires source_create /
	 * source_remove storms; each one ends up setting
	 * volmeters_rebuild_requested_, and without this throttle the resulting
	 * release+rebuild churn against OBS audio thread has caused OBS hangs
	 * in field testing. Suppressed requests stay pending — flag is restored
	 * if it was already true so the very next allowed frame rebuilds. */
	const uint64_t MIN_REBUILD_INTERVAL_NS = 250000000ULL; /* 250 ms */
	const bool throttled = last_volmeter_rebuild_ns_ != 0 &&
			       (now_ns - last_volmeter_rebuild_ns_) < MIN_REBUILD_INTERVAL_NS;

	if (volmeters_rebuild_requested_.exchange(false, std::memory_order_acquire)) {
		if (throttled) {
			/* Re-arm the request so the next non-throttled frame
			 * picks it up; nothing else cares about the flag. */
			volmeters_rebuild_requested_.store(true, std::memory_order_release);
			return;
		}
		rebuild_volmeters();
		last_volmeter_rebuild_ns_ = now_ns;
		return; /* rebuild already refreshed current_track_bit_ + last_active_sources_ */
	}

	uint64_t now = now_ns;
	const uint64_t POLL_INTERVAL_NS = 1000000000ULL; /* 1 second */
	if (last_track_poll_ns_ != 0 && (now - last_track_poll_ns_) < POLL_INTERVAL_NS)
		return;
	last_track_poll_ns_ = now;

	uint32_t newBit = compute_active_track_bit();
	if (newBit != current_track_bit_) {
		if (!throttled) {
			rebuild_volmeters();
			last_volmeter_rebuild_ns_ = now_ns;
		} else {
			volmeters_rebuild_requested_.store(true, std::memory_order_release);
		}
		return;
	}

	/* Compare the currently-eligible source set against the snapshot from
	 * the last rebuild. Any difference (gained/lost source) means a rebuild
	 * is needed. This is the catch-all for cases where signal-based
	 * notification is missing (Mic gaining Track 1 from outside, scene
	 * tree edits, etc.). */
	std::vector<void *> currentActive;
	collect_active_source_pointers(currentActive, newBit);
	if (currentActive != last_active_sources_) {
		if (!throttled) {
			rebuild_volmeters();
			last_volmeter_rebuild_ns_ = now_ns;
		} else {
			volmeters_rebuild_requested_.store(true, std::memory_order_release);
		}
	}
}

/* Enumerate the set of audio source pointers that *would* be attached if we
 * rebuilt right now, given the active track bit. Identity-only — does not
 * retain references; we acquire/release internally to keep parity with
 * collect_audio_sources(). Result vector is sorted+deduped for set compare.
 *
 * Called from:
 *   - rebuild_volmeters() to record the snapshot used by polling
 *   - check_active_track_change() to compare against that snapshot
 */
void AmvInstanceCore::collect_active_source_pointers(std::vector<void *> &out, uint32_t track_bit)
{
	out.clear();
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	for (size_t i = 0; i < cell_sources_.size(); i++) {
		const auto &cs = cell_sources_[i];
		if (cs.type.empty())
			continue;

		obs_source_t *cellSrc = nullptr;
		bool isPgm = false;
		if (cs.type == "pgm") {
			/* Owned +1 to match the obs_source_release below (see rebuild path). */
			cellSrc = obs_source_get_ref(amv_frontend::current_program_scene());
			isPgm = true;
		} else if (cs.type == "prvw") {
			cellSrc = obs_source_get_ref(amv_frontend::current_preview_scene());
			if (!cellSrc) {
				/* Studio Mode off: PRVW mirrors PGM. Match rebuild path so
				 * the polling-based set comparison stays consistent across
				 * studio-mode toggles. */
				cellSrc = obs_source_get_ref(amv_frontend::current_program_scene());
				isPgm = (cellSrc != nullptr);
			}
		} else {
			OBSSourceAutoRelease strong = OBSGetStrongRef(cs.weak_ref);
			if (strong)
				cellSrc = obs_source_get_ref(strong);
		}
		if (!cellSrc)
			continue;

		std::vector<obs_source_t *> srcs;
		collect_audio_sources(cellSrc, srcs, track_bit);
		obs_source_release(cellSrc);

		/* PGM-only global devices (channel 1..5), filtered by track. */
		if (isPgm) {
			for (int ch = 1; ch <= 5; ch++) {
				obs_source_t *g = obs_get_output_source(ch);
				if (!g)
					continue;
				uint32_t am = obs_source_get_audio_mixers(g);
				if ((am & track_bit) != 0) {
					/* Dedup against scene-collected sources by pointer. */
					bool dup = false;
					for (auto *existing : srcs) {
						if (existing == g) {
							dup = true;
							break;
						}
					}
					if (!dup)
						out.push_back(g);
				}
				obs_source_release(g);
			}
		}

		for (auto *s : srcs) {
			out.push_back(s);
			obs_source_release(s); /* identity-only, drop the +1 ref */
		}
	}

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
}

void AmvInstanceCore::release_volmeters()
{
	/* Protect cell_volmeters_ vector against concurrent read from
	 * the render thread (render_vu_meter()). source_mutex_ is recursive
	 * so callers that already hold it (e.g. rebuild_volmeters() from
	 * within render()) are unaffected. */
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);
	for (auto *cellVm : cell_volmeters_) {
		if (!cellVm)
			continue;
		for (auto &sv : cellVm->meters) {
			if (!sv)
				continue;
			/* Disconnect per-source signal handlers BEFORE destroying
			 * the volmeter so the callbacks can no longer fire on the
			 * about-to-be-freed SingleVolmeter / window pointer.
			 *
			 * If OBSGetStrongRef returns null the source has already
			 * been destroyed, in which case its signal_handler_t was
			 * destroyed with it — handlers cannot fire, so skipping
			 * disconnect is safe by construction. */
			OBSSourceAutoRelease src = OBSGetStrongRef(sv->source_weak);
			if (src) {
				signal_handler_t *sh = obs_source_get_signal_handler(src);
				if (sh) {
					signal_handler_disconnect(sh, "mute", source_mute_callback, sv.get());
					signal_handler_disconnect(sh, "audio_mixers", source_audio_mixers_callback,
								  this);
				}
			}
			if (sv->volmeter) {
				obs_volmeter_remove_callback(sv->volmeter, volmeter_callback, sv.get());
				obs_volmeter_detach_source(sv->volmeter);
				obs_volmeter_destroy(sv->volmeter);
			}
		}
		delete cellVm;
	}
	cell_volmeters_.clear();
}

void AmvInstanceCore::render_vu_meter(int cellIndex, const CellRect &cell, int vpX, int vpY, int sigX, int sigY,
				      int sigW, int sigH)
{
	if (cellIndex < 0 || cellIndex >= (int)effective_visuals_.size())
		return;

	const VuMeterSettings &vmSettings = effective_visuals_[cellIndex].vuMeter;
	if (!vmSettings.enabled || vmSettings.opacity <= 0.0)
		return;

	/* Phase 3 / M5: skip rendering when the cell is not Active so a deleted
	 * scene/source stops dragging stale meters. cell_volmeters_[i] may still
	 * hold a volmeter attached to a now-dead source until the next rebuild,
	 * but state is updated each frame from render() so this branch wins
	 * immediately — no need to wait for the 1Hz active-source poll. */
	if (cellIndex < (int)cell_sources_.size()) {
		const auto &cs = cell_sources_[cellIndex];
		if (cs.state != SignalRuntimeState::Active)
			return;
	}

	if (cellIndex >= (int)cell_volmeters_.size() || !cell_volmeters_[cellIndex])
		return;

	CellVolmeter *cellVm = cell_volmeters_[cellIndex];

	/* Compute max peak across all audio sources in this cell.
	 * If a source's callback hasn't fired in 200ms, treat it as silent
	 * (source may have become inactive after a scene switch). */
	uint64_t now = os_gettime_ns();
	const uint64_t STALE_THRESHOLD_NS = 200000000ULL; /* 200ms */

	float peakMax = VU_SILENCE_DB;
	float magnitudeMax = VU_SILENCE_DB;
	float magnitudeByChannel[MAX_AUDIO_CHANNELS];
	float peakByChannel[MAX_AUDIO_CHANNELS];
	for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
		magnitudeByChannel[ch] = VU_SILENCE_DB;
		peakByChannel[ch] = VU_SILENCE_DB;
	}
	int detectedChannels = 1;
	for (auto &sv : cellVm->meters) {
		if (!sv)
			continue;
		/* User muted via Mixer: contribute zero to PGM → zero to VU.
		 * volmeter callback keeps streaming raw post-fader peak even when
		 * UI-muted (only PTT/PTM auto-mute zeroes it inside OBS), so we
		 * enforce WYSIWYG here. */
		if (sv->user_muted.load(std::memory_order_relaxed))
			continue;
		/* Check if this meter's data is stale */
		if (sv->last_callback_ns == 0 || (now - sv->last_callback_ns) > STALE_THRESHOLD_NS)
			continue;

		int ch = sv->channels > 0 ? sv->channels : 2;
		if (ch > detectedChannels)
			detectedChannels = ch;
		for (int c = 0; c < ch && c < MAX_AUDIO_CHANNELS; c++) {
			float m = sv->magnitude[c];
			if (std::isfinite(m)) {
				if (m > magnitudeMax)
					magnitudeMax = m;
				if (m > magnitudeByChannel[c])
					magnitudeByChannel[c] = m;
			}

			float p = sv->peak[c];
			if (std::isfinite(p)) {
				if (p > peakMax)
					peakMax = p;
				if (p > peakByChannel[c])
					peakByChannel[c] = p;
			}
		}
	}
	if (detectedChannels < 1)
		detectedChannels = 1;
	if (detectedChannels > MAX_AUDIO_CHANNELS)
		detectedChannels = MAX_AUDIO_CHANNELS;

	/* Clamp and normalize: -60 dB .. 0 dB -> 0.0 .. 1.0 */
	const float minDB = -60.0f;
	const float maxDB = 0.0f;

	/* Apply ballistics: immediate attack, gradual decay
	 * Decay rates (matching OBS): Fast=23.5, Medium=11.76, Slow=8.57 dB/s */
	float decayRate;
	switch (vmSettings.decayRate) {
	case VuMeterDecayRate::Medium:
		decayRate = 11.76f;
		break;
	case VuMeterDecayRate::Slow:
		decayRate = 8.57f;
		break;
	default:
		decayRate = 23.5f;
		break;
	}

	double renderDeltaS = 0.0;
	bool validRenderDelta = false;
	if (cellVm->last_render_ns > 0) {
		renderDeltaS = (double)(now - cellVm->last_render_ns) * 1e-9;
		validRenderDelta = renderDeltaS > 0.0 && renderDeltaS < 1.0;
	}

	if (cellVm->last_render_ns > 0) {
		if (validRenderDelta) {
			if (peakMax >= cellVm->displayPeak) {
				cellVm->displayPeak = peakMax;
			} else {
				cellVm->displayPeak -= decayRate * (float)renderDeltaS;
				if (cellVm->displayPeak < peakMax)
					cellVm->displayPeak = peakMax;
			}
		} else {
			cellVm->displayPeak = peakMax;
		}
	} else {
		cellVm->displayPeak = peakMax;
	}

	if (cellVm->last_render_ns > 0 && validRenderDelta) {
		float attack = (float)((magnitudeMax - cellVm->displayMagnitude) * (renderDeltaS / 0.3) * 0.99);
		cellVm->displayMagnitude += attack;
		if (cellVm->displayMagnitude < minDB)
			cellVm->displayMagnitude = minDB;
		if (cellVm->displayMagnitude > maxDB)
			cellVm->displayMagnitude = maxDB;
	} else {
		cellVm->displayMagnitude = magnitudeMax;
	}
	cellVm->last_render_ns = now;

	/* ---- Peak Hold ballistic ---- */
	float holdLevel = 0.0f;
	if (vmSettings.peakHoldEnabled) {
		if (peakMax > cellVm->holdPeak) {
			cellVm->holdPeak = peakMax;
			cellVm->holdSetAtNs = now;
		} else if (cellVm->holdSetAtNs > 0) {
			uint64_t holdNs = (uint64_t)vmSettings.peakHoldMs * 1000000ULL;
			if (now - cellVm->holdSetAtNs > holdNs) {
				double elapsedSinceHold = (double)(now - cellVm->holdSetAtNs - holdNs) * 1e-9;
				cellVm->holdPeak -= (float)(vmSettings.peakHoldDecayDbPerSec * elapsedSinceHold);
				if (cellVm->holdPeak < VU_SILENCE_DB)
					cellVm->holdPeak = VU_SILENCE_DB;
				if (cellVm->holdPeak < peakMax) {
					cellVm->holdPeak = peakMax;
					cellVm->holdSetAtNs = now;
				}
			}
		}
		float hp = cellVm->holdPeak;
		if (hp < minDB)
			hp = minDB;
		if (hp > maxDB)
			hp = maxDB;
		holdLevel = (hp - minDB) / (maxDB - minDB);
	}

	float smoothedPeak = cellVm->displayPeak;
	if (smoothedPeak < minDB)
		smoothedPeak = minDB;
	if (smoothedPeak > maxDB)
		smoothedPeak = maxDB;
	float level = (smoothedPeak - minDB) / (maxDB - minDB);

	float smoothedMagnitude = cellVm->displayMagnitude;
	if (smoothedMagnitude < minDB)
		smoothedMagnitude = minDB;
	if (smoothedMagnitude > maxDB)
		smoothedMagnitude = maxDB;
	float magnitudeLevel = (smoothedMagnitude - minDB) / (maxDB - minDB);

	/* Determine bar geometry based on position and anchor mode.
	 * Computed before the early-return so that scale ticks can always render. */
	int barW = vmSettings.width;
	int anchorX, anchorY, anchorW, anchorH;

	if (vmSettings.anchor == VuMeterAnchorMode::Signal) {
		/* Signal mode: render within the signal/video rect */
		anchorX = sigX;
		anchorY = sigY;
		anchorW = sigW;
		anchorH = sigH;
	} else {
		/* Cell mode: render within the full cell rect */
		anchorX = vpX + cell.x;
		anchorY = vpY + cell.y;
		anchorW = cell.w;
		anchorH = cell.h;
	}

	/* Color zones using custom dB thresholds:
	 * warningDB and errorDB normalized to 0..1 range on the -60..0 dB scale */
	float warningNorm = (float)(vmSettings.warningDB - minDB) / (maxDB - minDB);
	float errorNorm = (float)(vmSettings.errorDB - minDB) / (maxDB - minDB);
	if (warningNorm < 0.0f)
		warningNorm = 0.0f;
	if (warningNorm > 1.0f)
		warningNorm = 1.0f;
	if (errorNorm < warningNorm)
		errorNorm = warningNorm;
	if (errorNorm > 1.0f)
		errorNorm = 1.0f;

	/* Colors in ARGB format */
	uint32_t alpha = (uint32_t)(vmSettings.opacity * 255.0 + 0.5);
	if (alpha > 255)
		alpha = 255;
	uint32_t greenColor = (alpha << 24) | 0x0026A826;  /* green */
	uint32_t yellowColor = (alpha << 24) | 0x00D4D416; /* yellow */
	uint32_t redColor = (alpha << 24) | 0x00D41616;    /* red */
	uint32_t magnitudeColor = (alpha << 24) | 0x00000000;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	/* Draw up to 3 segments: green, yellow, red (as needed based on level) */
	struct Segment {
		float start; /* normalized 0..1 */
		float end;
		uint32_t color;
	};
	Segment segments[3] = {
		{0.0f, warningNorm, greenColor},
		{warningNorm, errorNorm, yellowColor},
		{errorNorm, 1.0f, redColor},
	};

	bool isHorizontal =
		(vmSettings.position == VuMeterPosition::Bottom || vmSettings.position == VuMeterPosition::Top);

	/* Apply length ratio and alignment */
	int barFullLen;
	int barX, barY;
	if (isHorizontal) {
		barFullLen = (int)(anchorW * vmSettings.lengthRatio + 0.5);
		int offset;
		if (vmSettings.alignment == VuMeterAlignment::Start) {
			/* Anchor the -∞ root to the cell edge it actually renders against.
			   Normal: root on left -> left-align. Flip: root on right -> right-align. */
			offset = vmSettings.flip ? (anchorW - barFullLen) : 0;
		} else {
			offset = (anchorW - barFullLen) / 2; /* center */
		}
		barX = anchorX + offset;
		barY = (vmSettings.position == VuMeterPosition::Top) ? anchorY : anchorY + anchorH - barW;
	} else {
		barFullLen = (int)(anchorH * vmSettings.lengthRatio + 0.5);
		int offset;
		if (vmSettings.alignment == VuMeterAlignment::Start) {
			/* Normal: root on bottom -> bottom-align. Flip: root on top -> top-align. */
			offset = vmSettings.flip ? 0 : (anchorH - barFullLen);
		} else {
			offset = (anchorH - barFullLen) / 2; /* center */
		}
		barY = anchorY + offset;
		barX = (vmSettings.position == VuMeterPosition::Left) ? anchorX : anchorX + anchorW - barW;
	}

	if (barFullLen <= 0) {
		gs_blend_state_pop();
		return;
	}

	auto draw_meter_lane = [&](int laneX, int laneY, int laneThickness, float laneLevel, float laneHoldLevel,
				   float laneMagnitudeLevel) {
		if (laneThickness <= 0 || (laneLevel <= 0.0f && laneHoldLevel <= 0.0f && laneMagnitudeLevel <= 0.0f))
			return;
		if (laneLevel > 0.0f || laneHoldLevel > 0.0f) {
			for (int s = 0; s < 3; s++) {
				float segStart = segments[s].start;
				float segEnd = segments[s].end;

				/* Clip segment to actual level */
				if (laneLevel <= segStart)
					break;
				float drawEnd = (laneLevel < segEnd) ? laneLevel : segEnd;

				int pixStart = (int)(segStart * (float)barFullLen + 0.5f);
				int pixEnd = (int)(drawEnd * (float)barFullLen + 0.5f);
				int pixLen = pixEnd - pixStart;
				if (pixLen <= 0)
					continue;

				gs_effect_set_color(colorParam, segments[s].color);

				if (isHorizontal) {
					int drawX;
					if (vmSettings.flip) {
						/* Flip: 0dB on left, -∞ on right */
						drawX = barX + barFullLen - pixEnd;
					} else {
						/* Normal: -∞ on left, 0dB on right */
						drawX = barX + pixStart;
					}
					startRegion(drawX, laneY, pixLen, laneThickness, 0.0f, (float)pixLen, 0.0f,
						    (float)laneThickness);
					while (gs_effect_loop(solid, "Solid"))
						gs_draw_sprite(nullptr, 0, pixLen, laneThickness);
					endRegion();
				} else {
					int drawY;
					if (vmSettings.flip) {
						/* Flip: 0dB on top, -∞ on bottom */
						drawY = barY + pixStart;
					} else {
						/* Normal: -∞ on top (bottom-up), 0dB at bottom */
						drawY = barY + barFullLen - pixEnd;
					}
					startRegion(laneX, drawY, laneThickness, pixLen, 0.0f, (float)laneThickness,
						    0.0f, (float)pixLen);
					while (gs_effect_loop(solid, "Solid"))
						gs_draw_sprite(nullptr, 0, laneThickness, pixLen);
					endRegion();
				}
			}

			/* ---- Peak Hold marker ---- */
			if (vmSettings.peakHoldEnabled && laneHoldLevel > 0.0f) {
				int holdWidthPx = vmSettings.peakHoldWidthPx;
				int holdPos = (int)(laneHoldLevel * (float)barFullLen + 0.5f);
				if (holdPos > barFullLen)
					holdPos = barFullLen;

				/* Determine color from dB zone */
				uint32_t holdColor;
				if (laneHoldLevel >= errorNorm)
					holdColor = redColor;
				else if (laneHoldLevel >= warningNorm)
					holdColor = yellowColor;
				else
					holdColor = greenColor;

				gs_effect_set_color(colorParam, holdColor);

				if (isHorizontal) {
					int hx;
					if (vmSettings.flip)
						hx = barX + barFullLen - holdPos;
					else
						hx = barX + holdPos - holdWidthPx;
					if (hx < barX)
						hx = barX;
					startRegion(hx, laneY, holdWidthPx, laneThickness, 0.0f, (float)holdWidthPx,
						    0.0f, (float)laneThickness);
					while (gs_effect_loop(solid, "Solid"))
						gs_draw_sprite(nullptr, 0, holdWidthPx, laneThickness);
					endRegion();
				} else {
					int hy;
					if (vmSettings.flip)
						hy = barY + holdPos - holdWidthPx;
					else
						hy = barY + barFullLen - holdPos;
					if (hy < barY)
						hy = barY;
					startRegion(laneX, hy, laneThickness, holdWidthPx, 0.0f, (float)laneThickness,
						    0.0f, (float)holdWidthPx);
					while (gs_effect_loop(solid, "Solid"))
						gs_draw_sprite(nullptr, 0, laneThickness, holdWidthPx);
					endRegion();
				}
			}
		}

		if (laneMagnitudeLevel > 0.0f) {
			int markerPx = 5;
			if (markerPx > barFullLen)
				markerPx = barFullLen;
			int magPos = (int)(laneMagnitudeLevel * (float)barFullLen + 0.5f);
			if (magPos > barFullLen)
				magPos = barFullLen;

			gs_effect_set_color(colorParam, magnitudeColor);
			if (isHorizontal) {
				int mx = vmSettings.flip ? barX + barFullLen - magPos : barX + magPos - markerPx;
				if (mx < barX)
					mx = barX;
				if (mx > barX + barFullLen - markerPx)
					mx = barX + barFullLen - markerPx;
				startRegion(mx, laneY, markerPx, laneThickness, 0.0f, (float)markerPx, 0.0f,
					    (float)laneThickness);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, markerPx, laneThickness);
				endRegion();
			} else {
				int my = vmSettings.flip ? barY + magPos - markerPx : barY + barFullLen - magPos;
				if (my < barY)
					my = barY;
				if (my > barY + barFullLen - markerPx)
					my = barY + barFullLen - markerPx;
				startRegion(laneX, my, laneThickness, markerPx, 0.0f, (float)laneThickness, 0.0f,
					    (float)markerPx);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, laneThickness, markerPx);
				endRegion();
			}
		}
	};

	/* Only draw bar segments and peak hold when there is actual signal.
	 * Scale ticks/labels always render (below this block). */
	if (vmSettings.multiChannelEnabled && detectedChannels > 1) {
		float channelMagnitudeLevels[MAX_AUDIO_CHANNELS];
		float channelLevels[MAX_AUDIO_CHANNELS];
		float channelHoldLevels[MAX_AUDIO_CHANNELS];
		for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
			float chMagnitude = magnitudeByChannel[ch];
			float chPeak = peakByChannel[ch];
			double channelDeltaS = 0.0;
			bool validChannelDelta = false;
			if (cellVm->channelLastRenderNs[ch] > 0) {
				channelDeltaS = (double)(now - cellVm->channelLastRenderNs[ch]) * 1e-9;
				validChannelDelta = channelDeltaS > 0.0 && channelDeltaS < 1.0;
			}

			if (cellVm->channelLastRenderNs[ch] > 0) {
				if (validChannelDelta) {
					if (chPeak >= cellVm->channelDisplayPeak[ch]) {
						cellVm->channelDisplayPeak[ch] = chPeak;
					} else {
						cellVm->channelDisplayPeak[ch] -= decayRate * (float)channelDeltaS;
						if (cellVm->channelDisplayPeak[ch] < chPeak)
							cellVm->channelDisplayPeak[ch] = chPeak;
					}
				} else {
					cellVm->channelDisplayPeak[ch] = chPeak;
				}
			} else {
				cellVm->channelDisplayPeak[ch] = chPeak;
			}

			if (cellVm->channelLastRenderNs[ch] > 0 && validChannelDelta) {
				float attack = (float)((chMagnitude - cellVm->channelDisplayMagnitude[ch]) *
						       (channelDeltaS / 0.3) * 0.99);
				cellVm->channelDisplayMagnitude[ch] += attack;
				if (cellVm->channelDisplayMagnitude[ch] < minDB)
					cellVm->channelDisplayMagnitude[ch] = minDB;
				if (cellVm->channelDisplayMagnitude[ch] > maxDB)
					cellVm->channelDisplayMagnitude[ch] = maxDB;
			} else {
				cellVm->channelDisplayMagnitude[ch] = chMagnitude;
			}
			cellVm->channelLastRenderNs[ch] = now;

			float chHoldLevel = 0.0f;
			if (vmSettings.peakHoldEnabled) {
				if (chPeak > cellVm->channelHoldPeak[ch]) {
					cellVm->channelHoldPeak[ch] = chPeak;
					cellVm->channelHoldSetAtNs[ch] = now;
				} else if (cellVm->channelHoldSetAtNs[ch] > 0) {
					uint64_t holdNs = (uint64_t)vmSettings.peakHoldMs * 1000000ULL;
					if (now - cellVm->channelHoldSetAtNs[ch] > holdNs) {
						double elapsedSinceHold =
							(double)(now - cellVm->channelHoldSetAtNs[ch] - holdNs) * 1e-9;
						cellVm->channelHoldPeak[ch] -=
							(float)(vmSettings.peakHoldDecayDbPerSec * elapsedSinceHold);
						if (cellVm->channelHoldPeak[ch] < VU_SILENCE_DB)
							cellVm->channelHoldPeak[ch] = VU_SILENCE_DB;
						if (cellVm->channelHoldPeak[ch] < chPeak) {
							cellVm->channelHoldPeak[ch] = chPeak;
							cellVm->channelHoldSetAtNs[ch] = now;
						}
					}
				}
				float hp = cellVm->channelHoldPeak[ch];
				if (hp < minDB)
					hp = minDB;
				if (hp > maxDB)
					hp = maxDB;
				chHoldLevel = (hp - minDB) / (maxDB - minDB);
			}

			float chSmoothed = cellVm->channelDisplayPeak[ch];
			if (chSmoothed < minDB)
				chSmoothed = minDB;
			if (chSmoothed > maxDB)
				chSmoothed = maxDB;
			float chMagSmoothed = cellVm->channelDisplayMagnitude[ch];
			if (chMagSmoothed < minDB)
				chMagSmoothed = minDB;
			if (chMagSmoothed > maxDB)
				chMagSmoothed = maxDB;
			channelMagnitudeLevels[ch] = (chMagSmoothed - minDB) / (maxDB - minDB);
			channelLevels[ch] = (chSmoothed - minDB) / (maxDB - minDB);
			channelHoldLevels[ch] = chHoldLevel;
		}

		const int totalThickness = (std::max)(1, barW);
		const int channelCount = (std::min)(detectedChannels, MAX_AUDIO_CHANNELS);
		const int baseLane = totalThickness / channelCount;
		int remainder = totalThickness % channelCount;
		int offset = 0;
		for (int ch = 0; ch < channelCount; ch++) {
			int laneThickness = baseLane + (remainder > 0 ? 1 : 0);
			if (remainder > 0)
				remainder--;
			if (laneThickness <= 0)
				continue;
			if (isHorizontal)
				draw_meter_lane(barX, barY + offset, laneThickness, channelLevels[ch],
						channelHoldLevels[ch], channelMagnitudeLevels[ch]);
			else
				draw_meter_lane(barX + offset, barY, laneThickness, channelLevels[ch],
						channelHoldLevels[ch], channelMagnitudeLevels[ch]);
			offset += laneThickness;
		}
	} else {
		draw_meter_lane(barX, barY, barW, level, holdLevel, magnitudeLevel);
	}

	/* ---- dB Scale ticks ---- */
	if (vmSettings.scaleEnabled) {
		/* Parse scale ticks from CSV string, fallback to default set */
		std::vector<float> ticks;
		std::string tickStr = vmSettings.scaleTicks;
		if (tickStr.empty())
			tickStr = "-60,-40,-20,-9,0";

		/* Simple CSV parse (cap at 20 ticks to prevent pathological configs) */
		{
			size_t pos = 0;
			while (pos < tickStr.size() && ticks.size() < 20) {
				size_t comma = tickStr.find(',', pos);
				if (comma == std::string::npos)
					comma = tickStr.size();
				std::string token = tickStr.substr(pos, comma - pos);
				pos = comma + 1;
				try {
					float val = std::stof(token);
					if (val >= -96.0f && val <= 0.0f)
						ticks.push_back(val);
				} catch (...) {
					/* skip invalid tokens */
				}
			}
			if (ticks.empty()) {
				ticks = {-60.0f, -40.0f, -20.0f, -9.0f, 0.0f};
			}
		}

		/* Scale tick geometry: tick length matches full bar width */
		int tickLen = barW;
		if (tickLen < 2)
			tickLen = 2;

		/* Determine which side to draw ticks on.
		 * Auto: opposite side of the bar (away from cell edge).
		 * Same: overlapping the bar itself.
		 * Opposite: explicitly on the non-bar side. */
		bool tickOnBarSide = (vmSettings.scaleSide == VuMeterScaleSide::Same);
		/* For Auto / Opposite: tick on the opposite side of the bar's edge */

		/* Scale ticks and labels always render at full alpha so they remain
		 * visible when drawn on top of the VU bar (Same side mode). */
		uint32_t tickColor = (vmSettings.scaleColor & 0x00FFFFFF) | 0xFF000000;

		gs_effect_set_color(colorParam, tickColor);

		for (float tickDB : ticks) {
			float tickNorm = (tickDB - minDB) / (maxDB - minDB);
			if (tickNorm < 0.0f || tickNorm > 1.0f)
				continue;
			int tickPos = (int)(tickNorm * (float)barFullLen + 0.5f);

			int tickDrawX = 0, tickDrawY = 0;

			if (isHorizontal) {
				int tx;
				if (vmSettings.flip)
					tx = barX + barFullLen - tickPos;
				else
					tx = barX + tickPos;
				int ty;
				if (vmSettings.position == VuMeterPosition::Top) {
					ty = tickOnBarSide ? barY : barY + barW;
				} else {
					ty = tickOnBarSide ? barY + barW - tickLen : barY - tickLen;
				}
				if (ty < 0)
					ty = 0;
				tickDrawX = tx;
				tickDrawY = ty;
				startRegion(tx, ty, 1, tickLen, 0.0f, 1.0f, 0.0f, (float)tickLen);
				gs_effect_set_color(colorParam, tickColor);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, 1, tickLen);
				endRegion();
			} else {
				int ty;
				if (vmSettings.flip)
					ty = barY + tickPos;
				else
					ty = barY + barFullLen - tickPos;
				int tx;
				if (vmSettings.position == VuMeterPosition::Left) {
					tx = tickOnBarSide ? barX : barX + barW;
				} else {
					tx = tickOnBarSide ? barX + barW - tickLen : barX - tickLen;
				}
				if (tx < 0)
					tx = 0;
				tickDrawX = tx;
				tickDrawY = ty;
				startRegion(tx, ty, tickLen, 1, 0.0f, (float)tickLen, 0.0f, 1.0f);
				gs_effect_set_color(colorParam, tickColor);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, tickLen, 1);
				endRegion();
			}

			/* ---- Render dB label text ---- */
			if (vmSettings.scaleShowLabels) {
				int dbTenths = (int)(tickDB * 10.0f + (tickDB < 0 ? -0.5f : 0.5f));
				for (auto &entry : scale_label_cache_) {
					if (entry.dbTenths != dbTenths || entry.fontFamily != vmSettings.fontFamily)
						continue;
					if (!entry.source)
						break;
					uint32_t tw = obs_source_get_width(entry.source);
					uint32_t th = obs_source_get_height(entry.source);
					if (tw == 0 || th == 0)
						break;

					/* Clamp render size to barW so label never exceeds meter width */
					int renderW = (int)tw;
					int renderH = (int)th;
					if (renderW > barW) {
						renderH = renderH * barW / renderW;
						renderW = barW;
					}
					if (renderW <= 0 || renderH <= 0)
						break;

					/* Label always below the tick line (vertical bar: below tick,
					 * horizontal bar: below tick). Centered on tick position. */
					int lx, ly;
					if (isHorizontal) {
						lx = tickDrawX - renderW / 2;
						ly = tickDrawY + tickLen + 1;
					} else {
						/* For vertical bar: label below the tick line,
						 * centered horizontally on the tick */
						lx = tickDrawX + (tickLen - renderW) / 2;
						ly = tickDrawY + 2;
					}
					if (lx < 0)
						lx = 0;
					if (ly < 0)
						ly = 0;

					startRegion(lx, ly, renderW, renderH, 0.0f, (float)tw, 0.0f, (float)th);
					obs_source_video_render(entry.source);
					endRegion();
					break;
				}
			}
		}
	}

	gs_blend_state_pop();
}
