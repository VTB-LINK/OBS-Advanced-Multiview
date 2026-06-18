/*
OBS Advanced Multiview - per-instance render/state core (issue #10)

Skeleton (Phase 1, commit 1): compiles and links but is not yet wired in.
Shared state + content-render logic migrate here from MultiviewWindow in
subsequent commits.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "multiview-output.hpp"

#include <obs.h>

AmvInstanceCore::AmvInstanceCore(ConfigManager *config, const std::string &uuid) : config_(config), uuid_(uuid)
{
	/* Canvas aspect ratio from OBS base resolution (a per-instance/global
	 * property, not a window property). */
	obs_video_info ovi;
	if (obs_get_video_info(&ovi))
		canvas_aspect_ = (double)ovi.base_width / (double)ovi.base_height;
}

AmvInstanceCore::~AmvInstanceCore() = default;

void AmvInstanceCore::tick_once_per_frame()
{
	/* Migrates from MultiviewWindow::tick_frame with a frame-token gate. */
}

void AmvInstanceCore::draw_cells(const std::vector<CellRect> &cells, int vpX, int vpY, int vpW, int vpH)
{
	(void)cells;
	(void)vpX;
	(void)vpY;
	(void)vpW;
	(void)vpH;
	/* Migrates from the per-cell half of MultiviewWindow::draw_grid. */
}

void AmvInstanceCore::render_output_only()
{
	/* Migrates from MultiviewWindow::render_output_only. */
}
