/*
OBS Advanced Multiview
Copyright (C) 2025 VTB-LINK

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "multiview-window.hpp"
#include "source-picker.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <QAction>
#include <QCloseEvent>
#include <QMenu>
#include <QScreen>
#include <QWindow>

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

/* ---- helpers (same as OBS internal) ---- */

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

static inline QSize GetPixelSize(QWidget *widget)
{
	return widget->size() * widget->devicePixelRatioF();
}

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y,
					float &scale)
{
	double windowAspect = (double)windowCX / (double)windowCY;
	double baseAspect = (double)baseCX / (double)baseCY;
	int newCX, newCY;

	if (windowAspect > baseAspect) {
		scale = (float)windowCY / (float)baseCY;
		newCX = (int)((double)windowCY * baseAspect);
		newCY = windowCY;
	} else {
		scale = (float)windowCX / (float)baseCX;
		newCX = windowCX;
		newCY = (int)((float)windowCX / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

/* ---- MultiviewWindow implementation ---- */

MultiviewWindow::MultiviewWindow(ConfigManager *config, const std::string &uuid, QWidget *parent)
	: QWidget(parent, Qt::Window),
	  config_(config),
	  uuid_(uuid)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_DeleteOnClose, false);
	setAttribute(Qt::WA_QuitOnClose, false);

	setMinimumSize(320, 180);
	resize(960, 540);

	/* Window title */
	refresh_title();

	/* Escape to close */
	QAction *escAction = new QAction(this);
	escAction->setShortcut(Qt::Key_Escape);
	addAction(escAction);
	connect(escAction, &QAction::triggered, this, &QWidget::close);

	/* Create display when window becomes visible */
	connect(windowHandle(), &QWindow::visibleChanged, this, [this](bool visible) {
		if (visible && !display_created_)
			create_display();
	});

	/* Get canvas aspect ratio from OBS base resolution */
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		canvas_aspect_ = (double)ovi.base_width / (double)ovi.base_height;
	}

	refresh_layout();
	refresh_sources();

	ready_ = true;
	show();
	activateWindow();
}

MultiviewWindow::~MultiviewWindow()
{
	ready_ = false;
	release_source_refs();
	destroy_display();
}

void MultiviewWindow::create_display()
{
	if (display_created_)
		return;
	if (!windowHandle())
		return;

	QSize size = GetPixelSize(this);

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#ifdef _WIN32
	info.window.hwnd = (HWND)windowHandle()->winId();
#elif __APPLE__
	info.window.view = (id)windowHandle()->winId();
#else
	info.window.id = windowHandle()->winId();
	info.window.display = obs_get_nix_platform_display();
#endif

	display_ = obs_display_create(&info, 0xFF000000);
	if (!display_)
		return;

	obs_display_add_draw_callback(display_, render_callback, this);
	display_created_ = true;
}

void MultiviewWindow::destroy_display()
{
	if (!display_created_)
		return;

	obs_display_remove_draw_callback(display_, render_callback, this);
	display_ = nullptr;
	display_created_ = false;
}

void MultiviewWindow::refresh_title()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (inst)
		setWindowTitle(QStringLiteral("Advanced Multiview - %1").arg(QString::fromStdString(inst->name)));
}

void MultiviewWindow::refresh_layout()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	layout_ = inst->layout;
	gutter_px_ = inst->effective_gutter(config_->global_settings().defaultGutterPx);

	/* Update layout with effective gutter */
	layout_.gutterPx = gutter_px_;

	/* Invalidate cached viewport to force recompute in next render frame */
	cached_vpW_ = 0;
	cached_vpH_ = 0;

	/* Re-resolve source refs: grid shape may have changed, affecting
	 * which cells exist and their indices */
	refresh_sources();

	/* Recompute effective visual settings for the new cell layout */
	refresh_visual_settings();
}

void MultiviewWindow::refresh_sources()
{
	release_source_refs();
	update_source_refs();
	rebuild_label_sources();
}

void MultiviewWindow::refresh_visual_settings()
{
	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (!inst) {
			effective_visuals_.clear();
			return;
		}

		const GlobalVisualSettings &globalVS = config_->global_settings().visualSettings;
		const InstanceVisualSettings &instVS = inst->visualSettings;

		/* Recompute layout to know cell positions */
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();

		const auto &cells = tmpEngine.cells();
		effective_visuals_.resize(cells.size());

		for (size_t i = 0; i < cells.size(); i++) {
			int r = cells[i].gridRow;
			int c = cells[i].gridCol;
			const CellVisualSettings *cellVS = inst->find_cell_visual(r, c);
			effective_visuals_[i] = resolve_effective_visual_settings(globalVS, instVS, cellVS);
		}

		rebuild_label_sources();
	}

	/* Image rebuild involves obs_enter_graphics() which must be called
	 * without holding source_mutex_ to prevent ABBA deadlock with the
	 * render thread (render thread: graphics lock -> source_mutex_). */
	rebuild_bg_images();
	rebuild_overlay_images();
}

