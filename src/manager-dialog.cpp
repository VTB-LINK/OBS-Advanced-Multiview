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

#include "manager-dialog.hpp"
#include "grid-preview-widget.hpp"
#include "multiview-window.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <set>

/* Load an icon from the active OBS theme directory.
 * Detects light/dark via palette, then searches for the
 * matching theme folder next to the OBS executable. */
static QIcon obs_theme_icon(const char *name)
{
	/* Determine if we're in a light or dark theme via text luminance */
	QColor textColor = QApplication::palette().color(QPalette::WindowText);
	bool isDark = (textColor.lightnessF() > 0.5);

	QString app_dir = QApplication::applicationDirPath();
	QDir dir(app_dir);
	dir.cdUp(); /* 64bit -> bin */
	dir.cdUp(); /* bin -> root */

	/* Preferred theme order based on palette */
	QStringList themes;
	if (isDark)
		themes = {"Dark", "Yami", "Rachni", "Acri", "Light"};
	else
		themes = {"Light", "Acri", "Rachni", "Yami", "Dark"};

	for (auto &t : themes) {
		QString path = dir.absoluteFilePath(
			QStringLiteral("data/obs-studio/themes/%1/%2.svg").arg(t, QString::fromUtf8(name)));
		if (QFile::exists(path))
			return QIcon(path);
	}
	return QIcon();
}

/* Generate a northeast-arrow (↗) icon matching current theme color */
static QIcon make_open_icon()
{
	QColor c = QApplication::palette().color(QPalette::WindowText);
	QString color = c.name(); /* e.g. "#fefefe" or "#202020" */

	/* Simple SVG: arrow pointing to top-right with a short line */
	QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' "
				     "viewBox='0 0 16 16' width='16' height='16'>"
				     "<path d='M4 12 L12 4 M12 4 L6 4 M12 4 L12 10' "
				     "fill='none' stroke='%1' stroke-width='2' "
				     "stroke-linecap='round' stroke-linejoin='round'/>"
				     "</svg>")
			      .arg(color);

	QPixmap pix(16, 16);
	pix.fill(Qt::transparent);
	QSvgRenderer renderer(svg.toUtf8());
	QPainter painter(&pix);
	renderer.render(&painter);
	return QIcon(pix);
}

ManagerDialog::ManagerDialog(ConfigManager *config, QWidget *parent) : QDialog(parent), config_(config)
{
	setWindowTitle(QStringLiteral("OBS Advanced Multiview"));
	setMinimumSize(800, 500);
	setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);

	setup_ui();
	refresh_instance_list();
	update_button_states();
}

ManagerDialog::~ManagerDialog() = default;

bool ManagerDialog::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == detail_name_edit_) {
		if (event->type() == QEvent::FocusIn) {
			name_edit_original_ = detail_name_edit_->text();
		} else if (event->type() == QEvent::FocusOut) {
			/* Cancel: revert to original */
			detail_name_edit_->setText(name_edit_original_);
		} else if (event->type() == QEvent::KeyPress) {
			auto *ke = static_cast<QKeyEvent *>(event);
			if (ke->key() == Qt::Key_Escape) {
				detail_name_edit_->setText(name_edit_original_);
				detail_name_edit_->clearFocus();
				return true; /* eat the event so dialog doesn't close */
			}
		}
	}
	return QDialog::eventFilter(obj, event);
}

/* ---- UI setup ---- */

void ManagerDialog::setup_ui()
{
	auto *main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(0, 0, 0, 0);

	tab_widget_ = new QTabWidget(this);

	auto *instances_tab = new QWidget();
	setup_instances_tab(instances_tab);
	tab_widget_->addTab(instances_tab, QStringLiteral("Instances"));

	auto *settings_tab = new QWidget();
	setup_settings_tab(settings_tab);
	tab_widget_->addTab(settings_tab, QStringLiteral("Settings"));

	main_layout->addWidget(tab_widget_);
}

void ManagerDialog::setup_instances_tab(QWidget *tab)
{
	auto *layout = new QHBoxLayout(tab);
	layout->setContentsMargins(0, 0, 0, 0);

	splitter_ = new QSplitter(Qt::Horizontal, tab);
	splitter_->setChildrenCollapsible(false);

	auto *left_panel = new QWidget(tab);
	setup_left_panel(left_panel);

	auto *right_panel = new QWidget(tab);
	setup_right_panel(right_panel);

	splitter_->addWidget(left_panel);
	splitter_->addWidget(right_panel);
	splitter_->setStretchFactor(0, 0);
	splitter_->setStretchFactor(1, 1);
	splitter_->setSizes({200, 600});

	layout->addWidget(splitter_);
}

