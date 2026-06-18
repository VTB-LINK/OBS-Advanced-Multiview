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
#include "amv-logging.hpp"
#include "amv-i18n.hpp"
#include "cell-display-settings-dialog.hpp"
#include "edit-source-dialog.hpp"
#include "signal-lost-settings-dialog.hpp"
#include "signal-provider.hpp"
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
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

static inline QSize GetPixelSize(QWidget *widget)
{
	return widget->size() * widget->devicePixelRatioF();
}

/* ---- MultiviewWindow implementation ---- */

MultiviewWindow::MultiviewWindow(ConfigManager *config, const std::string &uuid, QWidget *parent, bool startVisible)
	: QWidget(parent, Qt::Window),
	  config_(config),
	  uuid_(uuid),
	  headless_(!startVisible)
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

	/* The shared per-instance core owns sources / volmeters / output / render
	 * logic. Phase 1 (issue #10): one core per window (1:1). */
	core_ = std::make_unique<AmvInstanceCore>(config_, uuid_);
	core_->refresh_layout();

	/* Resume any persisted external output (issue #11) for this instance. */
	core_->apply_output_settings();

	ready_ = true;

	/* A headless render host (startVisible=false) stays hidden: no show(),
	 * so visibleChanged never fires and no OBS display is created. It keeps
	 * all cell state live and is driven by the global output callback. */
	if (startVisible) {
		show();
		activateWindow();
	}
}

MultiviewWindow::~MultiviewWindow()
{
	ready_ = false;
	destroy_display();
	/* destroy_display() removed the draw callback, so no render is in flight;
	 * the core dtor releases sources + tears the output sender/texrender down
	 * (graphics thread). */
	core_.reset();
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
		setWindowTitle(amv::text("AMVPlugin.Multiview.Title").arg(QString::fromStdString(inst->name)));
}

void MultiviewWindow::render_callback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MultiviewWindow *>(data);
	if (!self->ready_)
		return;
	self->render(cx, cy);
}

void MultiviewWindow::render(uint32_t cx, uint32_t cy)
{
	/* Display path: advance per-frame state, then paint the on-screen view.
	 * The external-output pass is NOT here — it is driven globally by
	 * obs_add_main_render_callback (render_output_only), so output runs even
	 * when this window is closed/headless (issue #11). For a visible window
	 * this display callback owns the per-frame tick; the global driver only
	 * ticks headless hosts (see plugin-main on_main_render). */
	core_->tick_once_per_frame();

	/* Compute canvas-aspect-ratio viewport (centered, with black borders) */
	const double canvasAspect = core_->canvas_aspect();
	double windowAspect = (double)cx / (double)cy;
	int vpX = 0, vpY = 0;
	int vpW = (int)cx, vpH = (int)cy;

	if (windowAspect > canvasAspect) {
		/* Window wider than canvas → pillarbox (black on sides) */
		vpW = (int)((double)cy * canvasAspect);
		vpX = ((int)cx - vpW) / 2;
	} else if (windowAspect < canvasAspect) {
		/* Window taller than canvas → letterbox (black on top/bottom) */
		vpH = (int)((double)cx / canvasAspect);
		vpY = ((int)cy - vpH) / 2;
	}

	/* Compute THIS window's layout at its own size, then paint the shared cells.
	 * Display ALWAYS renders natively into the centered viewport at full window
	 * resolution — output never downgrades the on-screen image. */
	if (vpW != cached_vpW_ || vpH != cached_vpH_) {
		engine_.set_layout(core_->layout());
		engine_.set_viewport(vpW, vpH);
		engine_.compute();
		cached_vpW_ = vpW;
		cached_vpH_ = vpH;
	}
	core_->draw_cells(engine_.cells(), vpX, vpY, vpW, vpH);
}

void MultiviewWindow::enter_headless()
{
	if (is_headless())
		return;
	/* Drop to a hidden, display-less render host: remove the OBS display
	 * (its draw callback) and hide the window, but keep cell_sources_ and
	 * output_ alive so the global driver keeps emitting output. */
	headless_.store(true, std::memory_order_relaxed);
	destroy_display();
	hide();
}

void MultiviewWindow::exit_headless()
{
	if (!is_headless())
		return;
	headless_.store(false, std::memory_order_relaxed);
	show();
	activateWindow();
	/* visibleChanged normally creates the display; create it directly too in
	 * case the handle was already visible-without-display. */
	if (!display_created_)
		create_display();
}

/* ---- Label rendering ---- */

void MultiviewWindow::closeEvent(QCloseEvent *event)
{
	/* Issue #11 Phase 2: if external output is still enabled for this
	 * instance, closing the projector must NOT stop the output. Instead drop
	 * to a headless render host — destroy the display + hide, but keep
	 * cell_sources_ and output_ alive (the global driver keeps emitting). The
	 * host is only fully torn down later when output is disabled. We keep the
	 * source refs because the host must keep rendering the grid. */
	if (has_output()) {
		event->accept();
		enter_headless();
		return;
	}

	/* No output: full close. Stop rendering and release the core immediately
	 * (its dtor calls obs_source_dec_showing), which stops screen-capture
	 * sources from signaling the OS (yellow border) without waiting for the
	 * deferred window deletion. */
	ready_ = false;
	destroy_display();
	core_.reset();

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

bool MultiviewWindow::event(QEvent *event)
{
	if (event->type() == QEvent::Expose) {
		if (!display_created_)
			create_display();
	}
	return QWidget::event(event);
}

/* ---- forwarders to the shared core ----
 *
 * The window holds the core 1:1 for now (issue #10 Phase 1). These keep the
 * existing plugin-main / manager / context-menu call sites working while the
 * shared state lives on the core. */

void MultiviewWindow::refresh_layout()
{
	if (core_)
		core_->refresh_layout();
	/* Force this view's layout engine to recompute at its own size next frame. */
	cached_vpW_ = 0;
	cached_vpH_ = 0;
}

void MultiviewWindow::refresh_sources()
{
	if (core_)
		core_->refresh_sources();
}

void MultiviewWindow::refresh_sources_lazy()
{
	if (core_)
		core_->refresh_sources_lazy();
}

bool MultiviewWindow::refresh_cell(int row, int col)
{
	return core_ ? core_->refresh_cell(row, col) : false;
}

void MultiviewWindow::on_source_being_removed(obs_source_t *source)
{
	if (core_)
		core_->on_source_being_removed(source);
}

void MultiviewWindow::on_source_just_created(obs_source_t *source)
{
	if (core_)
		core_->on_source_just_created(source);
}

void MultiviewWindow::refresh_visual_settings()
{
	if (core_)
		core_->refresh_visual_settings();
	cached_vpW_ = 0;
	cached_vpH_ = 0;
}

void MultiviewWindow::refresh_signal_settings()
{
	if (core_)
		core_->refresh_signal_settings();
}

bool MultiviewWindow::force_reconnect_cell(int cellIndex)
{
	return core_ ? core_->force_reconnect_cell(cellIndex) : false;
}

void MultiviewWindow::apply_output_settings()
{
	if (core_)
		core_->apply_output_settings();
}

void MultiviewWindow::tick_frame()
{
	if (core_)
		core_->tick_once_per_frame();
}

void MultiviewWindow::render_output_only()
{
	if (core_)
		core_->render_output_only();
}