void MultiviewWindow::update_source_refs()
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Recompute layout to know cell count and positions */
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(800, 600); /* dummy size for cell count */
	tmpEngine.compute();

	const auto &cells = tmpEngine.cells();
	size_t cellCount = cells.size();
	cell_sources_.resize(cellCount);

	for (size_t i = 0; i < cellCount; i++) {
		cell_sources_[i].type.clear();
		cell_sources_[i].name.clear();
		cell_sources_[i].weak_ref = nullptr;
		cell_sources_[i].showing = false;
		cell_sources_[i].prvw_fallback = false;

		/* Look up assignment by (gridRow, gridCol) */
		int r = cells[i].gridRow;
		int c = cells[i].gridCol;
		const CellAssignment *ca = nullptr;
		for (auto &a : inst->cellAssignments) {
			if (a.row == r && a.col == c) {
				ca = &a;
				break;
			}
		}
		if (!ca || ca->type.empty())
			continue;

		cell_sources_[i].type = ca->type;
		cell_sources_[i].name = ca->name;

		/* PGM/PRVW are resolved per-frame in render(), no caching */
		if (ca->type == "pgm" || ca->type == "prvw")
			continue;

		/* Scene/Source: cache weak ref and inc_showing */
		obs_source_t *src = obs_get_source_by_name(ca->name.c_str());
		if (src) {
			cell_sources_[i].weak_ref = OBSGetWeakRef(src);
			obs_source_inc_showing(src);
			cell_sources_[i].showing = true;
			obs_source_release(src);
		}
	}
}

void MultiviewWindow::release_source_refs()
{
	/* Collect textures to destroy outside the mutex to avoid
	 * deadlock: render thread holds graphics lock then takes mutex,
	 * so we must never hold mutex while calling obs_enter_graphics(). */
	std::vector<gs_texture_t *> textures_to_destroy;

	{
		std::lock_guard<std::recursive_mutex> lock(source_mutex_);

		for (auto &cs : cell_sources_) {
			if (cs.showing) {
				OBSSourceAutoRelease src = OBSGetStrongRef(cs.weak_ref);
				if (src)
					obs_source_dec_showing(src);
				cs.showing = false;
			}
			cs.weak_ref = nullptr;
		}
		cell_sources_.clear();

		/* Release label text sources */
		label_sources_.clear();

		/* Collect bg/overlay textures and clear vectors under lock */
		for (auto &bgi : bg_images_) {
			if (bgi.texture)
				textures_to_destroy.push_back(bgi.texture);
			bgi.texture = nullptr;
			bgi.path.clear();
		}
		bg_images_.clear();

		for (auto &oi : overlay_images_) {
			if (oi.texture)
				textures_to_destroy.push_back(oi.texture);
			oi.texture = nullptr;
			oi.path.clear();
		}
		overlay_images_.clear();
	}

	/* Destroy textures outside mutex, respecting lock order */
	if (!textures_to_destroy.empty()) {
		obs_enter_graphics();
		for (auto *tex : textures_to_destroy)
			gs_texture_destroy(tex);
		obs_leave_graphics();
	}
}

/* ---- Rendering ---- */

void MultiviewWindow::render_callback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiviewWindow *>(data);
	if (!self->ready_)
		return;
	self->render(cx, cy);
}