void ManagerDialog::setup_left_panel(QWidget *panel)
{
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(4, 4, 0, 4);

	instance_tree_ = new QTreeWidget(panel);
	instance_tree_->setHeaderHidden(true);
	instance_tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	instance_tree_->setContextMenuPolicy(Qt::CustomContextMenu);
	instance_tree_->setDragDropMode(QAbstractItemView::InternalMove);
	instance_tree_->setRootIsDecorated(false);
	instance_tree_->setIndentation(16);
	instance_tree_->setIconSize(QSize(0, 0));

	/* Style: remove leading space for leaf items, show arrows only
	 * on items that have children (folders). */
	{
		QString app_dir = QApplication::applicationDirPath();
		QDir d(app_dir);
		d.cdUp();
		d.cdUp();
		QColor tc = QApplication::palette().color(QPalette::WindowText);
		QString theme = (tc.lightnessF() > 0.5) ? "Dark" : "Light";
		QString closed_arrow =
			d.absoluteFilePath(QStringLiteral("data/obs-studio/themes/%1/expand.svg").arg(theme));
		QString open_arrow =
			d.absoluteFilePath(QStringLiteral("data/obs-studio/themes/%1/collapse.svg").arg(theme));

		instance_tree_->setStyleSheet(QStringLiteral("QTreeWidget::item { padding: 2px 0px; }"
							     "QTreeWidget::branch:has-children:!has-siblings:closed,"
							     "QTreeWidget::branch:closed:has-children:has-siblings {"
							     "  border-image: none;"
							     "  image: url(%1);"
							     "}"
							     "QTreeWidget::branch:open:has-children:!has-siblings,"
							     "QTreeWidget::branch:open:has-children:has-siblings {"
							     "  border-image: none;"
							     "  image: url(%2);"
							     "}"
							     "QTreeWidget::branch:!has-children {"
							     "  border-image: none;"
							     "  image: none;"
							     "}")
						      .arg(closed_arrow, open_arrow));
	}
	layout->addWidget(instance_tree_);

	connect(instance_tree_, &QTreeWidget::itemSelectionChanged, this,
		&ManagerDialog::on_instance_selection_changed);
	connect(instance_tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
		if (!item || is_folder_item(item))
			return;
		on_open_instance();
	});
	connect(instance_tree_, &QTreeWidget::customContextMenuRequested, this, &ManagerDialog::show_context_menu);

	/* Bottom toolbar - OBS style icon buttons */
	auto *toolbar = new QHBoxLayout();
	toolbar->setSpacing(2);

	const int btn_sz = 24;

	btn_new_ = new QPushButton(panel);
	btn_new_->setIcon(obs_theme_icon("plus"));
	btn_new_->setFixedSize(btn_sz, btn_sz);
	btn_new_->setFlat(true);
	btn_new_->setToolTip(QStringLiteral("New Instance"));
	toolbar->addWidget(btn_new_);

	btn_delete_ = new QPushButton(panel);
	btn_delete_->setIcon(obs_theme_icon("trash"));
	btn_delete_->setFixedSize(btn_sz, btn_sz);
	btn_delete_->setFlat(true);
	btn_delete_->setToolTip(QStringLiteral("Delete"));
	toolbar->addWidget(btn_delete_);

	btn_clone_ = new QPushButton(panel);
	btn_clone_->setIcon(obs_theme_icon("popout"));
	btn_clone_->setFixedSize(btn_sz, btn_sz);
	btn_clone_->setFlat(true);
	btn_clone_->setToolTip(QStringLiteral("Clone Instance"));
	toolbar->addWidget(btn_clone_);

	btn_open_ = new QPushButton(panel);
	btn_open_->setIcon(make_open_icon());
	btn_open_->setFixedSize(btn_sz, btn_sz);
	btn_open_->setFlat(true);
	btn_open_->setToolTip(QStringLiteral("Open Multiview Window"));
	toolbar->addWidget(btn_open_);

	toolbar->addStretch();

	btn_move_up_ = new QPushButton(panel);
	btn_move_up_->setIcon(obs_theme_icon("up"));
	btn_move_up_->setFixedSize(btn_sz, btn_sz);
	btn_move_up_->setFlat(true);
	btn_move_up_->setToolTip(QStringLiteral("Reorder is not implemented yet"));
	btn_move_up_->setEnabled(false);
	toolbar->addWidget(btn_move_up_);

	btn_move_down_ = new QPushButton(panel);
	btn_move_down_->setIcon(obs_theme_icon("down"));
	btn_move_down_->setFixedSize(btn_sz, btn_sz);
	btn_move_down_->setFlat(true);
	btn_move_down_->setToolTip(QStringLiteral("Reorder is not implemented yet"));
	btn_move_down_->setEnabled(false);
	toolbar->addWidget(btn_move_down_);

	layout->addLayout(toolbar);

	connect(btn_new_, &QPushButton::clicked, this, &ManagerDialog::on_new_instance);
	connect(btn_clone_, &QPushButton::clicked, this, &ManagerDialog::on_clone_instance);
	connect(btn_delete_, &QPushButton::clicked, this, &ManagerDialog::on_delete_instance);
	connect(btn_open_, &QPushButton::clicked, this, &ManagerDialog::on_open_instance);
	connect(btn_move_up_, &QPushButton::clicked, this, [this]() {
		/* TODO: reorder support */
	});
	connect(btn_move_down_, &QPushButton::clicked, this, [this]() {
		/* TODO: reorder support */
	});
}

