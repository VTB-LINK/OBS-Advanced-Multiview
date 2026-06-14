/*
OBS Advanced Multiview - Label text source management and rendering
Split from multiview-window.cpp for maintainability.
All functions remain members of MultiviewWindow.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "multiview-window.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <string>

/* ---- helpers (same as OBS internal, duplicated for compilation unit) ---- */

static inline void startRegion(int vX, int vY, int vCX, int vCY, float oL, float oR, float oT, float oB)
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
void MultiviewWindow::rebuild_label_sources()
{
	std::lock_guard<std::recursive_mutex> lock(source_mutex_);

	/* We create private text sources for each cell that needs a label. */
	LayoutEngine tmpEngine;
	tmpEngine.set_layout(layout_);
	tmpEngine.set_viewport(cached_vpW_ > 0 ? cached_vpW_ : 800, cached_vpH_ > 0 ? cached_vpH_ : 600);
	tmpEngine.compute();

	const auto &cells = tmpEngine.cells();
	size_t cellCount = cells.size();

	/* Resize label_sources_ to match cell count */
	label_sources_.resize(cellCount);

	for (size_t i = 0; i < cellCount; i++) {
		/* Determine label text from cell assignment */
		std::string labelText;
		if (i < cell_sources_.size()) {
			const auto &cs = cell_sources_[i];
			if (cs.type == "pgm")
				labelText = "PGM";
			else if (cs.type == "prvw")
				labelText = "PRVW";
			else if (!cs.name.empty())
				labelText = cs.name;
		}

		/* Determine effective label settings */
		const LabelSettings *ls = nullptr;
		if (i < effective_visuals_.size())
			ls = &effective_visuals_[i].label;

		/* If label is disabled or no text, release source */
		if (labelText.empty() || !ls || ls->displayMode == LabelDisplayMode::None) {
			label_sources_[i].source = nullptr;
			label_sources_[i].text.clear();
			continue;
		}

		/* Create source if needed */
		if (!label_sources_[i].source) {
			std::string srcName = "adv_mv_label_" + uuid_ + "_" + std::to_string(i);

			/* Pad text with spaces for horizontal padding (same as OBS native) */
			std::string paddedText = " " + labelText + " ";

			/* Use maxFontSize for ScaleWithCell so we always scale DOWN
			 * (avoids blurry upscaling of small bitmap textures) */
			int renderFontSize = ls->fontSize;
			if (ls->fontScaleMode == FontScaleMode::ScaleWithCell)
				renderFontSize = ls->maxFontSize > 0 ? ls->maxFontSize : 72;

#ifdef _WIN32
			/* Windows: use text_gdiplus for automatic CJK font fallback */
			const char *fontFace = ls->fontFamily.empty() ? "Arial" : ls->fontFamily.c_str();
			obs_data_t *fontObj = obs_data_create();
			obs_data_set_int(fontObj, "size", renderFontSize);
			obs_data_set_string(fontObj, "face", fontFace);
			obs_data_set_int(fontObj, "flags", 1); /* Bold */

			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "text", paddedText.c_str());
			obs_data_set_obj(settings, "font", fontObj);
			obs_data_set_int(settings, "color", ls->textColor);
			obs_data_set_int(settings, "opacity", 50); /* OBS native uses 50% */
			obs_data_set_bool(settings, "outline", false);
			obs_data_set_int(settings, "align", 0); /* left */

			obs_source_t *src = obs_source_create_private("text_gdiplus", srcName.c_str(), settings);
			obs_data_release(settings);
			obs_data_release(fontObj);
#else
			/* Mac/Linux: use text_ft2_source_v2 with CJK-capable font */
			const char *fontFace = ls->fontFamily.empty()
#ifdef __APPLE__
						       ? "Helvetica"
#else
						       ? "Monospace"
#endif
						       : ls->fontFamily.c_str();
			obs_data_t *fontObj = obs_data_create();
			obs_data_set_int(fontObj, "size", renderFontSize);
			obs_data_set_string(fontObj, "face", fontFace);
			obs_data_set_int(fontObj, "flags", 1); /* Bold */

			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "text", paddedText.c_str());
			obs_data_set_obj(settings, "font", fontObj);
			obs_data_set_int(settings, "color1", ls->textColor);
			obs_data_set_int(settings, "color2", ls->textColor);
			obs_data_set_bool(settings, "outline", false);
			obs_data_set_bool(settings, "drop_shadow", false);

			obs_source_t *src = obs_source_create_private("text_ft2_source_v2", srcName.c_str(), settings);
			obs_data_release(settings);
			obs_data_release(fontObj);

			if (!src) {
				/* Fallback: try text_ft2_source */
				fontObj = obs_data_create();
				obs_data_set_int(fontObj, "size", renderFontSize);
				obs_data_set_string(fontObj, "face", fontFace);
				obs_data_set_int(fontObj, "flags", 1);

				settings = obs_data_create();
				obs_data_set_string(settings, "text", paddedText.c_str());
				obs_data_set_obj(settings, "font", fontObj);
				obs_data_set_int(settings, "color1", ls->textColor);
				obs_data_set_int(settings, "color2", ls->textColor);
				src = obs_source_create_private("text_ft2_source", srcName.c_str(), settings);
				obs_data_release(settings);
				obs_data_release(fontObj);
			}
#endif

			label_sources_[i].source = src;
			label_sources_[i].text = labelText;
			label_sources_[i].color = ls->textColor;
			label_sources_[i].fontSize = renderFontSize;
			label_sources_[i].fontFamily = ls->fontFamily;
			obs_source_release(src);
		} else {
			int renderFontSize = ls->fontSize;
			if (ls->fontScaleMode == FontScaleMode::ScaleWithCell)
				renderFontSize = ls->maxFontSize > 0 ? ls->maxFontSize : 72;

			if (label_sources_[i].text != labelText || label_sources_[i].color != ls->textColor ||
			    label_sources_[i].fontSize != renderFontSize ||
			    label_sources_[i].fontFamily != ls->fontFamily) {
				/* Update existing source text/color/font */
				std::string paddedText = " " + labelText + " ";

#ifdef _WIN32
				const char *fontFace = ls->fontFamily.empty() ? "Arial" : ls->fontFamily.c_str();
#elif __APPLE__
				const char *fontFace = ls->fontFamily.empty() ? "Helvetica" : ls->fontFamily.c_str();
#else
				const char *fontFace = ls->fontFamily.empty() ? "Monospace" : ls->fontFamily.c_str();
#endif
				obs_data_t *fontObj = obs_data_create();
				obs_data_set_int(fontObj, "size", renderFontSize);
				obs_data_set_string(fontObj, "face", fontFace);
				obs_data_set_int(fontObj, "flags", 1);

				obs_data_t *settings = obs_source_get_settings(label_sources_[i].source);
				obs_data_set_string(settings, "text", paddedText.c_str());
				obs_data_set_obj(settings, "font", fontObj);
#ifdef _WIN32
				obs_data_set_int(settings, "color", ls->textColor);
#else
				obs_data_set_int(settings, "color1", ls->textColor);
				obs_data_set_int(settings, "color2", ls->textColor);
#endif
				obs_source_update(label_sources_[i].source, settings);
				obs_data_release(settings);
				obs_data_release(fontObj);
				label_sources_[i].text = labelText;
				label_sources_[i].color = ls->textColor;
				label_sources_[i].fontSize = renderFontSize;
				label_sources_[i].fontFamily = ls->fontFamily;
			}
		}
	}
}

