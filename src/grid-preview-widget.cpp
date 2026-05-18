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

#include "grid-preview-widget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>

static const QColor COLOR_BG(30, 30, 30);
static const QColor COLOR_CELL(60, 60, 60);
static const QColor COLOR_CELL_SELECTED(80, 120, 200);
static const QColor COLOR_CELL_SPAN(70, 70, 90);
static const QColor COLOR_TEXT(200, 200, 200);
static const QColor COLOR_BORDER(100, 100, 100);

GridPreviewWidget::GridPreviewWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(200, 150);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setMouseTracking(true);
}

void GridPreviewWidget::set_layout(const LayoutData &layout)
{
	layout_ = layout;
	/* Force gutter=0 for seamless preview */
	layout_.gutterPx = 0;
	selected_positions_.clear();
	shift_anchor_ = {-1, -1};
	recompute();
	update();
}

void GridPreviewWidget::set_cell_labels(
	const std::vector<std::string> &labels)
{
	cell_labels_ = labels;
	update();
}

void GridPreviewWidget::clear_selection()
{
	selected_positions_.clear();
	shift_anchor_ = {-1, -1};
	update();
	emit selection_changed();
}

const std::set<std::pair<int, int>> &
GridPreviewWidget::selected_positions() const
{
	return selected_positions_;
}

bool GridPreviewWidget::selection_is_mergeable(SelectionRect &out) const
{
	if (selected_positions_.size() < 2)
		return false;

	/* Find bounding box */
	int minR = INT_MAX, maxR = INT_MIN;
	int minC = INT_MAX, maxC = INT_MIN;
	for (auto &[r, c] : selected_positions_) {
		minR = std::min(minR, r);
		maxR = std::max(maxR, r);
		minC = std::min(minC, c);
		maxC = std::max(maxC, c);
	}

	int rows = maxR - minR + 1;
	int cols = maxC - minC + 1;

	/* Check that selection fills the entire bounding rectangle */
	if ((int)selected_positions_.size() != rows * cols)
		return false;

	/* Verify every cell in the rectangle is selected */
	for (int r = minR; r <= maxR; r++) {
		for (int c = minC; c <= maxC; c++) {
			if (selected_positions_.count({r, c}) == 0)
				return false;
		}
	}

	out.row = minR;
	out.col = minC;
	out.rowSpan = rows;
	out.colSpan = cols;
	return true;
}

bool GridPreviewWidget::selection_overlaps_span() const
{
	for (auto &[r, c] : selected_positions_) {
		for (auto &span : layout_.spans) {
			if (r >= span.row && r < span.row + span.rowSpan &&
			    c >= span.col && c < span.col + span.colSpan)
				return true;
		}
	}
	return false;
}

int GridPreviewWidget::span_at_cell(int cellIndex) const
{
	const auto &cells = engine_.cells();
	if (cellIndex < 0 || cellIndex >= (int)cells.size())
		return -1;
	return cells[cellIndex].spanIndex;
}

void GridPreviewWidget::recompute()
{
	engine_.set_layout(layout_);
	engine_.set_viewport(width(), height());
	engine_.compute();
}

void GridPreviewWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	recompute();
}

int GridPreviewWidget::cell_index_at(int x, int y) const
{
	auto result = engine_.hit_test(x, y);
	if (result && result->type == HitType::Cell)
		return result->cellIndex;
	return -1;
}

std::pair<int, int> GridPreviewWidget::grid_pos_of_cell(int cellIndex) const
{
	const auto &cells = engine_.cells();
	if (cellIndex < 0 || cellIndex >= (int)cells.size())
		return {-1, -1};
	return {cells[cellIndex].gridRow, cells[cellIndex].gridCol};
}

void GridPreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, false);

	/* Background */
	painter.fillRect(rect(), COLOR_BG);

	const auto &cells = engine_.cells();

	for (int i = 0; i < (int)cells.size(); i++) {
		const CellRect &c = cells[i];
		QRect r(c.x, c.y, c.w, c.h);

		/* Determine if this cell is selected */
		bool is_selected = false;
		if (c.rowSpan == 1 && c.colSpan == 1) {
			is_selected = selected_positions_.count(
					      {c.gridRow, c.gridCol}) > 0;
		} else {
			/* For span cells, check if any of its positions are selected */
			for (int sr = c.gridRow;
			     sr < c.gridRow + c.rowSpan; sr++) {
				for (int sc = c.gridCol;
				     sc < c.gridCol + c.colSpan; sc++) {
					if (selected_positions_.count(
						    {sr, sc}) > 0) {
						is_selected = true;
						break;
					}
				}
				if (is_selected)
					break;
			}
		}

		/* Fill */
		QColor fill;
		if (is_selected)
			fill = COLOR_CELL_SELECTED;
		else if (c.spanIndex >= 0)
			fill = COLOR_CELL_SPAN;
		else
			fill = COLOR_CELL;

		painter.fillRect(r, fill);

		/* Border */
		painter.setPen(COLOR_BORDER);
		painter.drawRect(r);

		/* Label */
		QString label;
		if (i < (int)cell_labels_.size() &&
		    !cell_labels_[i].empty()) {
			label = QString::fromStdString(cell_labels_[i]);
		} else {
			if (c.rowSpan > 1 || c.colSpan > 1) {
				label = QStringLiteral("%1,%2 [%3x%4]")
						.arg(c.gridRow)
						.arg(c.gridCol)
						.arg(c.rowSpan)
						.arg(c.colSpan);
			} else {
				label = QStringLiteral("%1,%2")
						.arg(c.gridRow)
						.arg(c.gridCol);
			}
		}

		painter.setPen(COLOR_TEXT);
		painter.drawText(r, Qt::AlignCenter, label);
	}
}

void GridPreviewWidget::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	int mx = (int)event->position().x();
	int my = (int)event->position().y();

	int cellIdx = cell_index_at(mx, my);
	if (cellIdx < 0) {
		/* Clicked empty area - clear selection */
		clear_selection();
		return;
	}

	auto [row, col] = grid_pos_of_cell(cellIdx);
	if (row < 0)
		return;

	bool ctrl = event->modifiers() & Qt::ControlModifier;
	bool shift = event->modifiers() & Qt::ShiftModifier;

	if (shift && shift_anchor_.first >= 0) {
		/* Shift+Click: range select from anchor to this cell */
		int r0 = std::min(shift_anchor_.first, row);
		int r1 = std::max(shift_anchor_.first, row);
		int c0 = std::min(shift_anchor_.second, col);
		int c1 = std::max(shift_anchor_.second, col);

		selected_positions_.clear();
		for (int r = r0; r <= r1; r++) {
			for (int c = c0; c <= c1; c++) {
				selected_positions_.insert({r, c});
			}
		}
	} else if (ctrl) {
		/* Ctrl+Click: toggle individual cell in selection */
		shift_anchor_ = {row, col};
		auto pos = std::make_pair(row, col);
		if (selected_positions_.count(pos))
			selected_positions_.erase(pos);
		else
			selected_positions_.insert(pos);
	} else {
		/* Plain click: single select, set anchor for future shift-click */
		shift_anchor_ = {row, col};
		selected_positions_.clear();
		selected_positions_.insert({row, col});
	}

	update();
	emit selection_changed();
}