void ManagerDialog::setup_right_panel(QWidget *panel)
{
	auto *layout = new QVBoxLayout(panel);

	right_stack_ = new QStackedWidget(panel);

	/* Page 0: empty placeholder */
	page_empty_ = new QWidget();
	auto *empty_layout = new QVBoxLayout(page_empty_);
	auto *empty_label = new QLabel(QStringLiteral("Select an instance or create a new one"), page_empty_);
	empty_label->setAlignment(Qt::AlignCenter);
	empty_layout->addWidget(empty_label);
	right_stack_->addWidget(page_empty_);

	/* Page 1: unified instance page (info + gutter + grid editor) */
	page_instance_ = new QWidget();
	auto *p_layout = new QVBoxLayout(page_instance_);
	p_layout->setContentsMargins(8, 8, 8, 8);

	/* --- Top row: name (editable) + action buttons right-aligned --- */
	auto *top_row = new QHBoxLayout();
	detail_name_edit_ = new QLineEdit(page_instance_);
	detail_name_edit_->setStyleSheet(QStringLiteral("QLineEdit { font-size: 14px; font-weight: bold; "
							"border: none; background: transparent; padding: 2px; }"
							"QLineEdit:focus { border: 1px solid palette(highlight); "
							"background: palette(base); }"));
	detail_name_edit_->installEventFilter(this);
	top_row->addWidget(detail_name_edit_, 1);

	btn_detail_open_ = new QPushButton(page_instance_);
	btn_detail_open_->setIcon(make_open_icon());
	btn_detail_open_->setFixedSize(24, 24);
	btn_detail_open_->setFlat(true);
	btn_detail_open_->setToolTip(QStringLiteral("Open"));
	top_row->addWidget(btn_detail_open_);

	btn_detail_delete_ = new QPushButton(page_instance_);
	btn_detail_delete_->setIcon(obs_theme_icon("trash"));
	btn_detail_delete_->setFixedSize(24, 24);
	btn_detail_delete_->setFlat(true);
	btn_detail_delete_->setToolTip(QStringLiteral("Delete"));
	top_row->addWidget(btn_detail_delete_);

	btn_detail_clone_ = new QPushButton(page_instance_);
	btn_detail_clone_->setIcon(obs_theme_icon("popout"));
	btn_detail_clone_->setFixedSize(24, 24);
	btn_detail_clone_->setFlat(true);
	btn_detail_clone_->setToolTip(QStringLiteral("Clone"));
	top_row->addWidget(btn_detail_clone_);

	p_layout->addLayout(top_row);

	/* UUID label */
	detail_uuid_label_ = new QLabel(page_instance_);
	detail_uuid_label_->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
	p_layout->addWidget(detail_uuid_label_);

	/* --- Separator --- */
	auto *sep1 = new QFrame(page_instance_);
	sep1->setFrameShape(QFrame::HLine);
	sep1->setFrameShadow(QFrame::Sunken);
	p_layout->addWidget(sep1);

	/* --- Gutter settings (single row) --- */
	auto *gutter_row = new QHBoxLayout();
	detail_use_global_gutter_ = new QCheckBox(QStringLiteral("Inherit gutter from Settings"), page_instance_);
	gutter_row->addWidget(detail_use_global_gutter_);
	gutter_row->addStretch();
	gutter_row->addWidget(new QLabel(QStringLiteral("Gutter (px):"), page_instance_));
	detail_gutter_spin_ = new QSpinBox(page_instance_);
	detail_gutter_spin_->setRange(0, 50);
	detail_gutter_spin_->setMinimumWidth(60);
	gutter_row->addWidget(detail_gutter_spin_);
	detail_gutter_effective_ = new QLabel(page_instance_);
	detail_gutter_effective_->setStyleSheet(QStringLiteral("color: gray;"));
	gutter_row->addWidget(detail_gutter_effective_);
	p_layout->addLayout(gutter_row);

	/* --- Separator --- */
	auto *sep2 = new QFrame(page_instance_);
	sep2->setFrameShape(QFrame::HLine);
	sep2->setFrameShadow(QFrame::Sunken);
	p_layout->addWidget(sep2);

	/* --- Grid controls row --- */
	auto *grid_ctrl_row = new QHBoxLayout();
	grid_ctrl_row->addWidget(new QLabel(QStringLiteral("Rows:"), page_instance_));
	grid_rows_spin_ = new QSpinBox(page_instance_);
	grid_rows_spin_->setRange(1, 10);
	grid_rows_spin_->setMinimumWidth(60);
	grid_ctrl_row->addWidget(grid_rows_spin_);

	grid_ctrl_row->addWidget(new QLabel(QStringLiteral("Cols:"), page_instance_));
	grid_cols_spin_ = new QSpinBox(page_instance_);
	grid_cols_spin_->setRange(1, 10);
	grid_cols_spin_->setMinimumWidth(60);
	grid_ctrl_row->addWidget(grid_cols_spin_);

	grid_ctrl_row->addStretch();

	btn_add_span_ = new QPushButton(QStringLiteral("Merge"), page_instance_);
	btn_add_span_->setEnabled(false);
	grid_ctrl_row->addWidget(btn_add_span_);

	btn_remove_span_ = new QPushButton(QStringLiteral("Unmerge"), page_instance_);
	btn_remove_span_->setEnabled(false);
	grid_ctrl_row->addWidget(btn_remove_span_);

	p_layout->addLayout(grid_ctrl_row);

	/* Span info row: label left, Reset All button right */
	auto *span_row = new QHBoxLayout();
	grid_span_info_ = new QLabel(page_instance_);
	grid_span_info_->setStyleSheet(QStringLiteral("color: gray;"));
	span_row->addWidget(grid_span_info_, 1);
	btn_reset_all_ = new QPushButton(QStringLiteral("Reset All"), page_instance_);
	btn_reset_all_->setFixedWidth(btn_remove_span_->sizeHint().width());
	span_row->addWidget(btn_reset_all_);
	p_layout->addLayout(span_row);

	/* --- Grid preview (fills remaining space) --- */
	grid_preview_ = new GridPreviewWidget(page_instance_);
	p_layout->addWidget(grid_preview_, 1);

	right_stack_->addWidget(page_instance_);

	/* ---- Connections ---- */

	/* Name edit: Enter applies, Esc/focus-out cancels (see eventFilter) */
	connect(detail_name_edit_, &QLineEdit::returnPressed, this, [this]() {
		if (current_detail_uuid_.empty())
			return;
		QString newName = detail_name_edit_->text().trimmed();
		if (newName.isEmpty()) {
			detail_name_edit_->setText(name_edit_original_);
			detail_name_edit_->clearFocus();
			return;
		}
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst || QString::fromStdString(inst->name) == newName) {
			detail_name_edit_->clearFocus();
			return;
		}
		name_edit_original_ = newName; /* update so focus-out won't revert */
		config_->rename_instance(current_detail_uuid_, newName.toStdString());
		config_->save();
		refresh_instance_list();
		select_instance_by_uuid(current_detail_uuid_);
		notify_multiview_name_changed(current_detail_uuid_);
		detail_name_edit_->clearFocus();
	});

	/* Action buttons */
	connect(btn_detail_open_, &QPushButton::clicked, this, &ManagerDialog::on_open_instance);
	connect(btn_detail_delete_, &QPushButton::clicked, this, &ManagerDialog::on_delete_instance);
	connect(btn_detail_clone_, &QPushButton::clicked, this, &ManagerDialog::on_clone_instance);

	/* Gutter */
	connect(detail_use_global_gutter_, &QCheckBox::toggled, this, [this](bool checked) {
		detail_gutter_spin_->setEnabled(!checked);
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst)
			return;
		inst->useGlobalGutter = checked;
		if (!checked)
			inst->layout.gutterPx = detail_gutter_spin_->value();
		int eff = inst->effective_gutter(config_->global_settings().defaultGutterPx);
		detail_gutter_effective_->setText(QStringLiteral("Effective: %1px").arg(eff));
		config_->save();
		notify_multiview_layout_changed(current_detail_uuid_);
	});

	connect(detail_gutter_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst || inst->useGlobalGutter)
			return;
		inst->layout.gutterPx = value;
		detail_gutter_effective_->setText(QStringLiteral("Effective: %1px").arg(value));
		config_->save();
		notify_multiview_layout_changed(current_detail_uuid_);
	});

	/* Grid rows/cols - auto save */
	connect(grid_rows_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val) {
		grid_edit_layout_.rows = val;
		auto &spans = grid_edit_layout_.spans;
		spans.erase(std::remove_if(spans.begin(), spans.end(),
					   [&](const SpanRegion &s) { return s.row + s.rowSpan > val; }),
			    spans.end());
		update_grid_preview();
		auto_save_layout();
	});

	connect(grid_cols_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val) {
		grid_edit_layout_.columns = val;
		auto &spans = grid_edit_layout_.spans;
		spans.erase(std::remove_if(spans.begin(), spans.end(),
					   [&](const SpanRegion &s) { return s.col + s.colSpan > val; }),
			    spans.end());
		update_grid_preview();
		auto_save_layout();
	});

	/* Span controls */
	connect(btn_add_span_, &QPushButton::clicked, this, [this]() {
		SelectionRect sr;
		if (!grid_preview_->selection_is_mergeable(sr))
			return;
		if (grid_preview_->selection_overlaps_span())
			return;

		SpanRegion newSpan{sr.row, sr.col, sr.rowSpan, sr.colSpan};
		LayoutEngine validator;
		validator.set_layout(grid_edit_layout_);
		if (validator.validate_span(newSpan) != LayoutEngine::SpanError::None)
			return;

		grid_edit_layout_.spans.push_back(newSpan);
		grid_preview_->clear_selection();
		update_grid_preview();
		auto_save_layout();
	});

	connect(btn_remove_span_, &QPushButton::clicked, this, [this]() {
		auto &sel = grid_preview_->selected_positions();
		if (sel.empty())
			return;

		std::set<int> span_indices;
		for (auto &[r, c] : sel) {
			for (int i = 0; i < (int)grid_edit_layout_.spans.size(); i++) {
				auto &s = grid_edit_layout_.spans[i];
				if (r >= s.row && r < s.row + s.rowSpan && c >= s.col && c < s.col + s.colSpan)
					span_indices.insert(i);
			}
		}
		if (span_indices.empty())
			return;

		for (auto it = span_indices.rbegin(); it != span_indices.rend(); ++it)
			grid_edit_layout_.spans.erase(grid_edit_layout_.spans.begin() + *it);

		grid_preview_->clear_selection();
		update_grid_preview();
		auto_save_layout();
	});

	connect(btn_reset_all_, &QPushButton::clicked, this, [this]() {
		if (grid_edit_layout_.spans.empty())
			return;
		grid_edit_layout_.spans.clear();
		grid_preview_->clear_selection();
		update_grid_preview();
		auto_save_layout();
	});

	/* Grid selection feedback */
	connect(grid_preview_, &GridPreviewWidget::selection_changed, this, [this]() {
		auto &sel = grid_preview_->selected_positions();
		if (sel.empty()) {
			grid_span_info_->setText(QString());
			btn_add_span_->setEnabled(false);
			btn_remove_span_->setEnabled(false);
			return;
		}

		bool has_span = grid_preview_->selection_overlaps_span();
		SelectionRect sr;
		bool mergeable = grid_preview_->selection_is_mergeable(sr);

		btn_add_span_->setEnabled(mergeable && !has_span);
		btn_remove_span_->setEnabled(has_span);

		if (sel.size() == 1) {
			auto [r, c] = *sel.begin();
			grid_span_info_->setText(has_span ? QStringLiteral("Span cell at %1,%2").arg(r).arg(c)
							  : QStringLiteral("Cell %1,%2").arg(r).arg(c));
		} else if (mergeable && !has_span) {
			grid_span_info_->setText(QStringLiteral("%1x%2 at %3,%4 - ready to merge")
							 .arg(sr.rowSpan)
							 .arg(sr.colSpan)
							 .arg(sr.row)
							 .arg(sr.col));
		} else if (!mergeable) {
			grid_span_info_->setText(QStringLiteral("Not a valid rectangle"));
		} else {
			grid_span_info_->setText(QStringLiteral("Overlaps existing span"));
		}
	});

	layout->addWidget(right_stack_);
}