void MultiviewWindow::render(uint32_t cx, uint32_t cy)
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	/* Throttle lazy re-resolution: attempt once per re-resolve interval.
	 * Interval is determined by OBS canvas FPS or custom setting. */
	{
		const GlobalSettings &gs = config_->global_settings();
		double intervalFps = gs.reResolveCustomFps;
		if (gs.reResolveInheritObs) {
			struct obs_video_info ovi;
			if (obs_get_video_info(&ovi))
				intervalFps = (double)ovi.fps_num / (double)ovi.fps_den;
		}
		int interval = (int)(intervalFps + 0.5);
		if (interval < 1)
			interval = 1;
		re_resolve_counter_ = (re_resolve_counter_ + 1) % interval;
	}

	/* Compute canvas-aspect-ratio viewport (centered, with black borders) */
	double windowAspect = (double)cx / (double)cy;
	int vpX = 0, vpY = 0;
	int vpW = (int)cx, vpH = (int)cy;

	if (windowAspect > canvas_aspect_) {
		/* Window wider than canvas → pillarbox (black on sides) */
		vpW = (int)((double)cy * canvas_aspect_);
		vpX = ((int)cx - vpW) / 2;
	} else if (windowAspect < canvas_aspect_) {
		/* Window taller than canvas → letterbox (black on top/bottom) */
		vpH = (int)((double)cx / canvas_aspect_);
		vpY = ((int)cy - vpH) / 2;
	}

	/* Recompute layout only when viewport size actually changed */
	if (vpW != cached_vpW_ || vpH != cached_vpH_) {
		engine_.set_layout(layout_);
		engine_.set_viewport(vpW, vpH);
		engine_.compute();
		cached_vpW_ = vpW;
		cached_vpH_ = vpH;
	}

	const auto &cells = engine_.cells();

	/* Draw each cell (offset by vpX, vpY for centering) */
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	for (int i = 0; i < (int)cells.size(); i++) {
		const CellRect &cell = cells[i];
		int cellX = cell.x + vpX;
		int cellY = cell.y + vpY;

		/* Get source for this cell */
		obs_source_t *src = nullptr;
		OBSSourceAutoRelease srcHolder;
		bool isPrvwFallback = false;
		bool isPgm = false;

		if (i < (int)cell_sources_.size()) {
			const auto &cs = cell_sources_[i];

			if (cs.type == "pgm") {
				/* PGM: we use obs_render_main_texture() for
				 * composited output (includes transitions).
				 * Still get current scene to verify non-null. */
				srcHolder = obs_frontend_get_current_scene();
				src = srcHolder;
				isPgm = true;
			} else if (cs.type == "prvw") {
				/* Resolve PRVW fresh each frame */
				srcHolder = obs_frontend_get_current_preview_scene();
				if (!srcHolder) {
					/* No Studio Mode → fallback to PGM */
					srcHolder = obs_frontend_get_current_scene();
					isPrvwFallback = (srcHolder != nullptr);
				}
				src = srcHolder;
			} else if (!cs.type.empty()) {
				/* scene/source: use cached ref, or lazy re-resolve */
				if (cs.weak_ref)
					srcHolder = OBSGetStrongRef(cs.weak_ref);
				if (!srcHolder && !cs.name.empty() && re_resolve_counter_ == 0) {
					/* Source may have been re-added (undo) - throttled */
					obs_source_t *resolved = obs_get_source_by_name(cs.name.c_str());
					if (resolved) {
						cell_sources_[i].weak_ref = OBSGetWeakRef(resolved);
						obs_source_inc_showing(resolved);
						cell_sources_[i].showing = true;
						srcHolder = resolved;
						obs_source_release(resolved);
					}
				}
				src = srcHolder;
			}
		}

		/* Signal rect: will be set if source exists, otherwise defaults to cell rect */
		int vrX = cellX, vrY = cellY, vrW = cell.w, vrH = cell.h;
		bool hasSignalRect = false;

		if (src) {
			/* Determine source dimensions for letterbox */
			uint32_t srcW, srcH;
			if (isPgm) {
				/* PGM uses canvas resolution (composited output) */
				struct obs_video_info ovi;
				obs_get_video_info(&ovi);
				srcW = ovi.base_width;
				srcH = ovi.base_height;
			} else {
				srcW = obs_source_get_width(src);
				srcH = obs_source_get_height(src);
			}

			if (srcW == 0 || srcH == 0) {
				/* Source not ready - leave as gutter/window bg */
				continue;
			}

			/* Determine content area (may be reduced for Below label mode) */
			int contentX = cellX;
			int contentY = cellY;
			int contentW = cell.w;
			int contentH = cell.h;

			if (i < (int)effective_visuals_.size() &&
			    effective_visuals_[i].label.displayMode == LabelDisplayMode::Below) {
				/* Reserve bottom portion for label + gutter separator */
				int labelRegionH = cell.h / 6;
				if (labelRegionH < 16)
					labelRegionH = 16;
				int gutterH = gutter_px_;
				contentH = cell.h - labelRegionH - gutterH;
				if (contentH < 16)
					contentH = 16;
			}

			/* Calculate letterbox/pillarbox rect within content area */
			double srcAspect = (double)srcW / (double)srcH;
			double contentAspect = (double)contentW / (double)contentH;

			if (srcAspect > contentAspect) {
				vrW = contentW;
				vrH = (int)((double)contentW / srcAspect + 0.5);
				vrX = contentX;
				vrY = contentY + (contentH - vrH) / 2;
			} else {
				vrH = contentH;
				vrW = (int)((double)contentH * srcAspect + 0.5);
				vrX = contentX + (contentW - vrW) / 2;
				vrY = contentY;
			}
			hasSignalRect = true;

			/* Draw background ONLY in signal rect area (non-signal = gutter/window bg) */
			uint32_t bgColor = 0xFF000000; /* default black */
			if (i < (int)effective_visuals_.size() && effective_visuals_[i].background.colorEnabled)
				bgColor = effective_visuals_[i].background.color;

			if (i < (int)effective_visuals_.size() &&
			    effective_visuals_[i].label.displayMode == LabelDisplayMode::Below) {
				/* Below mode: bg behind signal rect + label area
				 * (gutter strip between them shows window background) */
				int labelRegionH = cell.h / 6;
				if (labelRegionH < 16)
					labelRegionH = 16;
				int gutterH = gutter_px_;
				int bgContentH = cell.h - labelRegionH - gutterH;
				if (bgContentH < 16)
					bgContentH = 16;

				/* Signal area background only */
				startRegion(vrX, vrY, vrW, vrH, 0.0f, (float)vrW, 0.0f, (float)vrH);
				gs_effect_set_color(colorParam, bgColor);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, vrW, vrH);
				endRegion();

				/* Label area background */
				int labelAreaY = cellY + bgContentH + gutterH;
				startRegion(cellX, labelAreaY, cell.w, labelRegionH, 0.0f, (float)cell.w, 0.0f,
					    (float)labelRegionH);
				gs_effect_set_color(colorParam, bgColor);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, cell.w, labelRegionH);
				endRegion();
			} else {
				/* Normal: fill only signal rect area */
				startRegion(vrX, vrY, vrW, vrH, 0.0f, (float)vrW, 0.0f, (float)vrH);
				gs_effect_set_color(colorParam, bgColor);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, vrW, vrH);
				endRegion();
			}

			/* Draw background image if available */
			if (i < (int)bg_images_.size() && bg_images_[i].texture) {
				gs_texture_t *tex = bg_images_[i].texture;
				uint32_t imgW = bg_images_[i].width;
				uint32_t imgH = bg_images_[i].height;
				if (imgW > 0 && imgH > 0) {
					/* Fit image into cell */
					double imgAspect = (double)imgW / (double)imgH;
					double cAspect = (double)cell.w / (double)cell.h;
					int drawW, drawH, drawX, drawY;
					if (imgAspect > cAspect) {
						drawW = cell.w;
						drawH = (int)((double)cell.w / imgAspect + 0.5);
						drawX = cellX;
						drawY = cellY + (cell.h - drawH) / 2;
					} else {
						drawH = cell.h;
						drawW = (int)((double)cell.h * imgAspect + 0.5);
						drawX = cellX + (cell.w - drawW) / 2;
						drawY = cellY;
					}
					gs_effect_t *defEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					gs_eparam_t *imgParam = gs_effect_get_param_by_name(defEffect, "image");
					gs_effect_set_texture(imgParam, tex);
					startRegion(drawX, drawY, drawW, drawH, 0.0f, (float)imgW, 0.0f, (float)imgH);
					while (gs_effect_loop(defEffect, "Draw"))
						gs_draw_sprite(tex, 0, imgW, imgH);
					endRegion();
				}
			}

			/* Render into video rect */
			startRegion(vrX, vrY, vrW, vrH, 0.0f, (float)srcW, 0.0f, (float)srcH);
			if (isPgm)
				obs_render_main_texture();
			else
				obs_source_video_render(src);
			endRegion();

			/* Draw PRVW fallback indicator (yellow bar at bottom) */
			if (isPrvwFallback) {
				int barH = (std::max)(2, cell.h / 20);
				startRegion(cellX, cellY + cell.h - barH, cell.w, barH, 0.0f, (float)cell.w, 0.0f,
					    (float)barH);
				/* Yellow with some transparency: 0xCCFFD400 (ARGB) */
				gs_effect_set_color(colorParam, 0xCCFFD400);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, cell.w, barH);
				endRegion();
			}
		} else {
			/* Empty cell / no signal - leave as gutter/window background.
			 * Only draw background image if one is configured. */
			if (i < (int)bg_images_.size() && bg_images_[i].texture) {
				gs_texture_t *tex = bg_images_[i].texture;
				uint32_t imgW = bg_images_[i].width;
				uint32_t imgH = bg_images_[i].height;
				if (imgW > 0 && imgH > 0) {
					double imgAspect = (double)imgW / (double)imgH;
					double cAspect = (double)cell.w / (double)cell.h;
					int drawW, drawH, drawX, drawY;
					if (imgAspect > cAspect) {
						drawW = cell.w;
						drawH = (int)((double)cell.w / imgAspect + 0.5);
						drawX = cellX;
						drawY = cellY + (cell.h - drawH) / 2;
					} else {
						drawH = cell.h;
						drawW = (int)((double)cell.h * imgAspect + 0.5);
						drawX = cellX + (cell.w - drawW) / 2;
						drawY = cellY;
					}
					gs_effect_t *defEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					gs_eparam_t *imgParam = gs_effect_get_param_by_name(defEffect, "image");
					gs_effect_set_texture(imgParam, tex);
					startRegion(drawX, drawY, drawW, drawH, 0.0f, (float)imgW, 0.0f, (float)imgH);
					while (gs_effect_loop(defEffect, "Draw"))
						gs_draw_sprite(tex, 0, imgW, imgH);
					endRegion();
				}
			}
		}

		/* Render foreground overlay image if available */
		if (i < (int)overlay_images_.size() && overlay_images_[i].texture) {
			const OverlaySettings *ovl = nullptr;
			if (i < (int)effective_visuals_.size())
				ovl = &effective_visuals_[i].overlay;

			if (ovl && ovl->enabled && ovl->opacity > 0.0) {
				gs_texture_t *tex = overlay_images_[i].texture;
				uint32_t imgW = overlay_images_[i].width;
				uint32_t imgH = overlay_images_[i].height;

				/* Determine anchor rect based on anchorMode */
				int anchorX, anchorY, anchorW, anchorH;
				if (ovl->anchorMode == OverlayAnchorMode::Signal && hasSignalRect) {
					anchorX = vrX;
					anchorY = vrY;
					anchorW = vrW;
					anchorH = vrH;
				} else {
					anchorX = cellX;
					anchorY = cellY;
					anchorW = cell.w;
					anchorH = cell.h;
				}

				if (imgW > 0 && imgH > 0 && anchorW > 0 && anchorH > 0) {
					int drawX, drawY, drawW, drawH;
					if (ovl->fitMode == OverlayFitMode::Stretch) {
						drawX = anchorX;
						drawY = anchorY;
						drawW = anchorW;
						drawH = anchorH;
					} else {
						/* Fit: preserve aspect ratio */
						double imgAspect = (double)imgW / (double)imgH;
						double aAspect = (double)anchorW / (double)anchorH;
						if (imgAspect > aAspect) {
							drawW = anchorW;
							drawH = (int)((double)anchorW / imgAspect + 0.5);
							drawX = anchorX;
							drawY = anchorY + (anchorH - drawH) / 2;
						} else {
							drawH = anchorH;
							drawW = (int)((double)anchorH * imgAspect + 0.5);
							drawX = anchorX + (anchorW - drawW) / 2;
							drawY = anchorY;
						}
					}

					/* Apply opacity via color multiplier */
					uint32_t alpha = (uint32_t)(ovl->opacity * 255.0 + 0.5);
					if (alpha > 255)
						alpha = 255;
					uint32_t overlayColor = (alpha << 24) | 0x00FFFFFF;

					gs_effect_t *defEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
					gs_eparam_t *imgParam = gs_effect_get_param_by_name(defEffect, "image");
					gs_effect_set_texture(imgParam, tex);
					gs_eparam_t *clrParam = gs_effect_get_param_by_name(defEffect, "color");
					if (clrParam) {
						struct vec4 clrVec;
						vec4_from_rgba(&clrVec, overlayColor);
						gs_effect_set_vec4(clrParam, &clrVec);
					}

					gs_blend_state_push();
					gs_enable_blending(true);
					gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

					startRegion(drawX, drawY, drawW, drawH, 0.0f, (float)imgW, 0.0f, (float)imgH);
					while (gs_effect_loop(defEffect, "Draw"))
						gs_draw_sprite(tex, 0, imgW, imgH);
					endRegion();

					gs_blend_state_pop();
				}
			}
		}

		/* Render label overlay */
		render_label(i, cell, vpX, vpY);
	}
}

