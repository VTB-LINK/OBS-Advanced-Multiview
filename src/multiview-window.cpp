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
	rebuild_scale_label_sources();
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
		cell_sources_[i].state = SignalRuntimeState::Empty;
		cell_sources_[i].last_active_ns = 0;
		cell_sources_[i].last_reconnect_ns = 0;
		cell_sources_[i].retry_attempt = 0;

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
		if (ca->type == "pgm" || ca->type == "prvw") {
			cell_sources_[i].state = SignalRuntimeState::Active;
			continue;
		}

		/* Scene/Source: cache weak ref and inc_showing */
		obs_source_t *src = obs_get_source_by_name(ca->name.c_str());
		if (src) {
			cell_sources_[i].weak_ref = OBSGetWeakRef(src);
			obs_source_inc_showing(src);
			cell_sources_[i].showing = true;
			cell_sources_[i].state = SignalRuntimeState::Active;
			obs_source_release(src);
		} else {
			/* Phase 3 / M5: name resolves to nothing right now — mark as missing.
			 * The Phase 2 lazy re-resolve path in render() can still recover it
			 * (e.g. user undoes a deletion); we just record the state here. */
			cell_sources_[i].state = SignalRuntimeState::MissingInternal;
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

		/* Phase 3 / M5: drop shared status overlay text sources alongside
		 * label sources so the lifetime is identical and no text source
		 * outlives the runtime that referenced it. */
		release_status_text_sources();

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

	/* Release scale label sources */
	release_scale_label_sources();

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

			/* Phase 3 / M5.1a: track signal runtime state without
			 * altering Phase 2 visual behavior. State is only consumed
			 * by future M5 work (Missing Source overlay, Reconnect Now,
			 * fallback). PGM/PRVW are always considered Active because
			 * they target whichever scene is currently selected. */
			SignalRuntimeState newState = cs.state;
			if (cs.type.empty()) {
				newState = SignalRuntimeState::Empty;
			} else if (cs.type == "pgm" || cs.type == "prvw") {
				newState = SignalRuntimeState::Active;
			} else if (src) {
				newState = SignalRuntimeState::Active;
			} else {
				newState = SignalRuntimeState::MissingInternal;
			}
			if (newState != cs.state)
				cell_sources_[i].state = newState;
			if (newState == SignalRuntimeState::Active) {
				cell_sources_[i].last_active_ns = os_gettime_ns();
				cell_sources_[i].retry_attempt = 0;
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

		/* Phase 3 / M5: status overlay (Missing Source for now). Rendered
		 * after label so a Below-mode label area never gets covered, and
		 * before VU meter / highlight which are intentionally on top. */
		render_status_overlay(i, cellX, cellY, cell.w, cell.h);

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