void ManagerDialog::setup_settings_tab(QWidget *tab)
{
	auto *layout = new QVBoxLayout(tab);
	layout->setContentsMargins(12, 12, 12, 12);

	auto *gutter_row = new QHBoxLayout();
	gutter_row->addWidget(new QLabel(QStringLiteral("Default Gutter (px):"), tab));
	spin_default_gutter_ = new QSpinBox(tab);
	spin_default_gutter_->setRange(0, 50);
	spin_default_gutter_->setValue(config_->global_settings().defaultGutterPx);
	gutter_row->addWidget(spin_default_gutter_);
	gutter_row->addStretch();
	layout->addLayout(gutter_row);

	auto *gs_apply = new QPushButton(QStringLiteral("Apply"), tab);
	layout->addWidget(gs_apply);
	layout->addStretch();

	connect(gs_apply, &QPushButton::clicked, this, [this]() {
		config_->global_settings().defaultGutterPx = spin_default_gutter_->value();
		config_->save();
		obs_log(LOG_INFO, "global settings saved (gutter=%d)", spin_default_gutter_->value());
		notify_multiview_layout_changed();
	});
}

/* ---- instance list ---- */

void ManagerDialog::refresh_instance_list()
{
	instance_tree_->clear();

	for (auto &inst : config_->instances()) {
		QTreeWidgetItem *parent = nullptr;
		if (!inst.folder.empty())
			parent = find_or_create_folder_item(inst.folder);

		auto *item = new QTreeWidgetItem();
		item->setText(0, QString::fromStdString(inst.name));
		item->setData(0, Qt::UserRole, QString::fromStdString(inst.uuid));

		if (parent)
			parent->addChild(item);
		else
			instance_tree_->addTopLevelItem(item);
	}

	instance_tree_->expandAll();
	update_button_states();
}