/* ---- Label rendering ---- */

void MultiviewWindow::rebuild_label_sources()
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	/* We create private text sources for each cell that needs a label. */
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
	tmpEngine.compute();

	const auto &cells = tmpEngine.cells();
	size_t cellCount = cells.size();

	/* Resize label_sources_ to match cell count */
	label_sources_.resize(cellCount);

	for (size_t i = 0; i < cellCount; i++) {
		/* Determine label text from cell assignment */
		std::string labelText;
		if (i < cell_sources_.size()) {
			const auto &cs = cell_sources_[i];
			if (cs.type == "pgm")
				labelText = "PGM";
			else if (cs.type == "prvw")
				labelText = "PRVW";
			else if (!cs.name.empty())
				labelText = cs.name;
		}

		/* Determine effective label settings */
		const LabelSettings *ls = nullptr;
		if (i < effective_visuals_.size())
			ls = &effective_visuals_[i].label;

		/* If label is disabled or no text, release source */
		if (labelText.empty() || !ls || ls->displayMode == LabelDisplayMode::None) {
			label_sources_[i].source = nullptr;
			label_sources_[i].text.clear();
			continue;
		}

		/* Create source if needed */
		if (!label_sources_[i].source) {
			std::string srcName = "adv_mv_label_" + uuid_ + "_" + std::to_string(i);

			/* Pad text with spaces for horizontal padding (same as OBS native) */
			std::string paddedText = " " + labelText + " ";

			/* Use maxFontSize for ScaleWithCell so we always scale DOWN
			 * (avoids blurry upscaling of small bitmap textures) */
			int renderFontSize = ls->fontSize;
			if (ls->fontScaleMode == FontScaleMode::ScaleWithCell)
				renderFontSize = ls->maxFontSize > 0 ? ls->maxFontSize : 72;

#ifdef _WIN32
			/* Windows: use text_gdiplus for automatic CJK font fallback */
			const char *fontFace = ls->fontFamily.empty() ? "Arial" : ls->fontFamily.c_str();
			obs_data_t *fontObj = obs_data_create();
			obs_data_set_int(fontObj, "size", renderFontSize);
			obs_data_set_string(fontObj, "face", fontFace);
			obs_data_set_int(fontObj, "flags", 1); /* Bold */

			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "text", paddedText.c_str());
			obs_data_set_obj(settings, "font", fontObj);
			obs_data_set_int(settings, "color", ls->textColor);
			obs_data_set_int(settings, "opacity", 100);
			obs_data_set_bool(settings, "outline", false);
			obs_data_set_int(settings, "align", 0); /* left */

			obs_source_t *src = obs_source_create_private("text_gdiplus", srcName.c_str(), settings);
			obs_data_release(settings);
			obs_data_release(fontObj);
#else
			/* Mac/Linux: use text_ft2_source_v2 with CJK-capable font */
			const char *fontFace = ls->fontFamily.empty()
#ifdef __APPLE__
						       ? "Helvetica"
#else
						       ? "Monospace"
#endif
						       : ls->fontFamily.c_str();
			obs_data_t *fontObj = obs_data_create();
			obs_data_set_int(fontObj, "size", renderFontSize);
			obs_data_set_string(fontObj, "face", fontFace);
			obs_data_set_int(fontObj, "flags", 1); /* Bold */

			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "text", paddedText.c_str());
			obs_data_set_obj(settings, "font", fontObj);
			obs_data_set_int(settings, "color1", ls->textColor);
			obs_data_set_int(settings, "color2", ls->textColor);
			obs_data_set_bool(settings, "outline", false);
			obs_data_set_bool(settings, "drop_shadow", false);

			obs_source_t *src = obs_source_create_private("text_ft2_source_v2", srcName.c_str(), settings);
			obs_data_release(settings);
			obs_data_release(fontObj);

			if (!src) {
				/* Fallback: try text_ft2_source */
				fontObj = obs_data_create();
				obs_data_set_int(fontObj, "size", renderFontSize);
				obs_data_set_string(fontObj, "face", fontFace);
				obs_data_set_int(fontObj, "flags", 1);

				settings = obs_data_create();
				obs_data_set_string(settings, "text", paddedText.c_str());
				obs_data_set_obj(settings, "font", fontObj);
				obs_data_set_int(settings, "color1", ls->textColor);
				obs_data_set_int(settings, "color2", ls->textColor);
				src = obs_source_create_private("text_ft2_source", srcName.c_str(), settings);
				obs_data_release(settings);
				obs_data_release(fontObj);
			}
