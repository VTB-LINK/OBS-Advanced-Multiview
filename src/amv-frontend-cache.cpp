/*
OBS Advanced Multiview - frontend scene/streaming cache (issue #10 isolation F2)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-frontend-cache.hpp"

#include <obs-frontend-api.h>

#include <mutex>

namespace {
std::mutex g_mtx;
OBSWeakSource g_program;
OBSWeakSource g_preview;
OBSOutputAutoRelease g_stream_output;
} // namespace

namespace amv_frontend {

void refresh()
{
	/* Main thread: query the frontend OUTSIDE our lock (don't hold g_mtx across
	 * obs_frontend_* calls), then swap the cached refs under the lock. */
	OBSSourceAutoRelease prog = obs_frontend_get_current_scene();
	OBSSourceAutoRelease prev = obs_frontend_get_current_preview_scene();
	OBSWeakSource progW = prog ? OBSGetWeakRef(prog) : OBSWeakSource();
	OBSWeakSource prevW = prev ? OBSGetWeakRef(prev) : OBSWeakSource();
	OBSOutputAutoRelease so = obs_frontend_get_streaming_output();

	std::lock_guard<std::mutex> lk(g_mtx);
	g_program = progW;
	g_preview = prevW;
	g_stream_output = std::move(so);
}

void shutdown()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	g_program = nullptr;
	g_preview = nullptr;
	g_stream_output = nullptr;
}

OBSSourceAutoRelease current_program_scene()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	/* obs_weak_source_get_source returns a +1 strong ref (null-safe); adopt it. */
	return OBSSourceAutoRelease(obs_weak_source_get_source(g_program));
}

OBSSourceAutoRelease current_preview_scene()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	return OBSSourceAutoRelease(obs_weak_source_get_source(g_preview));
}

uint32_t streaming_mixers()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	return g_stream_output ? (uint32_t)obs_output_get_mixers(g_stream_output) : 0;
}

} // namespace amv_frontend