void MultiviewWindow::render_label(int cellIndex, const CellRect &cell, int vpX, int vpY)
{
	if (cellIndex < 0 || cellIndex >= (int)effective_visuals_.size())
		return;

	const LabelSettings &ls = effective_visuals_[cellIndex].label;
	if (ls.displayMode == LabelDisplayMode::None)
		return;

	if (cellIndex >= (int)label_sources_.size() || !label_sources_[cellIndex].source)
		return;

	obs_source_t *labelSrc = label_sources_[cellIndex].source;
	uint32_t labelW = obs_source_get_width(labelSrc);
	uint32_t labelH = obs_source_get_height(labelSrc);
	if (labelW == 0 || labelH == 0)
		return;

	int cellX = cell.x + vpX;
	int cellY = cell.y + vpY;

	/* Compute target label height based on scale mode */
	int targetH;
	if (ls.fontScaleMode == FontScaleMode::ScaleWithCell) {
		/* Scale: match OBS native effective size.
		 * OBS creates font at (canvas_h/3)/9.81 and renders at ppiScaleY≈0.5,
		 * giving effective text height ≈ cell_h / 14.7 for scene cells. */
		targetH = (int)((double)cell.h / 14.7 + 0.5);
		int minH = ls.minFontSize;
		int maxH = ls.maxFontSize;
		if (targetH < minH)
			targetH = minH;
		if (targetH > maxH)
			targetH = maxH;
	} else {
		/* Fixed: use fontSize directly as display height (1:1 with texture) */
		targetH = ls.fontSize;
		if (targetH < 8)
			targetH = 8;
	}

	/* Scale factor to fit label into target height */
	float scaleFactor = (float)targetH / (float)labelH;
	int drawW = (int)((float)labelW * scaleFactor);
	int drawH = targetH;

	const int margin = std::min(std::max(0, ls.margin), std::max(0, (cell.w - 1) / 2));
	const int maxTextW = std::max(1, cell.w - margin * 2);

	/* Clamp label width to cell width, accounting for configured label
	 * padding so Margin has a visible effect without forcing the label
	 * background outside the cell. */
	if (drawW > maxTextW) {
		float clamp = (float)maxTextW / (float)drawW;
		drawW = maxTextW;
		drawH = (int)((float)drawH * clamp);
		scaleFactor *= clamp;
	}

	/* Vertical padding for background (symmetric, like OBS native).
	 * OBS uses thickness=6 at canvas level, rendered at ppiScaleY≈0.5,
	 * giving effective ~3px padding per side for 270px cells. */
	int thickness = (int)((double)cell.h * 3.0 / 270.0 + 0.5);
	if (thickness < 1)
		thickness = 1;
	int bgH = drawH + thickness * 2 + margin * 2;
	int bgW = drawW + margin * 2;

	/* Calculate position */
	int bgX, bgY;

	/* Horizontal centering */
	bgX = cellX + (cell.w - bgW) / 2;

	if (ls.displayMode == LabelDisplayMode::Below) {
		/* Below mode: center in label region */
		int labelRegionH = cell.h / 6;
		if (labelRegionH < 16)
			labelRegionH = 16;
		bgY = cellY + cell.h - labelRegionH + (labelRegionH - bgH) / 2;
	} else if (ls.position == LabelPosition::Top) {
		bgY = cellY + thickness;
	} else {
		/* Bottom (Overlay) - offset upward from cell edge */
		bgY = cellY + cell.h - bgH - thickness * 3;
	}

	/* Draw semi-transparent background behind label (OBS approach:
	 * bg = full source width, source height + 2*thickness) */
	if (ls.backgroundOpacity > 0.01) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

		uint8_t alpha = (uint8_t)(ls.backgroundOpacity * 255.0);
		uint32_t bgColor = ((uint32_t)alpha << 24) | 0x000000;

		startRegion(bgX, bgY, bgW, bgH, 0.0f, (float)bgW, 0.0f, (float)bgH);
		gs_effect_set_color(colorParam, bgColor);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, bgW, bgH);
		endRegion();
	}

	/* Render text source centered within background box
	 * (text at y-offset = thickness, same width as bg since spaces provide h-padding) */
	int textX = bgX + margin;
	int textY = bgY + thickness + margin;
	startRegion(textX, textY, drawW, drawH, 0.0f, (float)labelW, 0.0f, (float)labelH);
	obs_source_video_render(labelSrc);
	endRegion();
}
