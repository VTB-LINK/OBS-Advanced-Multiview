/*
OBS Advanced Multiview - Background and overlay image texture management
Split from multiview-window.cpp for maintainability.
All functions remain members of AmvInstanceCore.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
/* ---- Background image management ---- */

void AmvInstanceCore::rebuild_bg_images()
{
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(ref_vp_width(), ref_vp_height());
	tmpEngine.compute();

	size_t cellCount = tmpEngine.cells().size();

	/* Phase 1: determine what changed under lock, collect old textures */
	struct ImageOp {
		size_t index;
		std::string newPath;
		gs_texture_t *oldTexture = nullptr;
	};
	std::vector<ImageOp> ops;
	std::vector<gs_texture_t *> textures_to_destroy;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		while (bg_images_.size() < cellCount)
			bg_images_.push_back(BgImage{});

		/* Shrink: if grid shrank, collect textures from excess entries
		 * for destruction before erasing them. Without this the textures
		 * would leak (held by orphan entries past cellCount). */
		if (bg_images_.size() > cellCount) {
			for (size_t i = cellCount; i < bg_images_.size(); i++) {
				if (bg_images_[i].texture)
					textures_to_destroy.push_back(bg_images_[i].texture);
			}
			bg_images_.resize(cellCount);
		}

		for (size_t i = 0; i < cellCount; i++) {
			const BackgroundSettings *bg = nullptr;
			if (i < effective_visuals_.size())
				bg = &effective_visuals_[i].background;

			std::string wantPath;
			if (bg && bg->imageEnabled && !bg->imagePath.empty())
				wantPath = bg->imagePath;

			if (bg_images_[i].path == wantPath)
				continue; /* no change */

			ImageOp op;
			op.index = i;
			op.newPath = wantPath;
			op.oldTexture = bg_images_[i].texture;

			/* Clear immediately under lock */
			bg_images_[i].texture = nullptr;
			bg_images_[i].width = 0;
			bg_images_[i].height = 0;
			bg_images_[i].path = wantPath;

			if (op.oldTexture)
				textures_to_destroy.push_back(op.oldTexture);

			if (!wantPath.empty())
				ops.push_back(std::move(op));
		}
	}

	/* Phase 2: load images from disk (no lock needed) */
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
			obs_log(LOG_WARNING, "failed to load background image: %s", op.newPath.c_str());
		loaded.push_back(std::move(li));
	}

	/* Phase 3: graphics operations (destroy old + create new textures) */
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

	/* Phase 4: install new textures under lock */
	if (!loaded.empty()) {
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);
		for (auto &li : loaded) {
			if (li.loaded && li.imgFile.texture) {
				bg_images_[li.index].texture = li.imgFile.texture;
				bg_images_[li.index].width = li.imgFile.cx;
				bg_images_[li.index].height = li.imgFile.cy;
				li.imgFile.texture = nullptr; /* prevent free */
			}
		}
	}

	/* Free image file data (no lock needed) */
	for (auto &li : loaded)
		gs_image_file_free(&li.imgFile);
}

void AmvInstanceCore::release_bg_images()
{
	/* Called only from release_source_refs() which already handles
	 * texture destruction outside the mutex. This is now a no-op stub
	 * kept for interface compatibility. */
}

/* ---- Overlay image management ---- */

void AmvInstanceCore::rebuild_overlay_images()
{
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(ref_vp_width(), ref_vp_height());
	tmpEngine.compute();

	size_t cellCount = tmpEngine.cells().size();

	/* Phase 1: determine what changed under lock, collect old textures */
	struct ImageOp {
		size_t index;
		std::string newPath;
		gs_texture_t *oldTexture = nullptr;
	};
	std::vector<ImageOp> ops;
	std::vector<gs_texture_t *> textures_to_destroy;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		while (overlay_images_.size() < cellCount)
			overlay_images_.push_back(OverlayImage{});

		/* Shrink: collect textures from excess entries for destruction
		 * before erasing them (otherwise they leak when the grid shrinks). */
		if (overlay_images_.size() > cellCount) {
			for (size_t i = cellCount; i < overlay_images_.size(); i++) {
				if (overlay_images_[i].texture)
					textures_to_destroy.push_back(overlay_images_[i].texture);
			}
			overlay_images_.resize(cellCount);
		}

		for (size_t i = 0; i < cellCount; i++) {
			const OverlaySettings *ovl = nullptr;
			if (i < effective_visuals_.size())
				ovl = &effective_visuals_[i].overlay;

			std::string wantPath;
			if (ovl && ovl->enabled && !ovl->imagePath.empty())
				wantPath = ovl->imagePath;

			if (overlay_images_[i].path == wantPath)
				continue;

			ImageOp op;
			op.index = i;
			op.newPath = wantPath;
			op.oldTexture = overlay_images_[i].texture;

			overlay_images_[i].texture = nullptr;
			overlay_images_[i].width = 0;
			overlay_images_[i].height = 0;
			overlay_images_[i].path = wantPath;

			if (op.oldTexture)
				textures_to_destroy.push_back(op.oldTexture);

			if (!wantPath.empty())
				ops.push_back(std::move(op));
		}
	}

	/* Phase 2: load images from disk (no lock needed) */
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
			obs_log(LOG_WARNING, "failed to load overlay image: %s", op.newPath.c_str());
		loaded.push_back(std::move(li));
	}

	/* Phase 3: graphics operations (destroy old + create new textures) */
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

	/* Phase 4: install new textures under lock */
	if (!loaded.empty()) {
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);
		for (auto &li : loaded) {
			if (li.loaded && li.imgFile.texture) {
				overlay_images_[li.index].texture = li.imgFile.texture;
				overlay_images_[li.index].width = li.imgFile.cx;
				overlay_images_[li.index].height = li.imgFile.cy;
				li.imgFile.texture = nullptr;
			}
		}
	}

	for (auto &li : loaded)
		gs_image_file_free(&li.imgFile);
}

void AmvInstanceCore::release_overlay_images()
{
	/* Called only from release_source_refs() which already handles
	 * texture destruction outside the mutex. This is now a no-op stub
	 * kept for interface compatibility. */
}

/* ---- Safe Area rendering ---- */

/* EBU R95 / Rec. ITU-R BT.1848-1 margins (same as OBS native) */
#define ACTION_SAFE_PERCENT 0.035f
#define GRAPHICS_SAFE_PERCENT 0.05f
#define FOURBYTHREE_SAFE_PERCENT 0.1625f
#define CENTER_LINE_LENGTH 0.1f
