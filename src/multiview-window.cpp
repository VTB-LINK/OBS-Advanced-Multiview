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

/* Issue #10 perf: global multiview-window compose divisor (1=Full, 2=Half). Set
 * on the main thread (config load / Settings tab), read on the graphics thread
 * (render()). Relaxed atomic — a one-frame-stale value is harmless. */
static std::atomic<int> g_window_fps_divisor{2};
void multiview_set_window_fps_divisor(int divisor)
{
	g_window_fps_divisor.store((divisor == 1) ? 1 : 2, std::memory_order_relaxed);
}
int multiview_window_fps_divisor()
{
	return g_window_fps_divisor.load(std::memory_order_relaxed);
}

/* ---- MultiviewWindow implementation ---- */

MultiviewWindow::MultiviewWindow(ConfigManager *config, AmvInstanceCore *core, QWidget *parent)
	: QWidget(parent, Qt::Window),
	  config_(config),
	  uuid_(core ? core->uuid() : std::string()),
	  core_(core)
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

	/* Window title (plugin-main assigns the final window number right after
	 * construction; default to 1 until then). */
	refresh_title();

	/* Escape: exit fullscreen first (back to the windowed projector), then close
	 * on a second press when already windowed. Mirrors how a fullscreen projector
	 * is expected to behave — the first Escape shouldn't tear the window down. */
	QAction *escAction = new QAction(this);
	escAction->setShortcut(Qt::Key_Escape);
	addAction(escAction);
	connect(escAction, &QAction::triggered, this, [this]() {
		if (isFullScreen())
			on_toggle_fullscreen(); /* exit fullscreen (showNormal + restore geometry) */
		else
			close();
	});

	/* Create display when window becomes visible */
	connect(windowHandle(), &QWindow::visibleChanged, this, [this](bool visible) {
		if (visible && !closing_ && !display_created_)
			create_display();
	});

	ready_ = true;

	/* Every projector opens at the default size, centered on its screen — OBS
	 * projectors don't remember geometry (issue #10: each open is a fresh
	 * window, so there is nothing to restore). */
	show();
	if (QScreen *scr = screen()) {
		const QRect avail = scr->availableGeometry();
		move(avail.x() + (avail.width() - width()) / 2, avail.y() + (avail.height() - height()) / 2);
	}
	activateWindow();
}

MultiviewWindow::~MultiviewWindow()
{
	ready_ = false;
	destroy_display();
	/* destroy_display() removed the draw callback, so no render is in flight.
	 * The core is owned by plugin-main and shared with other views — NEVER
	 * release it here. */
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

	/* Free the compose texrender (issue #10 perf). gs_texrender_destroy needs a
	 * graphics context; destroy_display runs on the UI thread (closeEvent/dtor),
	 * and the draw callback is already removed above, so no render is in flight. */
	if (compose_tr_) {
		obs_enter_graphics();
		destroy_compose_texrender();
		obs_leave_graphics();
	}
}

void MultiviewWindow::set_window_number(int number)
{
	window_number_ = number;
	refresh_title();
}

void MultiviewWindow::refresh_title()
{
	MultiviewInstance *inst = config_->find_instance(uuid_);
	if (inst)
		setWindowTitle(amv::text("AMVPlugin.Multiview.TitleWindow")
				       .arg(QString::fromStdString(inst->name))
				       .arg(window_number_));
}

