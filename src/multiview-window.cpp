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
#include <graphics/matrix4.h>
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
#endif

/* ---- helpers (same as OBS internal) ---- */

static inline void startRegion(int vX, int vY, int vCX, int vCY, float oL,
			       float oR, float oT, float oB)
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

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX,
					int windowCY, int &x, int &y,
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

MultiviewWindow::MultiviewWindow(ConfigManager *config,
				 const std::string &uuid, QWidget *parent)
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
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (inst) {
		setWindowTitle(QStringLiteral("Multiview - %1")
				       .arg(QString::fromStdString(
					       inst->name)));
	}

	/* Escape to close */
	QAction *escAction = new QAction(this);
	escAction->setShortcut(Qt::Key_Escape);
	addAction(escAction);
	connect(escAction, &QAction::triggered, this, &QWidget::close);

	/* Create display when window becomes visible */
	connect(windowHandle(), &QWindow::visibleChanged, this,
		[this](bool visible) {
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

void MultiviewWindow::refresh_layout()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	layout_ = inst->layout;
	gutter_px_ = inst->effective_gutter(
		config_->global_settings().defaultGutterPx);

	/* Update layout with effective gutter */
	layout_.gutterPx = gutter_px_;

	/* Engine will be recomputed in render callback with actual viewport size */
}

void MultiviewWindow::refresh_sources()
{
	release_source_refs();
	update_source_refs();
}

void MultiviewWindow::update_source_refs()
{
	std::lock_guard<std::mutex> lock(source_mutex_);

	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	/* Recompute layout to know cell count */
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(800, 600); /* dummy size for cell count */
	tmpEngine.compute();

	size_t cellCount = tmpEngine.cells().size();
	cell_sources_.resize(cellCount);

	for (size_t i = 0; i < cellCount; i++) {
		cell_sources_[i].type.clear();
		cell_sources_[i].weak_ref = nullptr;
		cell_sources_[i].showing = false;
		cell_sources_[i].prvw_fallback = false;

		if (i >= inst->cellAssignments.size())
			continue;

		const CellAssignment &ca = inst->cellAssignments[i];
		if (ca.type.empty())
			continue;

		cell_sources_[i].type = ca.type;

		/* PGM/PRVW are resolved per-frame in render(), no caching */
		if (ca.type == "pgm" || ca.type == "prvw")
			continue;

		/* Scene/Source: cache weak ref and inc_showing */
		obs_source_t *src = obs_get_source_by_name(ca.name.c_str());
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
	std::lock_guard<std::mutex> lock(source_mutex_);

	for (auto &cs : cell_sources_) {
		if (cs.showing) {
			OBSSourceAutoRelease src =
				OBSGetStrongRef(cs.weak_ref);
			if (src)
				obs_source_dec_showing(src);
			cs.showing = false;
		}
		cs.weak_ref = nullptr;
	}
	cell_sources_.clear();
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
	std::lock_guard<std::mutex> lock(source_mutex_);

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

	/* Recompute layout within the aspect-correct viewport */
	engine_.set_layout(layout_);
	engine_.set_viewport(vpW, vpH);
	engine_.compute();

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
				srcHolder =
					obs_frontend_get_current_preview_scene();
				if (!srcHolder) {
					/* No Studio Mode → fallback to PGM */
					srcHolder =
						obs_frontend_get_current_scene();
					isPrvwFallback = (srcHolder != nullptr);
				}
				src = srcHolder;
			} else if (cs.weak_ref) {
				/* scene/source: use cached ref */
				srcHolder = OBSGetStrongRef(cs.weak_ref);
				src = srcHolder;
			}
		}

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
				/* Source not ready, draw black */
				startRegion(cellX, cellY, cell.w, cell.h,
					    0.0f, (float)cell.w, 0.0f,
					    (float)cell.h);
				gs_effect_set_color(colorParam, 0xFF000000);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, cell.w,
						       cell.h);
				endRegion();
				continue;
			}

			/* Calculate letterbox/pillarbox rect */
			VideoRect vr = engine_.video_rect(i, srcW, srcH);
			int vrX = vr.x + vpX;
			int vrY = vr.y + vpY;

			/* Draw black background for the entire cell first */
			startRegion(cellX, cellY, cell.w, cell.h, 0.0f,
				    (float)cell.w, 0.0f, (float)cell.h);
			gs_effect_set_color(colorParam, 0xFF000000);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, cell.w, cell.h);
			endRegion();

			/* Render into video rect */
			startRegion(vrX, vrY, vr.w, vr.h, 0.0f,
				    (float)srcW, 0.0f, (float)srcH);
			if (isPgm)
				obs_render_main_texture();
			else
				obs_source_video_render(src);
			endRegion();

			/* Draw PRVW fallback indicator (yellow bar at bottom) */
			if (isPrvwFallback) {
				int barH = (std::max)(2, cell.h / 20);
				startRegion(cellX, cellY + cell.h - barH,
					    cell.w, barH, 0.0f,
					    (float)cell.w, 0.0f, (float)barH);
				/* Yellow with some transparency: 0xCC00D4FF (ABGR) */
				gs_effect_set_color(colorParam, 0xCC00D4FF);
				while (gs_effect_loop(solid, "Solid"))
					gs_draw_sprite(nullptr, 0, cell.w,
						       barH);
				endRegion();
			}
		} else {
			/* Empty cell - draw dark background */
			startRegion(cellX, cellY, cell.w, cell.h, 0.0f,
				    (float)cell.w, 0.0f, (float)cell.h);
			gs_effect_set_color(colorParam, 0xFF1A1A1A);
			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, cell.w, cell.h);
			endRegion();
		}
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
	QAction *fullscreenAction =
		menu.addAction(QStringLiteral("Fullscreen"));
	fullscreenAction->setCheckable(true);
	fullscreenAction->setChecked(isFullScreen());
	connect(fullscreenAction, &QAction::triggered, this,
		&MultiviewWindow::on_toggle_fullscreen);

	/* Always on top */
	QAction *onTopAction =
		menu.addAction(QStringLiteral("Always on Top"));
	onTopAction->setCheckable(true);
	onTopAction->setChecked(is_always_on_top_);
	connect(onTopAction, &QAction::triggered, this,
		&MultiviewWindow::on_toggle_always_on_top);

	menu.addSeparator();

	if (cellIndex >= 0) {
		/* Check if cell has a source assigned */
		bool hasSource = false;
		{
			std::lock_guard<std::mutex> lock(source_mutex_);
			if (cellIndex < (int)cell_sources_.size() &&
			    !cell_sources_[cellIndex].type.empty())
				hasSource = true;
		}

		if (hasSource) {
			QAction *changeAction = menu.addAction(
				QStringLiteral("Change Source..."));
			connect(changeAction, &QAction::triggered, this,
				[this, cellIndex]() {
					on_change_source(cellIndex);
				});

			QAction *clearAction =
				menu.addAction(QStringLiteral("Clear Cell"));
			connect(clearAction, &QAction::triggered, this,
				[this, cellIndex]() {
					on_clear_cell(cellIndex);
				});
		} else {
			QAction *addAction = menu.addAction(
				QStringLiteral("Add Source..."));
			connect(addAction, &QAction::triggered, this,
				[this, cellIndex]() {
					on_add_source(cellIndex);
				});
		}

		menu.addSeparator();
	}

	QAction *editGridAction =
		menu.addAction(QStringLiteral("Edit Grid..."));
	connect(editGridAction, &QAction::triggered, this,
		&MultiviewWindow::on_edit_grid);

	QAction *saveAction =
		menu.addAction(QStringLiteral("Save Cell Assignments"));
	connect(saveAction, &QAction::triggered, this,
		&MultiviewWindow::on_save_assignments);

	menu.addSeparator();

	QAction *settingsAction =
		menu.addAction(QStringLiteral("Global Settings"));
	connect(settingsAction, &QAction::triggered, this,
		&MultiviewWindow::on_global_settings);

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

	/* Ensure cellAssignments vector is large enough */
	while ((int)inst->cellAssignments.size() <= cellIndex)
		inst->cellAssignments.push_back(CellAssignment{"", ""});

	inst->cellAssignments[cellIndex] = ca;
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

	if (cellIndex < (int)inst->cellAssignments.size()) {
		inst->cellAssignments[cellIndex] = CellAssignment{"", ""};
		inst->signalDirty = true;
		config_->save();
		refresh_sources();
	}
}

void MultiviewWindow::on_save_assignments()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (!inst)
		return;

	inst->signalDirty = false;
	config_->save();
	obs_log(LOG_INFO, "cell assignments saved for '%s'",
		inst->name.c_str());
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

	Qt::WindowFlags flags = windowFlags();
	if (is_always_on_top_)
		flags |= Qt::WindowStaysOnTopHint;
	else
		flags &= ~Qt::WindowStaysOnTopHint;

	setWindowFlags(flags);
	show(); /* Required after changing window flags */
}
