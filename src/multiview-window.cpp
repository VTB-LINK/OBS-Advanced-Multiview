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
#include "cell-display-settings-dialog.hpp"
#include "source-picker.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QAction>
#include <QCloseEvent>
#include <QMenu>
#include <QScreen>
#include <QWindow>

#include <algorithm>
#include <cmath>

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

/* ---- PGM/PRVW tree collection helpers ----
 *
 * obs_source_enum_active_tree walks all active (rendered) sources reachable
 * from a root scene, including nested scenes, source filters' inputs, and
 * any other compositional dependencies. It performs cycle detection
 * internally, so a self-referencing scene group will not infinite-recurse.
 *
 * We only keep raw pointer identity for set-membership testing — we never
 * dereference these pointers. The parent scene holds strong references to
 * everything in the tree for the duration of the frame, so the pointers
 * remain unique and valid for the lookup window. */
static void collect_tree_cb(obs_source_t * /*parent*/, obs_source_t *child, void *param)
{
	auto *out = static_cast<std::unordered_set<obs_source_t *> *>(param);
	if (child)
		out->insert(child);
}

static void collect_tree_sources(obs_source_t *root, std::unordered_set<obs_source_t *> &out)
{
	if (!root)
		return;
	/* Include the root itself so a cell whose source IS the PGM/PRVW scene
	 * gets classified correctly by the same set membership lookup. */
	out.insert(root);
	obs_source_enum_active_tree(root, collect_tree_cb, &out);
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
	rebuild_volmeters();
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

	/* Release volmeters (no graphics context needed) */
	release_volmeters();

	/* Release safe area vertex buffers */
	obs_enter_graphics();
	release_safe_area_vbs();
	obs_leave_graphics();
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

	/* Detect PGM/PRVW scene changes and rebuild volmeters if needed */
	if (has_pgm_cell_ || has_prvw_cell_)
		check_scene_change_for_volmeters();

	/* Snapshot PGM / PRVW scene trees once per frame for cell-highlight
	 * classification. We always do this (independent of has_pgm_cell_ /
	 * has_prvw_cell_) because a regular "scene" cell can be highlighted
	 * when its source happens to be the current PGM/PRVW scene or is
	 * nested inside one. Studio Mode OFF → prvw_tree_set_ stays empty
	 * → no green borders anywhere, automatically. */
	refresh_highlight_tree_sets();

	/* Handle deferred rebuild requested from non-render threads
	 * (audio_mixers signal) and poll for AutoFollow streaming-track changes
	 * (no event fires on Settings → Output → Streaming Audio Track edits). */
	check_active_track_change();

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

	/* Fill viewport with gutter color (same as OBS native outerColor) */
	startRegion(vpX, vpY, vpW, vpH, 0.0f, (float)vpW, 0.0f, (float)vpH);
	gs_effect_set_color(colorParam, 0xFF999999);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, vpW, vpH);
	endRegion();

	for (int i = 0; i < (int)cells.size(); i++) {
		const CellRect &cell = cells[i];
		int cellX = cell.x + vpX;
		int cellY = cell.y + vpY;

		/* Determine if FillSignalOnly mode (pillarbox/letterbox stays gutter color) */
		bool signalFillOnly = false;
		if (i < (int)effective_visuals_.size() &&
		    effective_visuals_[i].background.fillMode == BackgroundFillMode::FillSignalOnly) {
			signalFillOnly = true;
		}

		if (!signalFillOnly) {
			/* Fill entire cell with background color (default black) */
			uint32_t cellBg = 0xFF000000;
			if (i < (int)effective_visuals_.size() && effective_visuals_[i].background.colorEnabled)
				cellBg = effective_visuals_[i].background.color;
			startRegion(cellX, cellY, cell.w, cell.h, 0.0f, (float)cell.w, 0.0f, (float)cell.h);
			gs_effect_set_color(colorParam, cellBg);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, cell.w, cell.h);
			endRegion();
		}

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

			/* Snap to fill: if letterbox/pillarbox is tiny (<=8px total),
			 * skip it and stretch to fill content area (like OBS native). */
			constexpr int SNAP_THRESHOLD = 8;
			if ((contentW - vrW) <= SNAP_THRESHOLD && (contentH - vrH) <= SNAP_THRESHOLD) {
				vrX = contentX;
				vrY = contentY;
				vrW = contentW;
				vrH = contentH;
			}

			hasSignalRect = true;

			/* Draw background fill for FillSignalOnly mode:
			 * Only fill the signal rect area; pillarbox/letterbox stays gutter color. */
			if (signalFillOnly) {
				uint32_t bgColor = 0xFF000000;
				if (i < (int)effective_visuals_.size() && effective_visuals_[i].background.colorEnabled)
					bgColor = effective_visuals_[i].background.color;
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
					/* Determine target rect based on fill mode */
					const BackgroundSettings *bgS = i < (int)effective_visuals_.size()
										? &effective_visuals_[i].background
										: nullptr;
					int tgtX, tgtY, tgtW, tgtH;
					if (bgS && bgS->fillMode == BackgroundFillMode::FillSignalOnly) {
						tgtX = vrX;
						tgtY = vrY;
						tgtW = vrW;
						tgtH = vrH;
					} else {
						tgtX = cellX;
						tgtY = cellY;
						tgtW = cell.w;
						tgtH = cell.h;
					}

					int drawW, drawH, drawX, drawY;
					if (bgS && bgS->imageFitMode == ImageFitMode::Stretch) {
						drawX = tgtX;
						drawY = tgtY;
						drawW = tgtW;
						drawH = tgtH;
					} else {
						/* Fit: maintain aspect ratio */
						double imgAspect = (double)imgW / (double)imgH;
						double tgtAspect = (double)tgtW / (double)tgtH;
						if (imgAspect > tgtAspect) {
							drawW = tgtW;
							drawH = (int)((double)tgtW / imgAspect + 0.5);
							drawX = tgtX;
							drawY = tgtY + (tgtH - drawH) / 2;
						} else {
							drawH = tgtH;
							drawW = (int)((double)tgtH * imgAspect + 0.5);
							drawX = tgtX + (tgtW - drawW) / 2;
							drawY = tgtY;
						}
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
					const BackgroundSettings *bgS = i < (int)effective_visuals_.size()
										? &effective_visuals_[i].background
										: nullptr;
					/* Empty cell has no signal rect, always use cell rect */
					int tgtX = cellX, tgtY = cellY, tgtW = cell.w, tgtH = cell.h;

					int drawW, drawH, drawX, drawY;
					if (bgS && bgS->imageFitMode == ImageFitMode::Stretch) {
						drawX = tgtX;
						drawY = tgtY;
						drawW = tgtW;
						drawH = tgtH;
					} else {
						double imgAspect = (double)imgW / (double)imgH;
						double tgtAspect = (double)tgtW / (double)tgtH;
						if (imgAspect > tgtAspect) {
							drawW = tgtW;
							drawH = (int)((double)tgtW / imgAspect + 0.5);
							drawX = tgtX;
							drawY = tgtY + (tgtH - drawH) / 2;
						} else {
							drawH = tgtH;
							drawW = (int)((double)tgtH * imgAspect + 0.5);
							drawX = tgtX + (tgtW - drawW) / 2;
							drawY = tgtY;
						}
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

		/* Render safe area guides (anchored to SignalRect, after video, before overlay) */
		if (hasSignalRect)
			render_safe_area(i, vrX, vrY, vrW, vrH);

		/* (PGM/PRVW highlight borders are rendered in two post-loop passes
		 * below so PGM (red) always paints on top of PRVW (green) even when
		 * the two cells are adjacent / share a gutter edge, and so the
		 * border sits on top of label & VU overlays — which is important in
		 * gutter == 0 layouts where the border is drawn INSIDE the cell.) */

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

		/* Fill label region background (Below mode only, when labelRegionFill enabled) */
		if (i < (int)effective_visuals_.size() &&
		    effective_visuals_[i].label.displayMode == LabelDisplayMode::Below &&
		    effective_visuals_[i].background.labelRegionFill) {
			int labelRegionH = cell.h / 6;
			if (labelRegionH < 16)
				labelRegionH = 16;
			int gutterH = gutter_px_;
			int labelY = cellY + cell.h - labelRegionH;
			/* Fill the label row with bgColor (or default black if color not enabled) */
			uint32_t bgColor = effective_visuals_[i].background.colorEnabled
						   ? effective_visuals_[i].background.color
						   : 0xFF000000;
			startRegion(cellX, labelY, cell.w, labelRegionH, 0.0f, (float)cell.w, 0.0f,
				    (float)labelRegionH);
			gs_effect_set_color(colorParam, bgColor);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, cell.w, labelRegionH);
			endRegion();
			(void)gutterH;
		}

		/* Render label overlay */
		render_label(i, cell, vpX, vpY);

		/* Render VU meter bars */
		render_vu_meter(i, cell, vpX, vpY, vrX, vrY, vrW, vrH);
	}

	/* ---- PGM / PRVW highlight pass (post-cell, two layers) ----
	 *
	 * Done outside the per-cell loop so we can control layering precisely:
	 *
	 *   Pass 1 — draw all PRVW (green) borders
	 *   Pass 2 — draw all PGM  (red)   borders   ← always on top
	 *
	 * This guarantees PGM > PRVW visually even when two highlighted cells
	 * are adjacent (or diagonally touching), where the second cell's border
	 * would otherwise paint over the first's in the shared gutter zone.
	 *
	 * It also means highlight is the LAST thing drawn into each cell rect,
	 * which makes the gutter == 0 inset-border mode visible on top of any
	 * label / VU meter that happens to sit at the cell edge.
	 *
	 * compute_cell_highlight() walks the PGM/PRVW tree sets and is cheap
	 * but non-trivial; cache the per-cell kind once instead of recomputing
	 * in both passes. */
	std::vector<HighlightKind> cellKinds(cells.size(), HighlightKind::None);
	for (int i = 0; i < (int)cells.size(); i++) {
		if (i >= (int)effective_visuals_.size())
			continue;
		cellKinds[i] = compute_cell_highlight(i);
	}
	auto render_highlight_pass = [&](bool pgmPass) {
		for (int i = 0; i < (int)cells.size(); i++) {
			HighlightKind hk = cellKinds[i];
			if (hk == HighlightKind::None)
				continue;
			bool isPgm = (hk == HighlightKind::PgmDirect || hk == HighlightKind::PgmNested);
			if (pgmPass != isPgm)
				continue;
			render_cell_highlight(cells[i], vpX, vpY, hk, effective_visuals_[i].highlight);
		}
	};
	render_highlight_pass(false); /* PRVW first */
	render_highlight_pass(true);  /* PGM on top */
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
			obs_data_set_int(settings, "opacity", 50); /* OBS native uses 50% */
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
			label_sources_[i].fontSize = renderFontSize;
			label_sources_[i].fontFamily = ls->fontFamily;
			obs_source_release(src);
		} else {
			int renderFontSize = ls->fontSize;
			if (ls->fontScaleMode == FontScaleMode::ScaleWithCell)
				renderFontSize = ls->maxFontSize > 0 ? ls->maxFontSize : 72;

			if (label_sources_[i].text != labelText || label_sources_[i].color != ls->textColor ||
			    label_sources_[i].fontSize != renderFontSize ||
			    label_sources_[i].fontFamily != ls->fontFamily) {
				/* Update existing source text/color/font */
				std::string paddedText = " " + labelText + " ";

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
				label_sources_[i].fontSize = renderFontSize;
				label_sources_[i].fontFamily = ls->fontFamily;
			}
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
		/* Scale: match OBS native effective size.
		 * OBS creates font at (canvas_h/3)/9.81 and renders at ppiScaleY≈0.5,
		 * giving effective text height ≈ cell_h / 14.7 for scene cells. */
		targetH = (int)((double)cell.h / 14.7 + 0.5);
		int minH = ls.minFontSize;
		int maxH = ls.maxFontSize;
		if (targetH < minH)
			targetH = minH;
		if (targetH > maxH)
			targetH = maxH;
	} else {
		/* Fixed: use fontSize directly as display height (1:1 with texture) */
		targetH = ls.fontSize;
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

	/* Vertical padding for background (symmetric, like OBS native).
	 * OBS uses thickness=6 at canvas level, rendered at ppiScaleY≈0.5,
	 * giving effective ~3px padding per side for 270px cells. */
	int thickness = (int)((double)cell.h * 3.0 / 270.0 + 0.5);
	if (thickness < 1)
		thickness = 1;
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

void MultiviewWindow::release_overlay_images()
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

void MultiviewWindow::init_safe_area_vbs()
{
	if (safe_area_vb_init_)
		return;

	/* Action Safe (3.5% margin) */
	gs_render_start(true);
	gs_vertex2f(ACTION_SAFE_PERCENT, ACTION_SAFE_PERCENT);
	gs_vertex2f(ACTION_SAFE_PERCENT, 1.0f - ACTION_SAFE_PERCENT);
	gs_vertex2f(1.0f - ACTION_SAFE_PERCENT, 1.0f - ACTION_SAFE_PERCENT);
	gs_vertex2f(1.0f - ACTION_SAFE_PERCENT, ACTION_SAFE_PERCENT);
	gs_vertex2f(ACTION_SAFE_PERCENT, ACTION_SAFE_PERCENT);
	safe_action_vb_ = gs_render_save();

	/* Graphics Safe (5.0% margin) */
	gs_render_start(true);
	gs_vertex2f(GRAPHICS_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(GRAPHICS_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - GRAPHICS_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - GRAPHICS_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(GRAPHICS_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	safe_graphics_vb_ = gs_render_save();

	/* 4:3 safe for widescreen (16.25% horizontal margin) */
	gs_render_start(true);
	gs_vertex2f(FOURBYTHREE_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - FOURBYTHREE_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(1.0f - FOURBYTHREE_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(FOURBYTHREE_SAFE_PERCENT, 1.0f - GRAPHICS_SAFE_PERCENT);
	gs_vertex2f(FOURBYTHREE_SAFE_PERCENT, GRAPHICS_SAFE_PERCENT);
	safe_4x3_vb_ = gs_render_save();

	/* Center horizontal line - LEFT */
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.5f);
	gs_vertex2f(CENTER_LINE_LENGTH, 0.5f);
	safe_center_left_vb_ = gs_render_save();

	/* Center vertical line - TOP */
	gs_render_start(true);
	gs_vertex2f(0.5f, 0.0f);
	gs_vertex2f(0.5f, CENTER_LINE_LENGTH);
	safe_center_top_vb_ = gs_render_save();

	/* Center horizontal line - RIGHT */
	gs_render_start(true);
	gs_vertex2f(1.0f, 0.5f);
	gs_vertex2f(1.0f - CENTER_LINE_LENGTH, 0.5f);
	safe_center_right_vb_ = gs_render_save();

	safe_area_vb_init_ = true;
}

void MultiviewWindow::release_safe_area_vbs()
{
	if (!safe_area_vb_init_)
		return;

	gs_vertexbuffer_destroy(safe_action_vb_);
	gs_vertexbuffer_destroy(safe_graphics_vb_);
	gs_vertexbuffer_destroy(safe_4x3_vb_);
	gs_vertexbuffer_destroy(safe_center_left_vb_);
	gs_vertexbuffer_destroy(safe_center_top_vb_);
	gs_vertexbuffer_destroy(safe_center_right_vb_);
	safe_action_vb_ = nullptr;
	safe_graphics_vb_ = nullptr;
	safe_4x3_vb_ = nullptr;
	safe_center_left_vb_ = nullptr;
	safe_center_top_vb_ = nullptr;
	safe_center_right_vb_ = nullptr;
	safe_area_vb_init_ = false;
}

void MultiviewWindow::render_safe_area(int cellIndex, int vrX, int vrY, int vrW, int vrH)
{
	if (cellIndex < 0 || cellIndex >= (int)effective_visuals_.size())
		return;

	const SafeAreaSettings &sa = effective_visuals_[cellIndex].safeArea;
	if (!sa.enabled)
		return;

	if (vrW <= 0 || vrH <= 0)
		return;

	/* Lazily init vertex buffers (we're already in graphics context during render) */
	if (!safe_area_vb_init_)
		init_safe_area_vbs();

	/* Compute color with user-specified opacity */
	uint8_t alpha = (uint8_t)(sa.opacity * 255.0);
	uint32_t saColor = ((uint32_t)alpha << 24) | (sa.color & 0x00FFFFFF);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	/* Helper lambda: render a vertex buffer scaled to SignalRect */
	auto renderVB = [&](gs_vertbuffer_t *vb) {
		if (!vb)
			return;

		gs_load_vertexbuffer(vb);

		gs_matrix_push();
		/* Translate to signal rect origin, then scale normalized coords to signal size */
		gs_matrix_translate3f((float)vrX, (float)vrY, 0.0f);
		gs_matrix_scale3f((float)vrW, (float)vrH, 1.0f);

		gs_effect_set_color(colorParam, saColor);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw(GS_LINESTRIP, 0, 0);

		gs_matrix_pop();
	};

	/* Draw all safe area guides */
	renderVB(safe_action_vb_);
	renderVB(safe_graphics_vb_);
	renderVB(safe_4x3_vb_);
	renderVB(safe_center_left_vb_);
	renderVB(safe_center_top_vb_);
	renderVB(safe_center_right_vb_);
}

/* ---- VU Meter ---- */

/* Practical silence level in dB (below minimum display range) */
#define VU_SILENCE_DB -200.0f

void MultiviewWindow::volmeter_callback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
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
void MultiviewWindow::source_mute_callback(void *data, calldata_t *cd)
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
void MultiviewWindow::source_audio_mixers_callback(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	auto *self = static_cast<MultiviewWindow *>(data);
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
uint32_t MultiviewWindow::compute_active_track_bit()
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

	/* AutoFollow: query streaming output mixer mask.
	 * obs_frontend_get_streaming_output() returns +1 ref; release after use. */
	obs_output_t *so = obs_frontend_get_streaming_output();
	uint32_t mask = 0;
	if (so) {
		mask = (uint32_t)obs_output_get_mixers(so);
		obs_output_release(so);
	}
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

void MultiviewWindow::rebuild_volmeters()
{
	release_volmeters();

	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	/* Refresh the active track bit before collecting sources so that
	 * track-filtered enumeration uses the up-to-date mask. */
	current_track_bit_ = compute_active_track_bit();

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
		if (cs.type.empty())
			continue;

		obs_source_t *cellSrc = nullptr;
		bool isPgm = false;

		if (cs.type == "pgm") {
			cellSrc = obs_frontend_get_current_scene();
			isPgm = true;
			has_pgm_cell_ = true;
		} else if (cs.type == "prvw") {
			cellSrc = obs_frontend_get_current_preview_scene();
			if (!cellSrc) {
				/* Studio Mode disabled: PRVW has no separate scene, so it
				 * 100% mirrors PGM (matches render() fallback). Treat the
				 * cell as PGM for VU purposes — also enables the global
				 * channel 1..5 sweep below. */
				cellSrc = obs_frontend_get_current_scene();
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
		OBSSourceAutoRelease pgm = obs_frontend_get_current_scene();
		last_pgm_scene_ = pgm ? OBSGetWeakRef(pgm) : nullptr;
	}
	if (has_prvw_cell_) {
		OBSSourceAutoRelease prvw = obs_frontend_get_current_preview_scene();
		last_prvw_scene_ = prvw ? OBSGetWeakRef(prvw) : nullptr;
	}

	/* Single summary line — replaces the per-source LOG_INFO that used to
	 * fire inside the attach loop. With 1Hz active-source polling triggering
	 * rebuilds on any scene-tree mutation, per-source logs flooded the OBS
	 * log; one line per rebuild is sufficient for diagnostics. The instance
	 * prefix `[name(uuid8)]` lets logs stay readable when several Multiview
	 * windows are open simultaneously. */
	obs_log(LOG_INFO, "[%s(%s)] VU meters rebuilt: cells=%d sources=%d track_bit=0x%x%s%s",
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

void MultiviewWindow::check_scene_change_for_volmeters()
{
	bool needRebuild = false;

	if (has_pgm_cell_) {
		OBSSourceAutoRelease pgm = obs_frontend_get_current_scene();
		OBSWeakSource currentWeak = pgm ? OBSGetWeakRef(pgm) : nullptr;
		if (currentWeak != last_pgm_scene_)
			needRebuild = true;
	}
	if (!needRebuild && has_prvw_cell_) {
		OBSSourceAutoRelease prvw = obs_frontend_get_current_preview_scene();
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
void MultiviewWindow::check_active_track_change()
{
	if (volmeters_rebuild_requested_.exchange(false, std::memory_order_acquire)) {
		rebuild_volmeters();
		return; /* rebuild already refreshed current_track_bit_ + last_active_sources_ */
	}

	uint64_t now = os_gettime_ns();
	const uint64_t POLL_INTERVAL_NS = 1000000000ULL; /* 1 second */
	if (last_track_poll_ns_ != 0 && (now - last_track_poll_ns_) < POLL_INTERVAL_NS)
		return;
	last_track_poll_ns_ = now;

	uint32_t newBit = compute_active_track_bit();
	if (newBit != current_track_bit_) {
		rebuild_volmeters();
		return;
	}

	/* Compare the currently-eligible source set against the snapshot from
	 * the last rebuild. Any difference (gained/lost source) means a rebuild
	 * is needed. This is the catch-all for cases where signal-based
	 * notification is missing (Mic gaining Track 1 from outside, scene
	 * tree edits, etc.). */
	std::vector<void *> currentActive;
	collect_active_source_pointers(currentActive, newBit);
	if (currentActive != last_active_sources_)
		rebuild_volmeters();
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
void MultiviewWindow::collect_active_source_pointers(std::vector<void *> &out, uint32_t track_bit)
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
			cellSrc = obs_frontend_get_current_scene();
			isPgm = true;
		} else if (cs.type == "prvw") {
			cellSrc = obs_frontend_get_current_preview_scene();
			if (!cellSrc) {
				/* Studio Mode off: PRVW mirrors PGM. Match rebuild path so
				 * the polling-based set comparison stays consistent across
				 * studio-mode toggles. */
				cellSrc = obs_frontend_get_current_scene();
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

void MultiviewWindow::release_volmeters()
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

void MultiviewWindow::render_vu_meter(int cellIndex, const CellRect &cell, int vpX, int vpY, int sigX, int sigY,
				      int sigW, int sigH)
{
	if (cellIndex < 0 || cellIndex >= (int)effective_visuals_.size())
		return;

	const VuMeterSettings &vmSettings = effective_visuals_[cellIndex].vuMeter;
	if (!vmSettings.enabled || vmSettings.opacity <= 0.0)
		return;

	if (cellIndex >= (int)cell_volmeters_.size() || !cell_volmeters_[cellIndex])
		return;

	CellVolmeter *cellVm = cell_volmeters_[cellIndex];

	/* Compute max peak across all audio sources in this cell.
	 * If a source's callback hasn't fired in 200ms, treat it as silent
	 * (source may have become inactive after a scene switch). */
	uint64_t now = os_gettime_ns();
	const uint64_t STALE_THRESHOLD_NS = 200000000ULL; /* 200ms */

	float peakMax = VU_SILENCE_DB;
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
		for (int c = 0; c < ch && c < MAX_AUDIO_CHANNELS; c++) {
			float p = sv->peak[c];
			if (std::isfinite(p) && p > peakMax)
				peakMax = p;
		}
	}

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

	if (cellVm->last_render_ns > 0) {
		double deltaS = (double)(now - cellVm->last_render_ns) * 1e-9;
		if (deltaS > 0.0 && deltaS < 1.0) {
			if (peakMax >= cellVm->displayPeak) {
				cellVm->displayPeak = peakMax;
			} else {
				cellVm->displayPeak -= decayRate * (float)deltaS;
				if (cellVm->displayPeak < peakMax)
					cellVm->displayPeak = peakMax;
			}
		} else {
			cellVm->displayPeak = peakMax;
		}
	} else {
		cellVm->displayPeak = peakMax;
	}
	cellVm->last_render_ns = now;

	float smoothedPeak = cellVm->displayPeak;

	/* Clamp and normalize: -60 dB .. 0 dB -> 0.0 .. 1.0 */
	const float minDB = -60.0f;
	const float maxDB = 0.0f;
	if (smoothedPeak < minDB)
		smoothedPeak = minDB;
	if (smoothedPeak > maxDB)
		smoothedPeak = maxDB;
	float level = (smoothedPeak - minDB) / (maxDB - minDB);

	if (level <= 0.0f)
		return;

	/* Determine bar geometry based on position and anchor mode */
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
			offset = 0; /* anchor at -∞ end (left for horizontal) */
		} else {
			offset = (anchorW - barFullLen) / 2; /* center */
		}
		barX = anchorX + offset;
		barY = (vmSettings.position == VuMeterPosition::Top) ? anchorY : anchorY + anchorH - barW;
	} else {
		barFullLen = (int)(anchorH * vmSettings.lengthRatio + 0.5);
		int offset;
		if (vmSettings.alignment == VuMeterAlignment::Start) {
			offset = anchorH - barFullLen; /* anchor at -∞ end (bottom for vertical) */
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

	for (int s = 0; s < 3; s++) {
		float segStart = segments[s].start;
		float segEnd = segments[s].end;

		/* Clip segment to actual level */
		if (level <= segStart)
			break;
		float drawEnd = (level < segEnd) ? level : segEnd;

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
			startRegion(drawX, barY, pixLen, barW, 0.0f, (float)pixLen, 0.0f, (float)barW);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, pixLen, barW);
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
			startRegion(barX, drawY, barW, pixLen, 0.0f, (float)barW, 0.0f, (float)pixLen);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, barW, pixLen);
			endRegion();
		}
	}

	gs_blend_state_pop();
}

/* ---- PGM / PRVW cell highlight borders ----
 *
 * Strategy: each frame we snapshot the recursive active-source trees of the
 * current PGM and PRVW scenes into raw-pointer hash sets. For each rendered
 * cell, the cell's resolved source is classified by:
 *   1. cell.type == "pgm"  → PgmDirect (uses obs_render_main_texture)
 *   2. cell.type == "prvw" → PrvwDirect when Studio Mode is on, else PgmDirect
 *      (PRVW visually falls back to PGM, so the highlight should follow suit)
 *   3. scene/source cell whose pointer == current PGM scene → PgmDirect
 *   4. scene/source cell whose pointer == current PRVW scene → PrvwDirect
 *   5. scene/source cell whose pointer is in pgm_tree_set_ → PgmNested
 *   6. scene/source cell whose pointer is in prvw_tree_set_ → PrvwNested
 *
 * Priority: PgmDirect > PrvwDirect > PgmNested > PrvwNested. Direct matches
 * always render as solid borders; nested matches render as dashed when
 * HighlightSettings.nestedDashed is true, solid otherwise.
 *
 * Border geometry: thickness defaults to gutter_px_ so the border fills the
 * gutter (matches OBS native multiview look). When gutter == 0, we fall back
 * to a thin inner inset of HighlightSettings.minThicknessPx so the border
 * remains visible without spilling outside the cell.
 *
 * No per-cell override: highlight is a window-wide concept driven by the
 * scene-tree of the OBS frontend, so the resolve chain in
 * resolve_effective_visual_settings only considers Global + Instance. */

void MultiviewWindow::refresh_highlight_tree_sets()
{
	pgm_tree_set_.clear();
	prvw_tree_set_.clear();
	OBSSourceAutoRelease pgm = obs_frontend_get_current_scene();
	if (pgm)
		collect_tree_sources(pgm, pgm_tree_set_);
	OBSSourceAutoRelease prvw = obs_frontend_get_current_preview_scene();
	if (prvw)
		collect_tree_sources(prvw, prvw_tree_set_);
}

MultiviewWindow::HighlightKind MultiviewWindow::compute_cell_highlight(int cellIndex)
{
	if (cellIndex < 0 || cellIndex >= (int)cell_sources_.size())
		return HighlightKind::None;

	const auto &cs = cell_sources_[cellIndex];

	/* Dedicated PGM / PRVW viewer cells already serve as the visual primary
	 * monitor of their respective bus, so an additional colored border on
	 * top of them is just noise. The OBS-native multiview behaves the same
	 * way — only scene/source cells get the red/green outline. */
	if (cs.type == "pgm" || cs.type == "prvw")
		return HighlightKind::None;

	/* Scene/source cell: resolve to a raw pointer for set comparison.
	 * We pull a strong ref to ensure the pointer is valid while compared,
	 * and release it at scope end. */
	if (cs.type.empty() || !cs.weak_ref)
		return HighlightKind::None;

	OBSSourceAutoRelease cellHolder = OBSGetStrongRef(cs.weak_ref);
	obs_source_t *cellSrc = cellHolder;
	if (!cellSrc)
		return HighlightKind::None;

	OBSSourceAutoRelease pgm = obs_frontend_get_current_scene();
	OBSSourceAutoRelease prvw = obs_frontend_get_current_preview_scene();

	if (pgm && cellSrc == pgm.Get())
		return HighlightKind::PgmDirect;
	if (prvw && cellSrc == prvw.Get())
		return HighlightKind::PrvwDirect;
	/* PGM nested check first so PGM outranks PRVW when both contain the
	 * same source nested inside. */
	if (pgm_tree_set_.find(cellSrc) != pgm_tree_set_.end())
		return HighlightKind::PgmNested;
	if (prvw_tree_set_.find(cellSrc) != prvw_tree_set_.end())
		return HighlightKind::PrvwNested;
	return HighlightKind::None;
}

void MultiviewWindow::render_cell_highlight(const CellRect &cell, int vpX, int vpY, HighlightKind kind,
					    const HighlightSettings &hs)
{
	if (kind == HighlightKind::None || !hs.enabled)
		return;

	uint32_t color = (kind == HighlightKind::PgmDirect || kind == HighlightKind::PgmNested) ? hs.pgmColor
												: hs.prvwColor;

	const bool nested = (kind == HighlightKind::PgmNested || kind == HighlightKind::PrvwNested);
	const bool dashed = nested && hs.nestedDashed;

	int cellX = cell.x + vpX;
	int cellY = cell.y + vpY;
	int cellW = cell.w;
	int cellH = cell.h;
	if (cellW <= 0 || cellH <= 0)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	/* draw_rect: emit a single filled rectangle in absolute viewport coords.
	 * Mirrors the pattern used by the gutter fill, label background, and
	 * PRVW fallback yellow bar above.
	 *
	 * NOTE: gs_effect_set_color must be called BEFORE each effect_loop —
	 * the SOLID effect's color uniform does not survive across separate
	 * loop invocations in a reliable way. Hoisting the color set above the
	 * 4-rect / dash sequence produced "only the first rect colored" bug
	 * (top edge red, the other three edges white). */
	auto draw_rect = [&](int x, int y, int w, int h) {
		if (w <= 0 || h <= 0)
			return;
		startRegion(x, y, w, h, 0.0f, (float)w, 0.0f, (float)h);
		gs_effect_set_color(colorParam, color);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, w, h);
		endRegion();
	};

	/* Determine geometry mode:
	 *   gutter_px_ > 0  → "gutter fill" mode: border occupies the
	 *                     gutter zone OUTSIDE the cell rect (matches
	 *                     OBS native multiview).
	 *   gutter_px_ == 0 → "inset" mode: border drawn INSIDE the cell
	 *                     at thickness = hs.minThicknessPx so it
	 *                     remains visible with zero-gutter layouts.
	 *
	 * In both modes we draw 4 rectangles (top/bottom/left/right) for
	 * solid borders. Dashed borders walk each side and emit a series
	 * of small rectangles separated by `dashGapPx`. */
	int t;
	int outerX, outerY, outerW, outerH; /* full outer bounding box */
	if (gutter_px_ > 0) {
		t = gutter_px_;
		outerX = cellX - t;
		outerY = cellY - t;
		outerW = cellW + 2 * t;
		outerH = cellH + 2 * t;
	} else {
		t = hs.minThicknessPx > 0 ? hs.minThicknessPx : 1;
		outerX = cellX;
		outerY = cellY;
		outerW = cellW;
		outerH = cellH;
	}

	/* Tiny-cell safety: when the outer box is smaller than 2*t in either
	 * dimension, the 4 "side strip" rects (solid mode) and 4 corner
	 * squares (dashed mode) start to overlap/invert. In the gutter==0
	 * inset case this also means the left/right strips would paint
	 * outside the cell (overflowing into the neighbour or window
	 * background). Skip the border entirely — a cell that small is
	 * unreadable anyway. */
	if (outerW < 2 * t || outerH < 2 * t)
		return;

	if (!dashed) {
		/* Solid 4-rect border. Side strips overlap at corners but
		 * that's harmless for opaque fills. */
		draw_rect(outerX, outerY, outerW, t);                          /* top */
		draw_rect(outerX, outerY + outerH - t, outerW, t);             /* bottom */
		draw_rect(outerX, outerY + t, t, outerH - 2 * t);              /* left */
		draw_rect(outerX + outerW - t, outerY + t, t, outerH - 2 * t); /* right */
		return;
	}

	/* Dashed border with miter-clean corners.
	 *
	 * Step 1: paint 4 solid t×t corner squares. This guarantees every
	 * corner is a closed 90° L regardless of dash phase — fixes the
	 * "gap at the corner" / "misaligned tip" artefact that occurs when
	 * the first/last dash on adjacent sides don't line up.
	 *
	 * Step 2: walk each side BETWEEN the corner squares and emit short
	 * dashes. Horizontal sides span [outerX+t, outerX+outerW-t],
	 * vertical sides span [outerY+t, outerY+outerH-t]. The last dash is
	 * truncated if it would overshoot the available span. */
	draw_rect(outerX, outerY, t, t);                           /* TL */
	draw_rect(outerX + outerW - t, outerY, t, t);              /* TR */
	draw_rect(outerX, outerY + outerH - t, t, t);              /* BL */
	draw_rect(outerX + outerW - t, outerY + outerH - t, t, t); /* BR */

	int dash = hs.dashLengthPx > 0 ? hs.dashLengthPx : 1;
	int gap = hs.dashGapPx > 0 ? hs.dashGapPx : 1;
	int period = dash + gap;

	int hSpan = outerW - 2 * t; /* horizontal run between L/R corner squares */
	int vSpan = outerH - 2 * t; /* vertical run between T/B corner squares */

	/* Top side */
	for (int off = 0; off < hSpan; off += period) {
		int segLen = dash;
		if (off + segLen > hSpan)
			segLen = hSpan - off;
		draw_rect(outerX + t + off, outerY, segLen, t);
	}
	/* Bottom side */
	for (int off = 0; off < hSpan; off += period) {
		int segLen = dash;
		if (off + segLen > hSpan)
			segLen = hSpan - off;
		draw_rect(outerX + t + off, outerY + outerH - t, segLen, t);
	}
	/* Left side */
	for (int off = 0; off < vSpan; off += period) {
		int segLen = dash;
		if (off + segLen > vSpan)
			segLen = vSpan - off;
		draw_rect(outerX, outerY + t + off, t, segLen);
	}
	/* Right side */
	for (int off = 0; off < vSpan; off += period) {
		int segLen = dash;
		if (off + segLen > vSpan)
			segLen = vSpan - off;
		draw_rect(outerX + outerW - t, outerY + t + off, t, segLen);
	}
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

	/* Safe Area toggle (instance-level) */
	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			bool safeEnabled = false;
			/* Check instance effective safe area state */
			if (inst->visualSettings.safeAreaMode == InheritanceMode::Override)
				safeEnabled = inst->visualSettings.safeArea.enabled;
			else
				safeEnabled = config_->global_settings().visualSettings.safeArea.enabled;

			QAction *safeAreaAction = menu.addAction(QStringLiteral("Safe Area"));
			safeAreaAction->setCheckable(true);
			safeAreaAction->setChecked(safeEnabled);
			connect(safeAreaAction, &QAction::triggered, this, [this, safeEnabled]() {
				MultiviewInstance *inst = config_->find_instance(uuid_);
				if (!inst)
					return;
				/* Switch to override mode and toggle */
				inst->visualSettings.safeAreaMode = InheritanceMode::Override;
				inst->visualSettings.safeArea.enabled = !safeEnabled;
				config_->save();
				refresh_visual_settings();
			});
		}
	}

	/* VU Meter toggle (instance-level) */
	{
		MultiviewInstance *inst = config_->find_instance(uuid_);
		if (inst) {
			bool vuEnabled = false;
			if (inst->visualSettings.vuMeterMode == InheritanceMode::Override)
				vuEnabled = inst->visualSettings.vuMeter.enabled;
			else
				vuEnabled = config_->global_settings().visualSettings.vuMeter.enabled;

			QAction *vuAction = menu.addAction(QStringLiteral("VU Meter"));
			vuAction->setCheckable(true);
			vuAction->setChecked(vuEnabled);
			connect(vuAction, &QAction::triggered, this, [this, vuEnabled]() {
				MultiviewInstance *inst = config_->find_instance(uuid_);
				if (!inst)
					return;
				inst->visualSettings.vuMeterMode = InheritanceMode::Override;
				inst->visualSettings.vuMeter.enabled = !vuEnabled;
				config_->save();
				refresh_visual_settings();
			});
		}
	}

	/* Cell Display Settings (per-cell) */
	if (cellIndex >= 0) {
		QAction *cellSettingsAction = menu.addAction(QStringLiteral("Cell Display Settings..."));
		connect(cellSettingsAction, &QAction::triggered, this, [this, cellIndex]() {
			MultiviewInstance *inst = config_->find_instance(uuid_);
			if (!inst)
				return;

			/* Find cell (row, col) from engine */
			LayoutEngine tmpEngine;
			tmpEngine.set_layout(inst->layout);
			tmpEngine.set_viewport(100, 100);
			tmpEngine.compute();
			const auto &cells = tmpEngine.cells();
			if (cellIndex >= (int)cells.size())
				return;
			int row = cells[cellIndex].gridRow;
			int col = cells[cellIndex].gridCol;

			/* Find or create CellVisualSettings for this cell */
			CellVisualSettings *cvs = nullptr;
			for (auto &c : inst->cellVisualSettings) {
				if (c.row == row && c.col == col) {
					cvs = &c;
					break;
				}
			}
			CellVisualSettings temp;
			if (!cvs) {
				temp.row = row;
				temp.col = col;
				cvs = &temp;
			}

			CellDisplaySettingsDialog dlg(CellDisplaySettingsDialog::Mode::Cell, this);
			dlg.set_cell_position(row, col);
			dlg.set_cell_settings(*cvs);
			if (dlg.exec() == QDialog::Accepted) {
				CellVisualSettings result = dlg.get_cell_settings();
				result.row = row;
				result.col = col;

				/* Update or insert */
				bool found = false;
				for (auto &c : inst->cellVisualSettings) {
					if (c.row == row && c.col == col) {
						c = result;
						found = true;
						break;
					}
				}
				if (!found)
					inst->cellVisualSettings.push_back(result);

				config_->save();
				refresh_visual_settings();
			}
		});
	}

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