void MultiviewWindow::invalidate_layout()
{
	/* Force engine_.compute() at this view's own size on the next frame. */
	cached_vpW_ = 0;
	cached_vpH_ = 0;
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

	/* Compose-rate divisor (issue #10 perf): Half composes the (heavy) grid at
	 * half the OBS fps into a texrender and blits the last result every display
	 * frame, so the window stays smooth while the per-window render cost ~halves.
	 * Half only makes sense above 30fps base; otherwise compose directly. */
	struct obs_video_info ovi;
	const bool haveOvi = obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0;
	const double baseFps = haveOvi ? (double)ovi.fps_num / (double)ovi.fps_den : 60.0;
	int divisor = g_window_fps_divisor.load(std::memory_order_relaxed);
	/* Force Full when the base rate is unknown or already <=30: the Half path
	 * below divides by ovi.fps_num, so a failed obs_get_video_info (ovi
	 * indeterminate) must never reach it. In practice OVI is always valid while
	 * a display renders; this is the broadcast-grade guard. */
	if (!haveOvi || baseFps <= 30.0)
		divisor = 1;

	if (divisor <= 1) {
		/* Full: compose straight to the display (no texrender overhead). */
		if (compose_tr_)
			destroy_compose_texrender();
		/* Issue #20 (P3): a nested "follow window" cell composes its target at
		 * this window's canvas-aspect render area (vpW x vpH — the default
		 * render-target size), so B keeps the SAME aspect as A's own grid. Every
		 * instance is canvas-shaped, so B matches A's other cells rather than
		 * stretching to the raw window aspect. */
		core_->draw_cells(engine_.cells(), vpX, vpY, vpW, vpH);
		return;
	}

	/* Half: throttle the compose into compose_tr_, blit it every frame. */
	const uint64_t now = os_gettime_ns();
	const uint64_t frameNs = (uint64_t)(1000000000.0 * ovi.fps_den / ovi.fps_num);
	const uint64_t composeIntervalNs = (uint64_t)(frameNs * 1.5); /* ~every 2nd frame -> half fps */
	const bool sizeChanged = (compose_tr_w_ != vpW || compose_tr_h_ != vpH);
	const bool composeNow = !compose_tr_valid_ || sizeChanged || (now - last_compose_ns_) >= composeIntervalNs;

	if (composeNow) {
		if (!compose_tr_ || sizeChanged) {
			destroy_compose_texrender();
			compose_tr_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
			compose_tr_w_ = vpW;
			compose_tr_h_ = vpH;
		}
		if (compose_tr_) {
			gs_texrender_reset(compose_tr_);
			if (gs_texrender_begin(compose_tr_, vpW, vpH)) {
				gs_set_viewport(0, 0, vpW, vpH);
				gs_ortho(0.0f, (float)vpW, 0.0f, (float)vpH, -100.0f, 100.0f);
				struct vec4 clr;
				vec4_set(&clr, 0.0f, 0.0f, 0.0f, 1.0f);
				gs_clear(GS_CLEAR_COLOR, &clr, 0.0f, 0);
				/* Issue #20 (P3): follow-window nested cells compose at this
				 * pass's canvas-aspect render area (vpW x vpH), matching A's
				 * grid aspect (see the Full path above). */
				core_->draw_cells(engine_.cells(), 0, 0, vpW, vpH);
				gs_texrender_end(compose_tr_);
				compose_tr_valid_ = true;
				last_compose_ns_ = now;
			}
		}
	}

	/* Blit the last composed frame into the centered viewport (OBS already
	 * cleared the display to black, so the pill/letterbox borders stay black). */
	if (compose_tr_valid_ && compose_tr_) {
		gs_texture_t *tex = gs_texrender_get_texture(compose_tr_);
		if (tex) {
			gs_viewport_push();
			gs_projection_push();
			gs_set_viewport(vpX, vpY, vpW, vpH);
			gs_ortho(0.0f, (float)vpW, 0.0f, (float)vpH, -100.0f, 100.0f);
			gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(eff, "image");
			gs_effect_set_texture(image, tex);
			while (gs_effect_loop(eff, "Draw"))
				gs_draw_sprite(tex, 0, vpW, vpH);
			gs_projection_pop();
			gs_viewport_pop();
		}
	}
}

void MultiviewWindow::destroy_compose_texrender()
{
	if (compose_tr_) {
		gs_texrender_destroy(compose_tr_);
		compose_tr_ = nullptr;
	}
	compose_tr_w_ = compose_tr_h_ = 0;
	compose_tr_valid_ = false;
}

void MultiviewWindow::closeEvent(QCloseEvent *event)
{
	/* Issue #10: a view is just one of N projectors onto a shared core. Closing
	 * it tears down THIS window's display only — never the core. plugin-main's
	 * on_window_closed decides whether the core can go (only when no views are
	 * left AND output is disabled); if output is still on, the core lives on as
	 * a headless host and the global driver keeps emitting.
	 *
	 * Remove the display callback BEFORE emitting, so no render is in flight
	 * when plugin-main may destroy the core. After the emit, core_ may already
	 * be dangling (if this was the last view) — do not touch it. */
	ready_ = false;
	closing_ = true;
	destroy_display();

	emit window_closed(this, uuid_);
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
		if (!closing_ && !display_created_)
			create_display();
	}
	return QWidget::event(event);
}

/* ---- forwarders to the shared core (context-menu call sites) ----
 *
 * The core is shared by all N views of the instance, so a single forwarded call
 * updates every view. plugin-main drives the broader notify_* / source-signal
 * paths against the core directly (once per core), not through these. */

void MultiviewWindow::refresh_sources()
{
	if (core_)
		core_->refresh_sources();
}

bool MultiviewWindow::refresh_cell(int row, int col)
{
	return core_ ? core_->refresh_cell(row, col) : false;
}

void MultiviewWindow::refresh_visual_settings()
{
	if (core_)
		core_->refresh_visual_settings();
	invalidate_layout();
}

bool MultiviewWindow::force_reconnect_cell(int cellIndex)
{
	return core_ ? core_->force_reconnect_cell(cellIndex) : false;
}
