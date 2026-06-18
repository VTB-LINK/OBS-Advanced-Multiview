/*
OBS Advanced Multiview - per-instance render/state core (issue #10)

One AmvInstanceCore exists per AMV instance (keyed by uuid). It owns the
SHARED, stream-pulling, content-rendering state that must exist exactly once
per instance no matter how many projector windows are open:

  - the cell sources (incl. private ffmpeg/ndi/spout/vlc sources that pull
    streams — created ONCE here, never per-window, so two windows of one
    instance do not double-pull a stream),
  - per-frame snapshots (PGM/PRVW scene trees, active audio track),
  - the audio volmeters, the label/status/image render resources,
  - the external output (Spout/NDI) manager (issue #11).

`MultiviewWindow` becomes a thin VIEW (QWidget + OBSDisplay + its own
LayoutEngine/viewport) that holds a pointer to its core and renders by calling
draw_cells() with the cells it computed for its own size. N views attach to one
core. A core with zero views but output enabled is the issue-#11 "headless
host" generalized.

This is the skeleton (issue #10 Phase 1, commit 1): the class exists and
compiles but is not yet wired in. Subsequent commits move the shared state and
logic out of MultiviewWindow into here.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "config-manager.hpp"
#include "layout-engine.hpp"

#include <obs.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* Owns the shared per-instance state + content render logic. Methods that touch
 * shared state run on the OBS graphics thread (serialized) or the UI thread;
 * see the threading notes as members are migrated in. */
class AmvInstanceCore {
public:
	AmvInstanceCore(ConfigManager *config, const std::string &uuid);
	~AmvInstanceCore();

	AmvInstanceCore(const AmvInstanceCore &) = delete;
	AmvInstanceCore &operator=(const AmvInstanceCore &) = delete;

	const std::string &uuid() const { return uuid_; }

	/* Layout/config accessors a VIEW reads to feed its own LayoutEngine. */
	const LayoutData &layout() const { return layout_; }
	int gutter_px() const { return gutter_px_; }
	double canvas_aspect() const { return canvas_aspect_; }

	/* Advance once-per-frame shared state. Idempotent within a frame via a
	 * frame token so the first of N views (or the output driver) ticks and the
	 * rest no-op. (Body migrates from MultiviewWindow::tick_frame.) */
	void tick_once_per_frame();

	/* Paint the multiview composition for a caller-computed cell layout into the
	 * current render target's viewport. Acquires source_mutex_. (Body migrates
	 * from the per-cell half of MultiviewWindow::draw_grid.) */
	void draw_cells(const std::vector<CellRect> &cells, int vpX, int vpY, int vpW, int vpH);

	/* External output (issue #11): does this core currently emit any output? */
	bool has_output() const { return output_ != nullptr; }

	/* Offscreen output pass (no display). (Body migrates from
	 * MultiviewWindow::render_output_only.) */
	void render_output_only();

private:
	ConfigManager *config_ = nullptr;
	std::string uuid_;

	LayoutData layout_;
	int gutter_px_ = 0;
	double canvas_aspect_ = 16.0 / 9.0;

	/* Output manager pointer placeholder; real type + members migrate in a
	 * later commit (kept null in the skeleton). */
	std::unique_ptr<class MultiviewOutputManager> output_;

	/* once-per-frame tick de-dup token (see tick_once_per_frame). */
	uint64_t last_tick_token_ = 0;
};