void ManagerDialog::select_instance_by_uuid(const std::string &uuid)
{
	if (uuid.empty())
		return;
	QString target = QString::fromStdString(uuid);
	std::function<QTreeWidgetItem *(QTreeWidgetItem *)> find_item;
	find_item = [&](QTreeWidgetItem *parent) -> QTreeWidgetItem * {
		int count = parent ? parent->childCount() : instance_tree_->topLevelItemCount();
		for (int i = 0; i < count; i++) {
			auto *item = parent ? parent->child(i) : instance_tree_->topLevelItem(i);
			if (item->data(0, Qt::UserRole).toString() == target)
				return item;
			if (auto *found = find_item(item))
				return found;
		}
		return nullptr;
	};
	if (auto *item = find_item(nullptr)) {
		instance_tree_->blockSignals(true);
		instance_tree_->setCurrentItem(item);
		instance_tree_->blockSignals(false);
	}
}

QTreeWidgetItem *ManagerDialog::find_or_create_folder_item(const std::string &folder)
{
	/* Check existing top-level items */
	for (int i = 0; i < instance_tree_->topLevelItemCount(); i++) {
		auto *item = instance_tree_->topLevelItem(i);
		if (is_folder_item(item) && item->text(0).toStdString() == folder)
			return item;
	}

	/* Create new folder item */
	auto *item = new QTreeWidgetItem();
	item->setText(0, QString::fromStdString(folder));
	item->setData(0, Qt::UserRole, QStringLiteral("__folder__"));
	item->setFlags(item->flags() | Qt::ItemIsDropEnabled);
	QFont f = item->font(0);
	f.setBold(true);
	item->setFont(0, f);
	instance_tree_->addTopLevelItem(item);
	return item;
}

