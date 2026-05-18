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

#include "layout-engine.hpp"

#include <QWidget>

#include <functional>
#include <set>

struct SelectionRect {
	int row;
	int col;
	int rowSpan;
	int colSpan;
};

class GridPreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit GridPreviewWidget(QWidget *parent = nullptr);

	void set_layout(const LayoutData &layout);
	void set_cell_labels(
		const std::vector<std::string> &labels);

	/* Selection */
	void clear_selection();
	const std::set<std::pair<int, int>> &selected_positions() const;

	/* Check if current selection forms a valid rectangle for span merge.
	 * Returns true and fills out if valid; false otherwise. */
	bool selection_is_mergeable(SelectionRect &out) const;

	/* Check if selection overlaps existing spans */
	bool selection_overlaps_span() const;

	/* Get span index at a given cell index (-1 if none) */
	int span_at_cell(int cellIndex) const;

signals:
	void selection_changed();

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void recompute();
	int cell_index_at(int x, int y) const;
	std::pair<int, int> grid_pos_of_cell(int cellIndex) const;

	LayoutEngine engine_;
	LayoutData layout_;
	std::vector<std::string> cell_labels_;

	/* Multi-selection state */
	std::set<std::pair<int, int>> selected_positions_; /* (row, col) */
	std::pair<int, int> shift_anchor_ = {-1, -1};     /* for shift-click range */
};