#endif

			label_sources_[i].source = src;
			label_sources_[i].text = labelText;
			label_sources_[i].color = ls->textColor;
			obs_source_release(src);
		} else if (label_sources_[i].text != labelText || label_sources_[i].color != ls->textColor) {
			/* Update existing source text/color */
			std::string paddedText = " " + labelText + " ";
			int renderFontSize = ls->fontSize;
			if (ls->fontScaleMode == FontScaleMode::ScaleWithCell)
				renderFontSize = ls->maxFontSize > 0 ? ls->maxFontSize : 72;

#ifdef _WIN32
			const char *fontFace = ls->fontFamily.empty() ? "Arial" : ls->fontFamily.c_str();
#elif __APPLE__
			const char *fontFace = ls->fontFamily.empty() ? "Helvetica" : ls->fontFamily.c_str();
#else
			const char *fontFace = ls->fontFamily.empty() ? "Monospace" : ls->fontFamily.c_str();
#endif
			obs_data_t *fontObj = obs_data_create();
			obs_data_set_int(fontObj, "size", renderFontSize);
			obs_data_set_string(fontObj, "face", fontFace);
			obs_data_set_int(fontObj, "flags", 1);

			obs_data_t *settings = obs_source_get_settings(label_sources_[i].source);
			obs_data_set_string(settings, "text", paddedText.c_str());
			obs_data_set_obj(settings, "font", fontObj);
#ifdef _WIN32
			obs_data_set_int(settings, "color", ls->textColor);
#else
			obs_data_set_int(settings, "color1", ls->textColor);
			obs_data_set_int(settings, "color2", ls->textColor);
#endif
			obs_source_update(label_sources_[i].source, settings);
			obs_data_release(settings);
			obs_data_release(fontObj);
			label_sources_[i].text = labelText;
			label_sources_[i].color = ls->textColor;
		}
	}
}

