/*
OBS Advanced Multiview - Lost-Signal image management
Loads and renders the per-cell texture used when a cell's source is
unavailable (PlaceholderImage / fallback static image).

Single slot per cell; the actual image path is recomputed each rebuild
from the cell's runtime state + effective LostSignalSettings, so the slot
naturally switches between "placeholder" and "fallback static image" when
the state machine transitions.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "multiview-instance.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <plugin-support.h>

#include <algorithm>

/* ---- helper: resolve the path the cell currently wants (under lock) ---- */

std::string AmvInstanceCore::compute_wanted_lost_image_path(int cellIndex)
{
	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return std::string();

	const auto &cs = cell_sources_[cellIndex];

	/* Phase 3 / M6 step 10: classify the cell as internal-cell (legacy
	 * M5 path: type in {scene,source}; pgm/prvw never hit the lost-image
	 * pipeline because they always resolve), or external-provider cell
	 * (M6: type empty + provider_type non-internal). Empty cells return
	 * the empty path either way. */
	const bool is_internal_cell = !cs.type.empty() && cs.type != "pgm" && cs.type != "prvw";
	const bool is_external_cell = cs.provider_type != SignalProviderType::Unknown &&
				      !signal_provider_is_internal(cs.provider_type);
	if (!is_internal_cell && !is_external_cell)
		return std::string();

	/* Phase 3 / M5.1 ClearCell: cell is about to be cleared on the next
	 * Qt main-thread tick. Don't load placeholder / fallback art for it —
	 * that would just flash for the few frames before refresh_sources()
	 * rebuilds cell_sources_ and the slot disappears entirely. */
	if (cs.pending_clear)
		return std::string();

	/* Cells whose source resolves (Active / FallbackActive on an OBS
	 * source) draw the source video directly — no image.
	 *
	 * Phase 3 / M6.6 nuance: external cells now also map to
	 * FallbackActive when the user configured an *image* fallback and
	 * the unhealthy latch is engaged. In that case we DO need to draw
	 * the image. The distinction below: internal-cell or external-cell
	 * with a source-type fallback (PGM/Scene/Source) → src is the
	 * fallback source and the image renderer must stay silent; external-
	 * cell with image fallback → src is null, image renderer paints
	 * the user's art. We disambiguate by inspecting the configured
	 * fallback type rather than just the state code.
	 *
	 * Paused cells also keep painting the last decoded frame, so the
	 * lost-signal image must NOT replace it; otherwise pressing pause
	 * on a media cell would briefly swap the frozen frame for a
	 * static graphic which is exactly the surprise we're avoiding. */
	if (cs.state == SignalRuntimeState::Active || cs.state == SignalRuntimeState::Paused)
		return std::string();
	if (cs.state == SignalRuntimeState::FallbackActive) {
		const auto &fb_eff = cs.effective_lost;
		const bool image_fallback_active =
			is_external_cell && fb_eff.externalLostBehavior == ExternalLostBehavior::RetryWithFallback &&
			fb_eff.fallbackType == std::string("image") && !fb_eff.fallbackName.empty();
		if (!image_fallback_active)
			return std::string();
		/* fall through to the image lookup below */
	}

	const LostSignalSettings &eff = cs.effective_lost;

	if (is_internal_cell && cs.state == SignalRuntimeState::MissingInternal) {
		/* InternalMissingBehavior::PlaceholderImage takes precedence
		 * over the static-image fallback. Both are user-configured and
		 * Phase 3 § 7.2 lists placeholder explicitly under M5.1; the
		 * fallback static image (M5.4) only kicks in when the user
		 * picked Black for InternalMissingBehavior but still set
		 * fallbackType == "image". */
		if (eff.internalMissingBehavior == InternalMissingBehavior::PlaceholderImage &&
		    !eff.placeholderImagePath.empty())
			return eff.placeholderImagePath;
		if (eff.fallbackType == std::string("image") && !eff.fallbackName.empty())
			return eff.fallbackName;
	}

	/* Phase 3 / M6 step 10: external cell in Lost / Error / Connecting /
	 * RetryScheduled. The user's ExternalLostBehavior gates which (if
	 * any) image to show:
	 *
	 *   SignalLostImage   -> always show signalLostImagePath while not
	 *                        Active. This is the "I want a static
	 *                        graphic in place of the broken stream"
	 *                        choice.
	 *   RetryWithFallback -> if the user also picked fallbackType=image
	 *                        and gave a path, show it while waiting for
	 *                        recovery. The OBS-source fallbacks (pgm/
	 *                        prvw/scene/source) are handled by the live
	 *                        render path, not this image renderer.
	 *   SignalLostOverlay /
	 *   RetryOnly         -> no image, the overlay text alone covers
	 *                        the cell. Render returns empty path here.
	 *
	 * The status overlay (RECONNECTING / SIGNAL LOST band) sits on top
	 * of whatever this returns, so the user gets BOTH the explanation
	 * text and their custom graphic. */
	if (is_external_cell && cs.state != SignalRuntimeState::Empty) {
		if (eff.externalLostBehavior == ExternalLostBehavior::SignalLostImage &&
		    !eff.signalLostImagePath.empty())
			return eff.signalLostImagePath;
		if (eff.externalLostBehavior == ExternalLostBehavior::RetryWithFallback &&
		    eff.fallbackType == std::string("image") && !eff.fallbackName.empty())
			return eff.fallbackName;
	}

	return std::string();
}