std::string ManagerDialog::get_item_uuid(QTreeWidgetItem *item) const
{
	if (!item)
		return {};
	return item->data(0, Qt::UserRole).toString().toStdString();
}

bool ManagerDialog::is_folder_item(QTreeWidgetItem *item) const
{
	if (!item)
		return false;
	return item->data(0, Qt::UserRole).toString() == QStringLiteral("__folder__");
}

void ManagerDialog::update_button_states()
{
	auto selected = instance_tree_->selectedItems();

	/* Filter to non-folder items */
	bool has_instance = false;
	for (auto *item : selected) {
		if (!is_folder_item(item)) {
			has_instance = true;
			break;
		}
	}

	btn_clone_->setEnabled(has_instance && selected.size() == 1);
	btn_delete_->setEnabled(!selected.isEmpty());
	btn_open_->setEnabled(has_instance);
	/* Move Up/Down kept disabled until reorder is implemented */
}

/* ---- slots ---- */

void ManagerDialog::on_instance_selection_changed()
{
	update_button_states();
	auto selected = instance_tree_->selectedItems();
	if (selected.size() != 1) {
		right_stack_->setCurrentIndex(PAGE_EMPTY);
		return;
	}
	auto *item = selected.first();
	if (is_folder_item(item)) {
		right_stack_->setCurrentIndex(PAGE_EMPTY);
		return;
	}
	std::string uuid = get_item_uuid(item);
	if (!uuid.empty())
		show_instance_detail(uuid);
}

