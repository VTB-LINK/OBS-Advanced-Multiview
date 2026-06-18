/*
OBS Advanced Multiview - per-instance render/state core (issue #10)

Construction / destruction. The bulk of the logic lives in the sibling
translation units: source resolution / refresh / tick / output dispatch in
amv-instance-core-sources.cpp, the grid composition in
amv-instance-core-draw.cpp, and the per-feature render helpers in
amv-instance-core-{vu,label,image,safe-area,highlight,status,health,lost-image}.cpp.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "amv-instance-core.hpp"
#include "amv-frontend-cache.hpp"
#include "multiview-output.hpp"

#include <obs.h>

AmvInstanceCore::AmvInstanceCore(ConfigManager *config, const std::string &uuid)
	: QObject(nullptr),
	  config_(config),
	  uuid_(uuid)
{
	/* Canvas aspect ratio from OBS base resolution (a per-instance/global
	 * property, not a window property). */
	obs_video_info ovi;
	if (obs_get_video_info(&ovi))
		canvas_aspect_ = (double)ovi.base_width / (double)ovi.base_height;

	/* Safety net (F2): cores are created on the main thread (window open /
	 * output enable). Prime the frontend cache so the first render pass has a
	 * valid program/preview scene even if no frontend event fired since load. */
	amv_frontend::refresh();
}

AmvInstanceCore::~AmvInstanceCore()
{
	/* Release all source refs (dec_showing/dec_active pairing + textures +
	 * volmeters), then tear down the output sender/texrender. Mirrors the old
	 * MultiviewWindow dtor; the caller has already removed any display draw
	 * callback so no render is in flight. */
	release_source_refs();
	output_.reset();
}