/* ---- rebuild ---- */

void AmvInstanceCore::rebuild_lost_signal_images()
{
	/* Mirror rebuild_bg_images() exactly: 4-phase
	 *   1. snapshot intentions under lock
	 *   2. disk IO without lock
	 *   3. graphics ops (destroy + create) once
	 *   4. install textures under lock */

	struct ImageOp {
		size_t index;
		std::string newPath;
		gs_texture_t *oldTexture = nullptr;
	};
	std::vector<ImageOp> ops;
	std::vector<gs_texture_t *> textures_to_destroy;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		size_t cellCount = cell_sources_.size();

		while (lost_signal_images_.size() < cellCount)
			lost_signal_images_.push_back(LostSignalImage{});

		if (lost_signal_images_.size() > cellCount) {
			for (size_t i = cellCount; i < lost_signal_images_.size(); i++) {
				if (lost_signal_images_[i].texture)
					textures_to_destroy.push_back(lost_signal_images_[i].texture);
			}
			lost_signal_images_.resize(cellCount);
		}

		for (size_t i = 0; i < cellCount; i++) {
			std::string wantPath = compute_wanted_lost_image_path((int)i);

			if (lost_signal_images_[i].path == wantPath)
				continue;

			ImageOp op;
			op.index = i;
			op.newPath = wantPath;
			op.oldTexture = lost_signal_images_[i].texture;

			lost_signal_images_[i].texture = nullptr;
			lost_signal_images_[i].width = 0;
			lost_signal_images_[i].height = 0;
			lost_signal_images_[i].path = wantPath;

			if (op.oldTexture)
				textures_to_destroy.push_back(op.oldTexture);

			if (!wantPath.empty())
				ops.push_back(std::move(op));
		}
	}

	/* Phase 2: disk IO outside any lock. */
	struct LoadedImage {
		size_t index;
		gs_image_file_t imgFile = {};
		bool loaded = false;
	};
	std::vector<LoadedImage> loaded;
	loaded.reserve(ops.size());

	for (auto &op : ops) {
		LoadedImage li;
		li.index = op.index;
		gs_image_file_init(&li.imgFile, op.newPath.c_str());
		li.loaded = li.imgFile.loaded;
		if (!li.loaded)
			obs_log(LOG_WARNING, "%sfailed to load lost-signal image: %s", log_prefix().c_str(),
				op.newPath.c_str());
		loaded.push_back(std::move(li));
	}

	/* Phase 3: graphics ops. */
	if (!textures_to_destroy.empty() || !loaded.empty()) {
		obs_enter_graphics();
		for (auto *tex : textures_to_destroy)
			gs_texture_destroy(tex);
		for (auto &li : loaded) {
			if (li.loaded)
				gs_image_file_init_texture(&li.imgFile);
		}
		obs_leave_graphics();
	}

	/* Phase 4: install. */
	if (!loaded.empty()) {
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);
		for (auto &li : loaded) {
			if (li.loaded && li.imgFile.texture) {
				if (li.index < lost_signal_images_.size()) {
					lost_signal_images_[li.index].texture = li.imgFile.texture;
					lost_signal_images_[li.index].width = li.imgFile.cx;
					lost_signal_images_[li.index].height = li.imgFile.cy;
				}
				li.imgFile.texture = nullptr;
			}
		}
	}

	for (auto &li : loaded)
		gs_image_file_free(&li.imgFile);
}