void MultiviewWindow::render_label(int cellIndex, const CellRect &cell, int vpX, int vpY)
{
	if (cellIndex < 0 || cellIndex >= (int)effective_visuals_.size())
		return;

	const LabelSettings &ls = effective_visuals_[cellIndex].label;
	if (ls.displayMode == LabelDisplayMode::None)
		return;

	if (cellIndex >= (int)label_sources_.size() || !label_sources_[cellIndex].source)
		return;

	obs_source_t *labelSrc = label_sources_[cellIndex].source;
	uint32_t labelW = obs_source_get_width(labelSrc);
	uint32_t labelH = obs_source_get_height(labelSrc);
	if (labelW == 0 || labelH == 0)
		return;

	int cellX = cell.x + vpX;
	int cellY = cell.y + vpY;

	/* Compute target label height based on scale mode */
	int targetH;
	if (ls.fontScaleMode == FontScaleMode::ScaleWithCell) {
		/* Scale: label is ~10% of cell height, clamped by min/max */
		targetH = cell.h * 10 / 100;
		int minH = ls.minFontSize;
		int maxH = ls.maxFontSize;
		if (targetH < minH)
			targetH = minH;
		if (targetH > maxH)
			targetH = maxH;
	} else {
		/* Fixed: use 1/4 cell height as max */
		targetH = cell.h / 4;
		if (targetH < 8)
			targetH = 8;
	}

	/* Scale factor to fit label into target height */
	float scaleFactor = (float)targetH / (float)labelH;
	int drawW = (int)((float)labelW * scaleFactor);
	int drawH = targetH;

	/* Clamp label width to cell width */
	if (drawW > cell.w) {
		float clamp = (float)cell.w / (float)drawW;
		drawW = cell.w;
		drawH = (int)((float)drawH * clamp);
		scaleFactor *= clamp;
	}

	/* Vertical padding for background (symmetric, like OBS native) */
	int thickness = (int)(scaleFactor * 4.0f + 0.5f);
	if (thickness < 2)
		thickness = 2;
	int bgH = drawH + thickness * 2;
	int bgW = drawW;

	/* Calculate position */
	int bgX, bgY;

	/* Horizontal centering */
	bgX = cellX + (cell.w - bgW) / 2;

	if (ls.displayMode == LabelDisplayMode::Below) {
		/* Below mode: center in label region */
		int labelRegionH = cell.h / 6;
		if (labelRegionH < 16)
			labelRegionH = 16;
		bgY = cellY + cell.h - labelRegionH + (labelRegionH - bgH) / 2;
	} else if (ls.position == LabelPosition::Top) {
		bgY = cellY + thickness;
	} else {
		/* Bottom (Overlay) - offset upward from cell edge */
		bgY = cellY + cell.h - bgH - thickness * 3;
	}

	/* Draw semi-transparent background behind label (OBS approach:
	 * bg = full source width, source height + 2*thickness) */
	if (ls.backgroundOpacity > 0.01) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

		uint8_t alpha = (uint8_t)(ls.backgroundOpacity * 255.0);
		uint32_t bgColor = ((uint32_t)alpha << 24) | 0x000000;

		startRegion(bgX, bgY, bgW, bgH, 0.0f, (float)bgW, 0.0f, (float)bgH);
		gs_effect_set_color(colorParam, bgColor);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, bgW, bgH);
		endRegion();
	}

	/* Render text source centered within background box
	 * (text at y-offset = thickness, same width as bg since spaces provide h-padding) */
	int textX = bgX;
	int textY = bgY + thickness;
	startRegion(textX, textY, drawW, drawH, 0.0f, (float)labelW, 0.0f, (float)labelH);
	obs_source_video_render(labelSrc);
	endRegion();
}

