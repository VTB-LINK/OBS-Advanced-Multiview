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

#include "layout-engine.hpp"

#include <algorithm>
#include <cmath>
#include <set>

LayoutEngine::LayoutEngine() = default;

void LayoutEngine::set_layout(const LayoutData &layout)
{
	layout_ = layout;
}

void LayoutEngine::set_viewport(int width, int height)
{
	vp_width_ = width;
	vp_height_ = height;
}

/* ---- compute ---- */

void LayoutEngine::compute()
{
	cells_.clear();

	int rows = layout_.rows;
	int cols = layout_.columns;
	int gutter = layout_.gutterPx;

	if (rows < 1 || cols < 1 || vp_width_ < 1 || vp_height_ < 1)
		return;

	/*
	 * Layout model:
	 *   gutter | cell | gutter | cell | ... | cell | gutter
	 *
	 * Total gutters horizontally: cols + 1
	 * Total gutters vertically:   rows + 1
	 */
	int total_gutter_w = gutter * (cols + 1);
	int total_gutter_h = gutter * (rows + 1);

	int avail_w = vp_width_ - total_gutter_w;
	int avail_h = vp_height_ - total_gutter_h;

	if (avail_w < cols)
		avail_w = cols;
	if (avail_h < rows)
		avail_h = rows;

	/* Base cell size (integer division, remainder distributed) */
	int base_cw = avail_w / cols;
	int base_ch = avail_h / rows;
	int extra_w = avail_w % cols;
	int extra_h = avail_h % rows;

	/* Precompute column x-offsets and widths */
	std::vector<int> col_x(cols);
	std::vector<int> col_w(cols);
	{
		int x = gutter;
		for (int c = 0; c < cols; c++) {
			col_x[c] = x;
			col_w[c] = base_cw + (c < extra_w ? 1 : 0);
			x += col_w[c] + gutter;
		}
	}

	/* Precompute row y-offsets and heights */
	std::vector<int> row_y(rows);
	std::vector<int> row_h(rows);
	{
		int y = gutter;
		for (int r = 0; r < rows; r++) {
			row_y[r] = y;
			row_h[r] = base_ch + (r < extra_h ? 1 : 0);
			y += row_h[r] + gutter;
		}
	}

	/* Mark which base cells are covered by spans */
	/* Build span cells first, then fill uncovered cells */

	/* Track covered base cells as (row, col) set */
	std::set<std::pair<int, int>> covered;

	/* Create cells from spans */
	for (int si = 0; si < (int)layout_.spans.size(); si++) {
		auto &span = layout_.spans[si];

		int r0 = span.row;
		int c0 = span.col;
		int rs = span.rowSpan;
		int cs = span.colSpan;

		/* Safety clamp */
		if (r0 < 0 || c0 < 0 || r0 >= rows || c0 >= cols)
			continue;
		if (r0 + rs > rows)
			rs = rows - r0;
		if (c0 + cs > cols)
			cs = cols - c0;
		if (rs < 1 || cs < 1)
			continue;

		int x = col_x[c0];
		int y = row_y[r0];
		int w = 0;
		int h = 0;

		for (int c = c0; c < c0 + cs; c++)
			w += col_w[c];
		w += gutter * (cs - 1); /* include internal gutters */

		for (int r = r0; r < r0 + rs; r++)
			h += row_h[r];
		h += gutter * (rs - 1);

		CellRect cell;
		cell.x = x;
		cell.y = y;
		cell.w = w;
		cell.h = h;
		cell.gridRow = r0;
		cell.gridCol = c0;
		cell.rowSpan = rs;
		cell.colSpan = cs;
		cell.spanIndex = si;
		cells_.push_back(cell);

		for (int r = r0; r < r0 + rs; r++)
			for (int c = c0; c < c0 + cs; c++)
				covered.insert({r, c});
	}

	/* Fill uncovered cells as 1x1 */
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if (covered.count({r, c}))
				continue;

			CellRect cell;
			cell.x = col_x[c];
			cell.y = row_y[r];
			cell.w = col_w[c];
			cell.h = row_h[r];
			cell.gridRow = r;
			cell.gridCol = c;
			cell.rowSpan = 1;
			cell.colSpan = 1;
			cell.spanIndex = -1;
			cells_.push_back(cell);
		}
	}

	/* Sort cells by row then col for consistent indexing */
	std::sort(cells_.begin(), cells_.end(),
		  [](const CellRect &a, const CellRect &b) {
			  if (a.gridRow != b.gridRow)
				  return a.gridRow < b.gridRow;
			  return a.gridCol < b.gridCol;
		  });
}