void AmvInstanceCore::release_lost_signal_images()
{
	/* Same convention as release_bg_images: actual texture destruction is
	 * driven from release_source_refs() which already collects textures
	 * outside the mutex. We drop just the path entry so the next rebuild
	 * starts fresh. */
	std::vector<gs_texture_t *> textures_to_destroy;
	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);
		for (auto &li : lost_signal_images_) {
			if (li.texture)
				textures_to_destroy.push_back(li.texture);
			li.texture = nullptr;
			li.width = 0;
			li.height = 0;
			li.path.clear();
		}
		lost_signal_images_.clear();
	}
	if (!textures_to_destroy.empty()) {
		obs_enter_graphics();
		for (auto *tex : textures_to_destroy)
			gs_texture_destroy(tex);
		obs_leave_graphics();
	}
}

/* ---- per-cell render ---- */

void AmvInstanceCore::render_lost_signal_image(int cellIndex, int contentX, int contentY, int contentW, int contentH)
{
	if (cellIndex < 0 || cellIndex >= (int)lost_signal_images_.size())
		return;
	if (contentW <= 0 || contentH <= 0)
		return;

	const auto &li = lost_signal_images_[cellIndex];
	if (!li.texture || li.width == 0 || li.height == 0)
		return;

	/* Phase 3 / M5.4 + M6 step 10: pick fit mode for the slot the cell
	 * is currently showing. The slot is determined by exactly the same
	 * logic as compute_wanted_lost_image_path() but here we only need
	 * the *kind*, not the path — the texture is already loaded. */
	ImageFitMode fitMode = ImageFitMode::Stretch;
	if (cellIndex < (int)cell_sources_.size()) {
		const auto &cs = cell_sources_[cellIndex];
		const LostSignalSettings &eff = cs.effective_lost;

		const bool is_external_cell = cs.provider_type != SignalProviderType::Unknown &&
					      !signal_provider_is_internal(cs.provider_type);

		if (is_external_cell && eff.externalLostBehavior == ExternalLostBehavior::SignalLostImage) {
			fitMode = eff.signalLostImageFitMode;
		} else if (cs.state == SignalRuntimeState::MissingInternal &&
			   eff.internalMissingBehavior == InternalMissingBehavior::PlaceholderImage &&
			   !eff.placeholderImagePath.empty()) {
			fitMode = eff.placeholderImageFitMode;
		} else {
			fitMode = eff.fallbackImageFitMode;
		}
	}

	int drawX = contentX, drawY = contentY, drawW = contentW, drawH = contentH;
	if (fitMode == ImageFitMode::Stretch) {
		/* Fill the cell exactly — no bars, may distort aspect. Default
		 * for placeholder / fallback because most artwork is authored
		 * for the cell. */
	} else {
		/* Fit: preserve image aspect, letterbox / pillarbox as needed. */
		double imgAspect = (double)li.width / (double)li.height;
		double cellAspect = (double)contentW / (double)contentH;

		if (imgAspect > cellAspect) {
			drawW = contentW;
			drawH = (int)((double)contentW / imgAspect + 0.5);
			drawY = contentY + (contentH - drawH) / 2;
		} else {
			drawH = contentH;
			drawW = (int)((double)contentH * imgAspect + 0.5);
			drawX = contentX + (contentW - drawW) / 2;
		}
	}

	gs_effect_t *defEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *imgParam = gs_effect_get_param_by_name(defEffect, "image");
	gs_effect_set_texture(imgParam, li.texture);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f((float)drawX, (float)drawY, 0.0f);
	gs_matrix_scale3f((float)drawW / (float)li.width, (float)drawH / (float)li.height, 1.0f);

	while (gs_effect_loop(defEffect, "Draw"))
		gs_draw_sprite(li.texture, 0, li.width, li.height);

	gs_matrix_pop();
}