/* ---- Background image management ---- */

void MultiviewWindow::rebuild_bg_images()
{
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
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
			blog(LOG_WARNING, "[adv-multiview] Failed to load background image: %s",
			     op.newPath.c_str());
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

void MultiviewWindow::release_bg_images()
{
	/* Called only from release_source_refs() which already handles
	 * texture destruction outside the mutex. This is now a no-op stub
	 * kept for interface compatibility. */
}

/* ---- Overlay image management ---- */

void MultiviewWindow::rebuild_overlay_images()
{
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
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
			blog(LOG_WARNING, "[adv-multiview] Failed to load overlay image: %s", op.newPath.c_str());
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

void MultiviewWindow::release_overlay_images()
{
	/* Called only from release_source_refs() which already handles
	 * texture destruction outside the mutex. This is now a no-op stub
	 * kept for interface compatibility. */
}

/* ---- Events ---- */

bool MultiviewWindow::event(QEvent *event)
{
	if (event->type() == QEvent::Expose) {
		if (!display_created_)
			create_display();
	}
	return QWidget::event(event);
}

void MultiviewWindow::closeEvent(QCloseEvent *event)
{
	/* Stop rendering and release all source refs immediately.
	 * This ensures obs_source_dec_showing is called, which stops
	 * screen capture sources from signaling the OS (yellow border). */
	ready_ = false;
	destroy_display();
	release_source_refs();

	emit window_closed(uuid_);
	event->accept();
	hide();
}

void MultiviewWindow::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	if (display_created_) {
		QSize size = GetPixelSize(this);
		obs_display_resize(display_, size.width(), size.height());
	}
}

void MultiviewWindow::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::RightButton) {
		/* Hit test to determine which cell was clicked */
		QSize pixelSize = GetPixelSize(this);
		float ratio = (float)pixelSize.width() / (float)width();
		int mx = (int)(event->position().x() * ratio);
		int my = (int)(event->position().y() * ratio);

		/* Account for canvas aspect ratio viewport offset */
		int totalW = pixelSize.width();
		int totalH = pixelSize.height();
		double windowAspect = (double)totalW / (double)totalH;
		int vpX = 0, vpY = 0;
		int vpW = totalW, vpH = totalH;

		if (windowAspect > canvas_aspect_) {
			vpW = (int)((double)totalH * canvas_aspect_);
			vpX = (totalW - vpW) / 2;
		} else if (windowAspect < canvas_aspect_) {
			vpH = (int)((double)totalW / canvas_aspect_);
			vpY = (totalH - vpH) / 2;
		}

		/* Translate mouse to layout-local coordinates */
		int lx = mx - vpX;
		int ly = my - vpY;

		engine_.set_layout(layout_);
		engine_.set_viewport(vpW, vpH);
		engine_.compute();

		auto hit = engine_.hit_test(lx, ly);
		int cellIndex = -1;
		if (hit && hit->type == HitType::Cell)
			cellIndex = hit->cellIndex;

		show_context_menu(event->globalPosition().toPoint(), cellIndex);
	}

	QWidget::mousePressEvent(event);
}

/* ---- Context Menu ---- */

