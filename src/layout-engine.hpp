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

#pragma once

#include "multiview-instance.hpp"

#include <optional>
#include <vector>

struct CellRect {
	int x;
	int y;
	int w;
	int h;
	int gridRow;    /* top-left row in the base grid */
	int gridCol;    /* top-left col in the base grid */
	int rowSpan;    /* 1 for normal cells */
	int colSpan;    /* 1 for normal cells */
	int spanIndex;  /* index into LayoutData::spans, or -1 */
};

struct VideoRect {
	int x;
	int y;
	int w;
	int h;
};

enum class HitType { Cell, Gutter };

struct HitTestResult {
	HitType type;
	int cellIndex; /* index into cells_, -1 for gutter */
};

class LayoutEngine {
public:
	LayoutEngine();

	void set_layout(const LayoutData &layout);
	void set_viewport(int width, int height);

	/* Recompute all rects. Call after set_layout or set_viewport. */
	void compute();

	/* Query */
	const std::vector<CellRect> &cells() const;
	int cell_count() const;

	VideoRect video_rect(int cellIndex, int srcWidth,
			     int srcHeight) const;

	std::optional<HitTestResult> hit_test(int x, int y) const;

	/* Span validation */
	enum class SpanError {
		None,
		OutOfBounds,
		TooSmall,
		Overlaps,
	};

	SpanError validate_span(const SpanRegion &span) const;
	SpanError validate_span(const SpanRegion &span,
				int excludeSpanIndex) const;
	static SpanError validate_all_spans(const LayoutData &layout);

private:
	bool is_cell_covered_by_span(int row, int col,
				     int excludeSpanIndex = -1) const;

	LayoutData layout_;
	int vp_width_ = 0;
	int vp_height_ = 0;
	std::vector<CellRect> cells_;
};
