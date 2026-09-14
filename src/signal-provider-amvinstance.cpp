/*
OBS Advanced Multiview - AMV-instance signal provider (issue #20 P2)

Wraps the hidden `amv_instance_source` obs type so a cell can host ANOTHER AMV
instance's composited picture (nested source). Unlike the external media providers
this wraps our OWN source type rather than a host plugin's, so is_available() is
gated only on our source having been registered (obs_module_load ->
register_amv_instance_source()).

create_private_source builds an `amv_instance_source` carrying the target instance
UUID + picture mode, exactly like the other providers build their private source.
It must NOT take any core / registry lock (called outside source_mutex_ by the
runtime, and the target core is resolved elsewhere on the graphics thread).

probe_health maps target availability onto the shared health state machine so a
deleted / not-yet-alive target drives the cell to the existing Lost path (design
§2.7, §5), with NO new state and NO auto media_restart / recreate (there is nothing
to restart — recovery happens when the target instance comes back and resumes
publishing, exactly like NDI/Spout wait for their sender).

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "signal-provider.hpp"
#include "multiview-instance.hpp"
#include "multiview-window.hpp"
#include "amv-logging.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <string>

namespace {

AmvInstanceCore::ConsumerPictureMode read_mode(obs_data_t *settings)
{
	const char *m = settings ? obs_data_get_string(settings, amv_nested::kPictureModeKey) : nullptr;
	if (m && *m && std::string(m) == "grid")
		return AmvInstanceCore::ConsumerPictureMode::GridOnly;
	return AmvInstanceCore::ConsumerPictureMode::Full;
}

class AmvInstanceProvider : public ISignalProvider {
public:
	SignalProviderType type() const override { return SignalProviderType::AmvInstance; }
	const char *id() const override { return signal_provider_to_string(type()); }
	const char *display_name() const override { return "AMV Instance"; }

	bool is_available() const override
	{
		/* Our own source type: available once obs_module_load registered it.
		 * Mirrors the ffmpeg availability probe (get_display_name != null). */
		return obs_source_get_display_name(amv_nested::kSourceId) != nullptr;
	}

	std::string unavailable_reason() const override
	{
		if (is_available())
			return std::string();
		return "The nested AMV instance source type is not registered.";
	}

	OBSSource create_private_source(const std::string &desired_name, const SignalConfig &cfg) const override
	{
		if (!is_available()) {
			obs_log(LOG_WARNING,
				"[signal-provider/amvinstance] create skipped for '%s': source type unavailable",
				desired_name.c_str());
			return OBSSource();
		}

		obs_data_t *src_settings = cfg.providerSettings;
		const char *target = src_settings ? obs_data_get_string(src_settings, amv_nested::kTargetUuidKey)
						  : nullptr;
		if (!target || !*target) {
			/* No target chosen — create anyway (the cell is occupied) but warn;
			 * probe_health will keep it Lost until a target is assigned. */
			obs_log(LOG_WARNING, "[signal-provider/amvinstance] create '%s' has no target UUID",
				desired_name.c_str());
		}

		/* Deep-copy the persisted settings so nothing leaks back into the config,
		 * then hand them to the private source verbatim (target UUID + mode). */
		obs_data_t *settings = ISignalProvider::deep_copy_provider_settings(src_settings);
		obs_source_t *raw = obs_source_create_private(amv_nested::kSourceId, desired_name.c_str(), settings);
		obs_data_release(settings);
		if (!raw) {
			obs_log(LOG_WARNING, "[signal-provider/amvinstance] obs_source_create_private failed for '%s'",
				desired_name.c_str());
			return OBSSource();
		}

		amv_log_detailed(LOG_INFO, "[signal-provider/amvinstance] created private source '%s' target='%s'",
				 desired_name.c_str(), (target && *target) ? target : "(none)");

		OBSSource wrapper(raw);
		obs_source_release(raw);
		return wrapper;
	}

	/* GRAPHICS THREAD (called from tick_external_cell_health inside draw_cells).
	 * Resolve the target from the graphics-lock snapshot and check whether it has
	 * a completed FRONT for the (R, mode) picture we sample. This is the SAME
	 * resolution R (canvas base) and mode the source's video_render demands, so a
	 * present front means the picture is genuinely renderable. */
	HealthReport probe_health(obs_source_t *src, uint64_t age_ns) const override
	{
		HealthReport r;
		if (!src)
			return r;

		/* A short grace after (re)creation so a freshly-assigned cell shows
		 * Connecting rather than a Lost flash while the UI-thread reconcile
		 * spins up the target core and it composes its first frame. Once past
		 * the grace, a missing / frame-less target is reported Lost promptly. */
		constexpr uint64_t kGraceNs = 3ULL * 1000 * 1000 * 1000;

		std::string uuid;
		AmvInstanceCore::ConsumerPictureMode mode = AmvInstanceCore::ConsumerPictureMode::Full;
		obs_data_t *settings = obs_source_get_settings(src);
		if (settings) {
			const char *u = obs_data_get_string(settings, amv_nested::kTargetUuidKey);
			uuid = (u && *u) ? u : "";
			mode = read_mode(settings);
			obs_data_release(settings);
		}
		if (uuid.empty()) {
			r.reason = "no target instance";
			r.code = (age_ns < kGraceNs) ? HealthCode::Opening : HealthCode::Lost;
			return r;
		}

		struct obs_video_info ovi;
		const bool have_canvas = obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0;

		AmvInstanceCore *target = have_canvas ? multiview_pull_target_graphics(uuid) : nullptr;
		if (target) {
			AmvInstanceCore::ConsumerFrame front =
				target->get_consumer_front(ovi.base_width, ovi.base_height, mode);
			if (front.texture) {
				r.code = HealthCode::Active;
				r.width = front.width;
				r.height = front.height;
				return r;
			}
			/* Target is alive but has not published a frame yet (first frames /
			 * momentarily idle). Opening while young, Lost once the grace ends. */
			r.reason = "target has no frame yet";
			r.code = (age_ns < kGraceNs) ? HealthCode::Opening : HealthCode::Lost;
			return r;
		}

		/* Target core not alive (instance deleted, or reconcile has not created
		 * it yet). Same young-grace treatment, then Lost. */
		r.reason = "target instance unavailable";
		r.code = (age_ns < kGraceNs) ? HealthCode::Opening : HealthCode::Lost;
		return r;
	}

	/* Nothing to restart/recreate on our side: recovery happens when the target
	 * instance returns and resumes publishing (mirrors NDI/Spout). The health
	 * supervisor's Lost path then just keeps painting SIGNAL LOST until Active. */
	bool supports_media_restart() const override { return false; }
	bool benefits_from_recreate() const override { return false; }
	bool prefers_unbuffered_async(const SignalConfig &) const override { return false; }
};

static AmvInstanceProvider g_amvinstance_provider;

} // namespace

void register_amvinstance_provider()
{
	SignalProviderRegistry::instance().register_provider(&g_amvinstance_provider);
}
