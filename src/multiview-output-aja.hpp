/*
OBS Advanced Multiview - AJA output backend (issue #18)

Transmits the composed multiview frame to an AJA SDI/HDMI output by reusing OBS's
built-in "aja_output" output type (zero NTV2 SDK dependency). The composed frame
is read back from the GPU (GS_BGRA texrender -> staging surface -> CPU) and pushed
into a self-owned BGRA video_t opened at the CANVAS size; OBS's aja_output installs
its own video conversion (to the SDI raster + UYVY/BGR3) at start, so libobs's raw
video scaler resizes + reformats for us. Independent of OBS's source/scene system
(Approach B), like the Spout/NDI/DeckLink backends.

Key differences from DeckLink (see docs/issue-18-aja-output-design.md §0):
 - video_t is opened at the canvas size, NOT the SDI raster (AJA cannot report its
   raster before the output is created).
 - the heavy hardware work (acquire channel, wait for vertical interrupt, spawn the
   output thread) happens in aja_output_create, so create — not just start/stop —
   must run on the UI thread.
 - each output must carry a process-unique owner id (kUIPropAJAOutputID) or the AJA
   CardManager mis-tracks channel ownership; it is generated fresh per create and
   never persisted.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#ifdef AMV_ENABLE_AJA_OUTPUT

#include "multiview-output.hpp"

#include <memory>

/* Factory: a fresh AJA backend. The output + video_t + hardware are acquired
 * lazily on the UI thread once a valid device/io/format is configured; the
 * graphics-thread staging surfaces are acquired on the first submit_frame. */
std::unique_ptr<IMultiviewOutputBackend> create_aja_output_backend();

#endif /* AMV_ENABLE_AJA_OUTPUT */