void ManagerDialog::on_new_instance()
{
	bool ok;
	QString name = QInputDialog::getText(this, QStringLiteral("New Instance"), QStringLiteral("Instance name:"),
					     QLineEdit::Normal, QString(), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	MultiviewInstance *inst = config_->add_instance(name.trimmed().toStdString());
	(void)inst;
	config_->save();
	refresh_instance_list();

	/* Select the new item (last instance at root) */
	int count = instance_tree_->topLevelItemCount();
	for (int i = count - 1; i >= 0; i--) {
		auto *item = instance_tree_->topLevelItem(i);
		if (!is_folder_item(item)) {
			instance_tree_->setCurrentItem(item);
			break;
		}
	}
}

void ManagerDialog::on_rename_instance()
{
	auto selected = instance_tree_->selectedItems();
	if (selected.size() != 1)
		return;
	auto *item = selected.first();
	if (is_folder_item(item))
		return;

	std::string uuid = get_item_uuid(item);

	bool ok;
	QString name = QInputDialog::getText(this, QStringLiteral("Rename Instance"), QStringLiteral("New name:"),
					     QLineEdit::Normal, item->text(0), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	config_->rename_instance(uuid, name.trimmed().toStdString());
	config_->save();
	refresh_instance_list();
	select_instance_by_uuid(uuid);
	notify_multiview_name_changed(uuid);
}

void ManagerDialog::on_clone_instance()
{
	auto selected = instance_tree_->selectedItems();
	if (selected.size() != 1)
		return;
	auto *item = selected.first();
	if (is_folder_item(item))
		return;

	std::string uuid = get_item_uuid(item);

	bool ok;
	QString name = QInputDialog::getText(this, QStringLiteral("Clone Instance"),
					     QStringLiteral("Name for cloned instance:"), QLineEdit::Normal,
					     item->text(0) + QStringLiteral(" (Copy)"), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	config_->clone_instance(uuid, name.trimmed().toStdString());
	config_->save();
	refresh_instance_list();
}

void ManagerDialog::on_delete_instance()
{
	auto selected = instance_tree_->selectedItems();
	if (selected.isEmpty())
		return;

	/* Collect UUIDs of instances to delete */
	QStringList names;
	std::vector<std::string> uuids;
	bool deleting_folders = false;

	for (auto *item : selected) {
		if (is_folder_item(item)) {
			deleting_folders = true;
			/* Delete all instances inside the folder */
			for (int i = 0; i < item->childCount(); i++) {
				auto *child = item->child(i);
				uuids.push_back(get_item_uuid(child));
				names.append(child->text(0));
			}
		} else {
			uuids.push_back(get_item_uuid(item));
			names.append(item->text(0));
		}
	}

	/* Build confirmation message */
	QString msg;
	if (uuids.empty() && deleting_folders) {
		msg = QStringLiteral("Delete empty folder(s)?");
	} else if (uuids.size() == 1) {
		msg = QStringLiteral("Delete instance \"%1\"?").arg(names.first());
	} else if (!uuids.empty()) {
		msg = QStringLiteral("Delete %1 instance(s)?").arg(uuids.size());
	} else {
		return;
	}

	auto ret = QMessageBox::question(this, QStringLiteral("Delete"), msg, QMessageBox::Yes | QMessageBox::No);
	if (ret != QMessageBox::Yes)
		return;

	for (auto &uuid : uuids)
		config_->delete_instance(uuid);

	/* Remove folder tags for deleted folders (clear folder field
	 * for any remaining instances that referenced them) */
	if (deleting_folders) {
		for (auto *item : selected) {
			if (!is_folder_item(item))
				continue;
			std::string folder_name = item->text(0).toStdString();
			for (auto &inst : const_cast<std::vector<MultiviewInstance> &>(config_->instances())) {
				if (inst.folder == folder_name)
					inst.folder.clear();
			}
		}
	}

	config_->save();
	refresh_instance_list();
	right_stack_->setCurrentIndex(PAGE_EMPTY);
}

void ManagerDialog::on_open_instance()
{
	auto selected = instance_tree_->selectedItems();
	for (auto *item : selected) {
		if (is_folder_item(item))
			continue;
		std::string uuid = get_item_uuid(item);
		if (!uuid.empty())
			open_multiview_window(uuid);
	}
}

void ManagerDialog::on_new_folder()
{
	bool ok;
	QString name = QInputDialog::getText(this, QStringLiteral("New Folder"), QStringLiteral("Folder name:"),
					     QLineEdit::Normal, QString(), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	/* Just create the folder node - it persists only if instances use it */
	find_or_create_folder_item(name.trimmed().toStdString());
}

void ManagerDialog::on_move_to_folder()
{
	auto selected = instance_tree_->selectedItems();
	if (selected.isEmpty())
		return;

	/* Gather existing folder names */
	QStringList folders;
	folders.append(QStringLiteral("(Root - No Folder)"));
	for (int i = 0; i < instance_tree_->topLevelItemCount(); i++) {
		auto *item = instance_tree_->topLevelItem(i);
		if (is_folder_item(item))
			folders.append(item->text(0));
	}

	bool ok;
	QString chosen = QInputDialog::getItem(this, QStringLiteral("Move to Folder"), QStringLiteral("Select folder:"),
					       folders, 0, true, &ok);
	if (!ok)
		return;

	std::string target_folder;
	if (chosen != QStringLiteral("(Root - No Folder)"))
		target_folder = chosen.toStdString();

	for (auto *item : selected) {
		if (is_folder_item(item))
			continue;
		std::string uuid = get_item_uuid(item);
		MultiviewInstance *inst = config_->find_instance(uuid);
		if (inst)
			inst->folder = target_folder;
	}
	config_->save();
	refresh_instance_list();
}

void ManagerDialog::on_rename_folder(QTreeWidgetItem *folder_item)
{
	if (!folder_item || !is_folder_item(folder_item))
		return;

	std::string old_name = folder_item->text(0).toStdString();
	bool ok;
	QString new_name = QInputDialog::getText(this, QStringLiteral("Rename Folder"),
						 QStringLiteral("New folder name:"), QLineEdit::Normal,
						 folder_item->text(0), &ok);
	if (!ok || new_name.trimmed().isEmpty())
		return;

	std::string new_folder = new_name.trimmed().toStdString();
	/* Update all instances that belong to this folder */
	for (auto &inst : const_cast<std::vector<MultiviewInstance> &>(config_->instances())) {
		if (inst.folder == old_name)
			inst.folder = new_folder;
	}
	config_->save();
	refresh_instance_list();
}

/* ---- context menu ---- */

void ManagerDialog::show_context_menu(const QPoint &pos)
{
	QMenu menu(this);

	auto *item = instance_tree_->itemAt(pos);
	bool is_on_folder = item && is_folder_item(item);
	bool is_on_instance = item && !is_folder_item(item);
	bool has_selection = !instance_tree_->selectedItems().isEmpty();

	/* Check if selection contains any non-folder items */
	bool sel_has_instance = false;
	for (auto *sel : instance_tree_->selectedItems()) {
		if (!is_folder_item(sel)) {
			sel_has_instance = true;
			break;
		}
	}

	QAction *act_new = menu.addAction(QStringLiteral("New Instance"));
	QAction *act_new_folder = menu.addAction(QStringLiteral("New Folder"));
	menu.addSeparator();

	QAction *act_open = menu.addAction(QStringLiteral("Open"));
	QAction *act_rename = menu.addAction(QStringLiteral("Rename"));
	QAction *act_clone = menu.addAction(QStringLiteral("Clone"));
	QAction *act_move = menu.addAction(QStringLiteral("Move to Folder..."));
	menu.addSeparator();
	QAction *act_delete = menu.addAction(QStringLiteral("Delete"));

	act_open->setEnabled(is_on_instance);
	act_rename->setEnabled(is_on_instance || is_on_folder);
	act_clone->setEnabled(is_on_instance);
	act_move->setEnabled(sel_has_instance && !is_on_folder);
	act_delete->setEnabled(has_selection);

	QAction *chosen = menu.exec(instance_tree_->mapToGlobal(pos));
	if (!chosen)
		return;

	if (chosen == act_new)
		on_new_instance();
	else if (chosen == act_new_folder)
		on_new_folder();
	else if (chosen == act_open)
		on_open_instance();
	else if (chosen == act_rename) {
		if (is_on_folder)
			on_rename_folder(item);
		else
			on_rename_instance();
	} else if (chosen == act_clone)
		on_clone_instance();
	else if (chosen == act_move)
		on_move_to_folder();
	else if (chosen == act_delete)
		on_delete_instance();
}

/* ---- right panel pages ---- */

void ManagerDialog::show_instance_detail(const std::string &uuid)
{
	MultiviewInstance *inst = config_->find_instance(uuid);
	if (!inst) {
		right_stack_->setCurrentIndex(PAGE_EMPTY);
		return;
	}

	current_detail_uuid_ = uuid;

	/* Name */
	detail_name_edit_->blockSignals(true);
	detail_name_edit_->setText(QString::fromStdString(inst->name));
	detail_name_edit_->blockSignals(false);

	/* UUID */
	detail_uuid_label_->setText(QString::fromStdString(inst->uuid));

	/* Gutter */
	detail_use_global_gutter_->blockSignals(true);
	detail_gutter_spin_->blockSignals(true);

	detail_use_global_gutter_->setChecked(inst->useGlobalGutter);
	detail_gutter_spin_->setValue(inst->layout.gutterPx);
	detail_gutter_spin_->setEnabled(!inst->useGlobalGutter);

	int eff = inst->effective_gutter(config_->global_settings().defaultGutterPx);
	detail_gutter_effective_->setText(QStringLiteral("Effective: %1px").arg(eff));

	detail_use_global_gutter_->blockSignals(false);
	detail_gutter_spin_->blockSignals(false);

	/* Grid editor */
	grid_edit_layout_ = inst->layout;
	grid_edit_layout_.gutterPx = 0;

	grid_rows_spin_->blockSignals(true);
	grid_cols_spin_->blockSignals(true);
	grid_rows_spin_->setValue(grid_edit_layout_.rows);
	grid_cols_spin_->setValue(grid_edit_layout_.columns);
	grid_rows_spin_->blockSignals(false);
	grid_cols_spin_->blockSignals(false);

	grid_span_info_->setText(QString());
	btn_add_span_->setEnabled(false);
	btn_remove_span_->setEnabled(false);
	grid_preview_->clear_selection();
	update_grid_preview();

	right_stack_->setCurrentIndex(PAGE_INSTANCE);
}

void ManagerDialog::auto_save_layout()
{
	MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
	if (!inst)
		return;

	inst->layout.rows = grid_edit_layout_.rows;
	inst->layout.columns = grid_edit_layout_.columns;
	inst->layout.spans = grid_edit_layout_.spans;
	inst->layoutDirty = true;
	config_->save();
	notify_multiview_layout_changed(current_detail_uuid_);
}

void ManagerDialog::update_grid_preview()
{
	grid_preview_->set_layout(grid_edit_layout_);
	grid_span_info_->setText(QStringLiteral("%1 span(s)").arg(grid_edit_layout_.spans.size()));
}
