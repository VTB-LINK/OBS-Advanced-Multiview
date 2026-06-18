/*
OBS Advanced Multiview - frontend scene/streaming cache (issue #10 isolation F2)

Process-wide cache of OBS frontend state (current program scene, preview scene,
streaming output) updated ONLY on the main thread from frontend event callbacks,
and read safely from the graphics/render thread.

Why: obs_frontend_get_current_scene() / get_current_preview_scene() /
get_streaming_output() read unlocked OBSBasic members (verified in the OBS
source). Calling them from the render thread races a main-thread scene switch
(torn read of a ref-counted handle -> possible use-after-free -> OBS crash). The
render path reads this cache instead so it never touches the frontend, which also
avoids the N-views multiplication of those calls in the multi-window phase.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include <obs.hpp>

#include <cstdint>

namespace amv_frontend {

/* Main thread only: re-read current program/preview scene + streaming output.
 * Call from frontend event callbacks (scene/preview/studio/streaming changes)
 * and once at startup / on window creation. */
void refresh();

/* Release cached refs. Call on module unload (main thread). */
void shutdown();

/* Thread-safe reads (graphics thread OK). Each returns a fresh strong ref that
 * the caller owns (may be null). preview is null when Studio Mode is off. */
OBSSourceAutoRelease current_program_scene();
OBSSourceAutoRelease current_preview_scene();

/* Live mixer-track mask of the streaming output (0 when not streaming). Reads
 * obs_output_get_mixers on the cached output, so mid-stream track edits are
 * still reflected without a frontend call on the render thread. */
uint32_t streaming_mixers();

} // namespace amv_frontend
