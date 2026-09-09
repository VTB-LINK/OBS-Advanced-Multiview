/*
OBS Advanced Multiview - DeckLink output backend (issue #16)

Transmits the composed multiview frame to a Blackmagic DeckLink SDI/HDMI
output by reusing OBS's built-in "decklink_output" output type (zero DeckLink
SDK dependency). The frame is read back from the GPU (GS_BGRA texrender ->
staging surface -> CPU) and pushed into a self-owned BGRA video_t that is wired
to the output via obs_output_set_media. Independent of OBS's source/scene
system (Approach B), like the Spout/NDI backends.

All output / video_t / hardware create/start/stop/release/close happen on the
UI thread (obs_queue_task OBS_TASK_UI); the graphics thread only stages + reads
back + pushes frames. See docs/issue-16-decklink-output-design.md.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#ifdef AMV_ENABLE_DECKLINK_OUTPUT

#include "multiview-output.hpp"

#include <memory>

/* Factory: a fresh DeckLink backend. The output + video_t + hardware are
 * acquired lazily on the UI thread once a device/mode is configured; the
 * graphics-thread staging surfaces are acquired on the first submit_frame. */
std::unique_ptr<IMultiviewOutputBackend> create_decklink_output_backend();

#endif /* AMV_ENABLE_DECKLINK_OUTPUT */