void MultiviewWindow::show_context_menu(const QPoint &pos, int cellIndex)
{
	QMenu menu(this);

	/* Fullscreen */
	QAction *fullscreenAction = menu.addAction(QStringLiteral("Fullscreen"));
	fullscreenAction->setCheckable(true);
	fullscreenAction->setChecked(isFullScreen());
	connect(fullscreenAction, &QAction::triggered, this, &MultiviewWindow::on_toggle_fullscreen);

	/* Always on top */
	QAction *onTopAction = menu.addAction(QStringLiteral("Always on Top"));
	onTopAction->setCheckable(true);
	onTopAction->setChecked(is_always_on_top_);
	connect(onTopAction, &QAction::triggered, this, &MultiviewWindow::on_toggle_always_on_top);

	menu.addSeparator();

	if (cellIndex >= 0) {
		/* Check if cell has a source assigned */
		bool hasSource = false;
		{
			std::lock_guard<std::recursive_mutex> lock(source_mutex_);
			if (cellIndex < (int)cell_sources_.size() && !cell_sources_[cellIndex].type.empty())
				hasSource = true;
		}

		if (hasSource) {
			QAction *changeAction = menu.addAction(QStringLiteral("Change Source..."));
			connect(changeAction, &QAction::triggered, this,
				[this, cellIndex]() { on_change_source(cellIndex); });

			QAction *clearAction = menu.addAction(QStringLiteral("Clear Cell"));
			connect(clearAction, &QAction::triggered, this,
				[this, cellIndex]() { on_clear_cell(cellIndex); });
		} else {
			QAction *addAction = menu.addAction(QStringLiteral("Add Source..."));
			connect(addAction, &QAction::triggered, this,
				[this, cellIndex]() { on_add_source(cellIndex); });
		}

		menu.addSeparator();
	}

	QAction *editGridAction = menu.addAction(QStringLiteral("Edit Grid..."));
	connect(editGridAction, &QAction::triggered, this, &MultiviewWindow::on_edit_grid);

	menu.addSeparator();

	QAction *settingsAction = menu.addAction(QStringLiteral("Global Settings"));
	connect(settingsAction, &QAction::triggered, this, &MultiviewWindow::on_global_settings);

	QAction *closeAction = menu.addAction(QStringLiteral("Close"));
	connect(closeAction, &QAction::triggered, this, &QWidget::close);

	menu.exec(pos);
}

void MultiviewWindow::on_add_source(int cellIndex)
{
	SourcePicker picker(this);
	if (picker.exec() != QDialog::Accepted)
		return;

	CellAssignment ca = picker.result_assignment();

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Determine the (row, col) of the clicked cell from the engine */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	ca.row = r;
	ca.col = c;

	/* Replace existing assignment at (r,c) or add new */
	bool found = false;
	for (auto &a : inst->cellAssignments) {
		if (a.row == r && a.col == c) {
			a = ca;
			found = true;
			break;
		}
	}
	if (!found)
		inst->cellAssignments.push_back(ca);

	inst->signalDirty = true;
	config_->save();

	refresh_sources();
}

void MultiviewWindow::on_change_source(int cellIndex)
{
	on_add_source(cellIndex); /* Same flow */
}

void MultiviewWindow::on_clear_cell(int cellIndex)
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Determine (row, col) of the cell */
	int r, c;
	{
		LayoutEngine tmpEngine;
		tmpEngine.set_layout(layout_);
		tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
		tmpEngine.compute();
		const auto &cells = tmpEngine.cells();
		if (cellIndex < 0 || cellIndex >= (int)cells.size())
			return;
		r = cells[cellIndex].gridRow;
		c = cells[cellIndex].gridCol;
	}

	/* Remove assignment at (r,c) */
	auto &assignments = inst->cellAssignments;
	assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
					 [r, c](const CellAssignment &a) { return a.row == r && a.col == c; }),
			  assignments.end());

	inst->signalDirty = true;
	config_->save();

	refresh_sources();
}

void MultiviewWindow::on_save_assignments()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	inst->signalDirty = false;
	config_->save();
	obs_log(LOG_INFO, "cell assignments saved for '%s'", inst->name.c_str());
}

void MultiviewWindow::on_edit_grid()
{
	open_manager_dialog();
}

void MultiviewWindow::on_global_settings()
{
	open_manager_dialog();
}

void MultiviewWindow::on_toggle_fullscreen()
{
	if (isFullScreen()) {
		showNormal();
	} else {
		showFullScreen();
	}
}

void MultiviewWindow::on_toggle_always_on_top()
{
	is_always_on_top_ = !is_always_on_top_;

#ifdef _WIN32
	/* Use SetWindowPos directly to avoid HWND recreation and flicker */
	HWND hwnd = (HWND)winId();
	HWND insertAfter = is_always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST;
	SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
	Qt::WindowFlags flags = windowFlags();
	if (is_always_on_top_)
		flags |= Qt::WindowStaysOnTopHint;
	else
		flags &= ~Qt::WindowStaysOnTopHint;

	destroy_display();
	setWindowFlags(flags);
	show();
	create_display();
#endif
}