/* ---- query ---- */

const std::vector<CellRect> &LayoutEngine::cells() const
{
	return cells_;
}

int LayoutEngine::cell_count() const
{
	return (int)cells_.size();
}

VideoRect LayoutEngine::video_rect(int cellIndex, int srcWidth,
				   int srcHeight) const
{
	if (cellIndex < 0 || cellIndex >= (int)cells_.size() || srcWidth <= 0 ||
	    srcHeight <= 0) {
		return {0, 0, 0, 0};
	}

	const CellRect &cell = cells_[cellIndex];

	double srcAspect = (double)srcWidth / srcHeight;
	double cellAspect = (double)cell.w / cell.h;

	VideoRect vr;

	if (srcAspect > cellAspect) {
		/* Pillarbox: source wider relative to cell → fit width */
		vr.w = cell.w;
		vr.h = (int)std::round((double)cell.w / srcAspect);
		vr.x = cell.x;
		vr.y = cell.y + (cell.h - vr.h) / 2;
	} else {
		/* Letterbox: source taller relative to cell → fit height */
		vr.h = cell.h;
		vr.w = (int)std::round((double)cell.h * srcAspect);
		vr.x = cell.x + (cell.w - vr.w) / 2;
		vr.y = cell.y;
	}

	return vr;
}

std::optional<HitTestResult> LayoutEngine::hit_test(int x, int y) const
{
	/* Check cells first */
	for (int i = 0; i < (int)cells_.size(); i++) {
		const CellRect &c = cells_[i];
		if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) {
			return HitTestResult{HitType::Cell, i};
		}
	}

	/* If within viewport but not in a cell, it's gutter */
	if (x >= 0 && x < vp_width_ && y >= 0 && y < vp_height_) {
		return HitTestResult{HitType::Gutter, -1};
	}

	return std::nullopt;
}

/* ---- span validation ---- */

bool LayoutEngine::is_cell_covered_by_span(int row, int col,
					   int excludeSpanIndex) const
{
	for (int i = 0; i < (int)layout_.spans.size(); i++) {
		if (i == excludeSpanIndex)
			continue;
		auto &s = layout_.spans[i];
		if (row >= s.row && row < s.row + s.rowSpan && col >= s.col &&
		    col < s.col + s.colSpan)
			return true;
	}
	return false;
}

LayoutEngine::SpanError
LayoutEngine::validate_span(const SpanRegion &span) const
{
	return validate_span(span, -1);
}

LayoutEngine::SpanError
LayoutEngine::validate_span(const SpanRegion &span,
			    int excludeSpanIndex) const
{
	if (span.rowSpan < 1 || span.colSpan < 1)
		return SpanError::TooSmall;

	if (span.rowSpan == 1 && span.colSpan == 1)
		return SpanError::TooSmall; /* not a span */

	if (span.row < 0 || span.col < 0 ||
	    span.row + span.rowSpan > layout_.rows ||
	    span.col + span.colSpan > layout_.columns)
		return SpanError::OutOfBounds;

	for (int r = span.row; r < span.row + span.rowSpan; r++) {
		for (int c = span.col; c < span.col + span.colSpan; c++) {
			if (is_cell_covered_by_span(r, c, excludeSpanIndex))
				return SpanError::Overlaps;
		}
	}

	return SpanError::None;
}

LayoutEngine::SpanError
LayoutEngine::validate_all_spans(const LayoutData &layout)
{
	LayoutEngine tmp;
	tmp.set_layout(layout);

	/* Check each span against all others */
	for (int i = 0; i < (int)layout.spans.size(); i++) {
		auto &span = layout.spans[i];

		if (span.rowSpan < 1 || span.colSpan < 1)
			return SpanError::TooSmall;

		if (span.row < 0 || span.col < 0 ||
		    span.row + span.rowSpan > layout.rows ||
		    span.col + span.colSpan > layout.columns)
			return SpanError::OutOfBounds;

		for (int r = span.row; r < span.row + span.rowSpan; r++) {
			for (int c = span.col;
			     c < span.col + span.colSpan; c++) {
				if (tmp.is_cell_covered_by_span(r, c, i))
					return SpanError::Overlaps;
			}
		}
	}

	return SpanError::None;
}
