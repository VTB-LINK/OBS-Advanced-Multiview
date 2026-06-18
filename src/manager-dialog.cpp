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
#include "amv-logging.hpp"
#include "amv-i18n.hpp"
#include "cell-display-settings-dialog.hpp"
#include "external-output-settings-dialog.hpp"
#include "signal-lost-settings-dialog.hpp"
#include "grid-preview-widget.hpp"
#include "multiview-window.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
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
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

/* Compute a muted "secondary text" color from the current palette.
 * Blends WindowText and Window at 50% for a reliably readable subdued color. */
static QString secondary_text_color()
{
	QColor fg = QApplication::palette().color(QPalette::WindowText);
	QColor bg = QApplication::palette().color(QPalette::Window);
	QColor muted;
	muted.setRed((fg.red() + bg.red()) / 2);
	muted.setGreen((fg.green() + bg.green()) / 2);
	muted.setBlue((fg.blue() + bg.blue()) / 2);
	return muted.name();
}

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
	setWindowTitle(amv::text("OBSAdvancedMultiview"));
	setMinimumSize(800, 500);
	resize(800, 600);
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
			if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
				/* Apply rename */
				if (!current_detail_uuid_.empty()) {
					QString newName = detail_name_edit_->text().trimmed();
					if (newName.isEmpty()) {
						detail_name_edit_->setText(name_edit_original_);
					} else {
						MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
						if (inst && QString::fromStdString(inst->name) != newName) {
							name_edit_original_ = newName;
							config_->rename_instance(current_detail_uuid_,
										 newName.toStdString());
							config_->save();
							refresh_instance_list();
							select_instance_by_uuid(current_detail_uuid_);
							show_instance_detail(current_detail_uuid_);
							notify_multiview_name_changed(current_detail_uuid_);
						}
					}
				}
				detail_name_edit_->clearFocus();
				return true; /* eat the event so it doesn't trigger buttons */
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
	tab_widget_->addTab(instances_tab, amv::text("AMVPlugin.Manager.Tab.Instances"));

	auto *settings_tab = new QWidget();
	setup_settings_tab(settings_tab);
	tab_widget_->addTab(settings_tab, amv::text("AMVPlugin.Manager.Tab.Settings"));

	main_layout->addWidget(tab_widget_);
}

void ManagerDialog::show_instances_tab_for_uuid(const std::string &uuid)
{
	refresh_instance_list();
	tab_widget_->setCurrentIndex(0);
	select_instance_by_uuid(uuid);
	show_instance_detail(uuid);
	update_button_states();
}

void ManagerDialog::show_settings_tab()
{
	tab_widget_->setCurrentIndex(1);
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
	instance_tree_->setRootIsDecorated(false);
	instance_tree_->setIndentation(0);
	instance_tree_->setIconSize(QSize(0, 0));

	/* Style: simple item padding */
	instance_tree_->setStyleSheet(QStringLiteral("QTreeWidget::item { padding: 2px 4px; }"));

	layout->addWidget(instance_tree_);

	connect(instance_tree_, &QTreeWidget::itemSelectionChanged, this,
		&ManagerDialog::on_instance_selection_changed);
	connect(instance_tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
		if (!item)
			return;
		on_open_instance();
	});
	connect(instance_tree_, &QTreeWidget::customContextMenuRequested, this, &ManagerDialog::show_context_menu);

	/* Bottom toolbar - OBS style icon buttons */
	auto *toolbar = new QHBoxLayout();
	toolbar->setSpacing(2);

	const int btn_sz = 24;
	const int ico_sz = 16;

	btn_new_ = new QToolButton(panel);
	btn_new_->setIcon(obs_theme_icon("plus"));
	btn_new_->setFixedSize(btn_sz, btn_sz);
	btn_new_->setIconSize(QSize(ico_sz, ico_sz));
	btn_new_->setAutoRaise(true);
	btn_new_->setToolTip(amv::text("AMVPlugin.Manager.Instance.New"));
	toolbar->addWidget(btn_new_);

	btn_delete_ = new QToolButton(panel);
	btn_delete_->setIcon(obs_theme_icon("trash"));
	btn_delete_->setFixedSize(btn_sz, btn_sz);
	btn_delete_->setIconSize(QSize(ico_sz, ico_sz));
	btn_delete_->setAutoRaise(true);
	btn_delete_->setToolTip(amv::text("AMVPlugin.Common.Delete"));
	toolbar->addWidget(btn_delete_);

	btn_clone_ = new QToolButton(panel);
	btn_clone_->setIcon(obs_theme_icon("popout"));
	btn_clone_->setFixedSize(btn_sz, btn_sz);
	btn_clone_->setIconSize(QSize(ico_sz, ico_sz));
	btn_clone_->setAutoRaise(true);
	btn_clone_->setToolTip(amv::text("AMVPlugin.Manager.Instance.Clone"));
	toolbar->addWidget(btn_clone_);

	btn_open_ = new QToolButton(panel);
	btn_open_->setIcon(make_open_icon());
	btn_open_->setFixedSize(btn_sz, btn_sz);
	btn_open_->setIconSize(QSize(ico_sz, ico_sz));
	btn_open_->setAutoRaise(true);
	btn_open_->setToolTip(amv::text("AMVPlugin.Manager.Instance.OpenWindow"));
	toolbar->addWidget(btn_open_);

	toolbar->addStretch();

	btn_move_up_ = new QToolButton(panel);
	btn_move_up_->setIcon(obs_theme_icon("up"));
	btn_move_up_->setFixedSize(btn_sz, btn_sz);
	btn_move_up_->setIconSize(QSize(ico_sz, ico_sz));
	btn_move_up_->setAutoRaise(true);
	btn_move_up_->setToolTip(amv::text("AMVPlugin.Common.MoveUp"));
	btn_move_up_->setEnabled(false);
	toolbar->addWidget(btn_move_up_);

	btn_move_down_ = new QToolButton(panel);
	btn_move_down_->setIcon(obs_theme_icon("down"));
	btn_move_down_->setFixedSize(btn_sz, btn_sz);
	btn_move_down_->setIconSize(QSize(ico_sz, ico_sz));
	btn_move_down_->setAutoRaise(true);
	btn_move_down_->setToolTip(amv::text("AMVPlugin.Common.MoveDown"));
	btn_move_down_->setEnabled(false);
	toolbar->addWidget(btn_move_down_);

	layout->addLayout(toolbar);

	connect(btn_new_, &QToolButton::clicked, this, &ManagerDialog::on_new_instance);
	connect(btn_clone_, &QToolButton::clicked, this, &ManagerDialog::on_clone_instance);
	connect(btn_delete_, &QToolButton::clicked, this, &ManagerDialog::on_delete_instance);
	connect(btn_open_, &QToolButton::clicked, this, &ManagerDialog::on_open_instance);
	connect(btn_move_up_, &QToolButton::clicked, this, [this]() {
		auto selected = instance_tree_->selectedItems();
		if (selected.size() != 1)
			return;
		int row = instance_tree_->indexOfTopLevelItem(selected.first());
		if (row <= 0)
			return;
		auto &vec = config_->instances_mutable();
		std::swap(vec[row], vec[row - 1]);
		config_->save();
		std::string uuid = get_item_uuid(selected.first());
		refresh_instance_list();
		select_instance_by_uuid(uuid);
	});
	connect(btn_move_down_, &QToolButton::clicked, this, [this]() {
		auto selected = instance_tree_->selectedItems();
		if (selected.size() != 1)
			return;
		int row = instance_tree_->indexOfTopLevelItem(selected.first());
		int count = instance_tree_->topLevelItemCount();
		if (row < 0 || row >= count - 1)
			return;
		auto &vec = config_->instances_mutable();
		std::swap(vec[row], vec[row + 1]);
		config_->save();
		std::string uuid = get_item_uuid(selected.first());
		refresh_instance_list();
		select_instance_by_uuid(uuid);
	});
}

void ManagerDialog::setup_right_panel(QWidget *panel)
{
	auto *layout = new QVBoxLayout(panel);

	right_stack_ = new QStackedWidget(panel);

	/* Page 0: empty placeholder */
	page_empty_ = new QWidget();
	auto *empty_layout = new QVBoxLayout(page_empty_);
	auto *empty_label = new QLabel(amv::text("AMVPlugin.Manager.Placeholder.SelectInstance"), page_empty_);
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

	btn_detail_open_ = new QToolButton(page_instance_);
	btn_detail_open_->setIcon(make_open_icon());
	btn_detail_open_->setFixedSize(24, 24);
	btn_detail_open_->setIconSize(QSize(16, 16));
	btn_detail_open_->setAutoRaise(true);
	btn_detail_open_->setToolTip(amv::text("AMVPlugin.Common.Open"));
	top_row->addWidget(btn_detail_open_);

	btn_detail_delete_ = new QToolButton(page_instance_);
	btn_detail_delete_->setIcon(obs_theme_icon("trash"));
	btn_detail_delete_->setFixedSize(24, 24);
	btn_detail_delete_->setIconSize(QSize(16, 16));
	btn_detail_delete_->setAutoRaise(true);
	btn_detail_delete_->setToolTip(amv::text("AMVPlugin.Common.Delete"));
	top_row->addWidget(btn_detail_delete_);

	btn_detail_clone_ = new QToolButton(page_instance_);
	btn_detail_clone_->setIcon(obs_theme_icon("popout"));
	btn_detail_clone_->setFixedSize(24, 24);
	btn_detail_clone_->setIconSize(QSize(16, 16));
	btn_detail_clone_->setAutoRaise(true);
	btn_detail_clone_->setToolTip(amv::text("AMVPlugin.Common.Clone"));
	top_row->addWidget(btn_detail_clone_);

	p_layout->addLayout(top_row);

	/* UUID label */
	detail_uuid_label_ = new QLabel(page_instance_);
	detail_uuid_label_->setStyleSheet(QString("color: %1; font-size: 11px;").arg(secondary_text_color()));
	p_layout->addWidget(detail_uuid_label_);

	/* --- Separator --- */
	auto *sep1 = new QFrame(page_instance_);
	sep1->setFrameShape(QFrame::HLine);
	sep1->setFrameShadow(QFrame::Sunken);
	p_layout->addWidget(sep1);

	/* --- Gutter settings (single row) --- */
	auto *gutter_row = new QHBoxLayout();
	detail_use_global_gutter_ = new QCheckBox(amv::text("AMVPlugin.Manager.Gutter.Inherit"), page_instance_);
	gutter_row->addWidget(detail_use_global_gutter_);
	gutter_row->addStretch();
	gutter_row->addWidget(new QLabel(amv::text("AMVPlugin.Manager.Gutter.Label"), page_instance_));
	detail_gutter_spin_ = new QSpinBox(page_instance_);
	detail_gutter_spin_->setRange(0, 50);
	detail_gutter_spin_->setMinimumWidth(60);
	gutter_row->addWidget(detail_gutter_spin_);
	detail_gutter_effective_ = new QLabel(page_instance_);
	detail_gutter_effective_->setStyleSheet(QString("color: %1;").arg(secondary_text_color()));
	gutter_row->addWidget(detail_gutter_effective_);
	p_layout->addLayout(gutter_row);

	/* --- Scene-click switching row (instance override) --- */
	auto *scene_click_row = new QHBoxLayout();
	detail_use_global_scene_click_ =
		new QCheckBox(amv::text("AMVPlugin.Manager.SceneClick.Inherit"), page_instance_);
	scene_click_row->addWidget(detail_use_global_scene_click_);
	scene_click_row->addStretch();
	detail_scene_click_enabled_ = new QCheckBox(amv::text("AMVPlugin.Manager.SceneClick.Enable"), page_instance_);
	detail_scene_click_enabled_->setToolTip(amv::text("AMVPlugin.Manager.SceneClick.InstanceTooltip"));
	scene_click_row->addWidget(detail_scene_click_enabled_);
	detail_scene_double_click_program_ =
		new QCheckBox(amv::text("AMVPlugin.Manager.SceneClick.DoubleClickProgram"), page_instance_);
	detail_scene_double_click_program_->setToolTip(
		amv::text("AMVPlugin.Manager.SceneClick.DoubleClickProgramTooltip"));
	scene_click_row->addWidget(detail_scene_double_click_program_);
	detail_scene_click_effective_ = new QLabel(page_instance_);
	detail_scene_click_effective_->setStyleSheet(QString("color: %1;").arg(secondary_text_color()));
	scene_click_row->addWidget(detail_scene_click_effective_);
	p_layout->addLayout(scene_click_row);

	/* --- Instance Visual Settings button --- */
	auto *btn_instance_visual =
		new QPushButton(amv::text("AMVPlugin.Manager.Instance.VisualSettings"), page_instance_);
	p_layout->addWidget(btn_instance_visual);
	connect(btn_instance_visual, &QPushButton::clicked, this, [this]() {
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst)
			return;
		CellDisplaySettingsDialog dlg(CellDisplaySettingsDialog::Mode::Instance, this);
		dlg.set_instance_settings(inst->visualSettings);
		if (dlg.exec() == QDialog::Accepted) {
			inst->visualSettings = dlg.get_instance_settings();
			config_->save();
			notify_multiview_visual_settings_changed(current_detail_uuid_);
		}
	});

	/* --- External Output (Spout/NDI) button (issue #11) --- */
	auto *btn_instance_output =
		new QPushButton(amv::text("AMVPlugin.Manager.Instance.ExternalOutput"), page_instance_);
	p_layout->addWidget(btn_instance_output);
	connect(btn_instance_output, &QPushButton::clicked, this, [this]() {
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst)
			return;
		ExternalOutputSettingsDialog dlg(this);
		dlg.set_settings(inst->outputSettings);
		if (dlg.exec() == QDialog::Accepted) {
			inst->outputSettings = dlg.get_settings();
			config_->save();
			/* Re-apply to the (possibly open) window and refresh the
			 * list so the italic+bold output marker updates. */
			notify_multiview_output_settings_changed(current_detail_uuid_);
			/* refresh_instance_list() rebuilt the tree to update the
			 * italic+bold output marker, which cleared the selection.
			 * Re-select with signals LIVE (not select_instance_by_uuid,
			 * which blocks them) so on_instance_selection_changed fires
			 * and restores both the row highlight and the detail panel —
			 * same pattern as on_new_instance. */
			refresh_instance_list();
			const QString target = QString::fromStdString(current_detail_uuid_);
			for (int i = 0; i < instance_tree_->topLevelItemCount(); i++) {
				auto *it = instance_tree_->topLevelItem(i);
				if (it->data(0, Qt::UserRole).toString() == target) {
					instance_tree_->setCurrentItem(it);
					break;
				}
			}
		}
	});

	/* --- Separator --- */
	auto *sep2 = new QFrame(page_instance_);
	sep2->setFrameShape(QFrame::HLine);
	sep2->setFrameShadow(QFrame::Sunken);
	p_layout->addWidget(sep2);

	/* --- Grid controls row --- */
	auto *grid_ctrl_row = new QHBoxLayout();
	grid_ctrl_row->addWidget(new QLabel(amv::text("AMVPlugin.Manager.Grid.Rows"), page_instance_));
	grid_rows_spin_ = new QSpinBox(page_instance_);
	grid_rows_spin_->setRange(1, 10);
	grid_rows_spin_->setMinimumWidth(60);
	grid_ctrl_row->addWidget(grid_rows_spin_);

	grid_ctrl_row->addWidget(new QLabel(amv::text("AMVPlugin.Manager.Grid.Cols"), page_instance_));
	grid_cols_spin_ = new QSpinBox(page_instance_);
	grid_cols_spin_->setRange(1, 10);
	grid_cols_spin_->setMinimumWidth(60);
	grid_ctrl_row->addWidget(grid_cols_spin_);

	grid_ctrl_row->addStretch();

	btn_add_span_ = new QPushButton(amv::text("AMVPlugin.Manager.Grid.Merge"), page_instance_);
	btn_add_span_->setEnabled(false);
	grid_ctrl_row->addWidget(btn_add_span_);

	btn_remove_span_ = new QPushButton(amv::text("AMVPlugin.Manager.Grid.Unmerge"), page_instance_);
	btn_remove_span_->setEnabled(false);
	grid_ctrl_row->addWidget(btn_remove_span_);

	p_layout->addLayout(grid_ctrl_row);

	/* Span info row: label left, Reset All button right */
	auto *span_row = new QHBoxLayout();
	grid_span_info_ = new QLabel(page_instance_);
	grid_span_info_->setStyleSheet(QString("color: %1;").arg(secondary_text_color()));
	span_row->addWidget(grid_span_info_, 1);
	btn_reset_all_ = new QPushButton(amv::text("AMVPlugin.Manager.Grid.ResetAll"), page_instance_);
	btn_reset_all_->setFixedWidth(btn_remove_span_->sizeHint().width());
	span_row->addWidget(btn_reset_all_);
	p_layout->addLayout(span_row);

	/* --- Grid preview (fills remaining space) --- */
	grid_preview_ = new GridPreviewWidget(page_instance_);
	p_layout->addWidget(grid_preview_, 1);

	right_stack_->addWidget(page_instance_);

	/* ---- Connections ---- */

	/* Name edit: Enter applies (handled in eventFilter), Esc/focus-out cancels */

	/* Action buttons */
	connect(btn_detail_open_, &QToolButton::clicked, this, &ManagerDialog::on_open_instance);
	connect(btn_detail_delete_, &QToolButton::clicked, this, &ManagerDialog::on_delete_instance);
	connect(btn_detail_clone_, &QToolButton::clicked, this, &ManagerDialog::on_clone_instance);

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
		detail_gutter_effective_->setText(amv::text("AMVPlugin.Manager.EffectivePx").arg(eff));
		config_->save();
		notify_multiview_layout_changed(current_detail_uuid_);
	});

	connect(detail_gutter_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst || inst->useGlobalGutter)
			return;
		inst->layout.gutterPx = value;
		detail_gutter_effective_->setText(amv::text("AMVPlugin.Manager.EffectivePx").arg(value));
		config_->save();
		notify_multiview_layout_changed(current_detail_uuid_);
	});

	/* Scene-click switching: instance inheritance + override */
	connect(detail_use_global_scene_click_, &QCheckBox::toggled, this, [this](bool checked) {
		detail_scene_click_enabled_->setEnabled(!checked);
		detail_scene_double_click_program_->setEnabled(!checked);
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst)
			return;
		inst->useGlobalSceneClickSwitch = checked;
		SceneClickSwitchSettings eff =
			inst->effective_scene_click_switch(config_->global_settings().sceneClickSwitch);
		detail_scene_click_effective_->setText(
			amv::text("AMVPlugin.Manager.SceneClick.Effective")
				.arg(amv::text(eff.enabled ? "AMVPlugin.Common.On" : "AMVPlugin.Common.Off"))
				.arg(amv::text(eff.doubleClickProgramEnabled ? "AMVPlugin.Common.On"
									     : "AMVPlugin.Common.Off")));
		config_->save();
	});

	connect(detail_scene_click_enabled_, &QCheckBox::toggled, this, [this](bool checked) {
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst || inst->useGlobalSceneClickSwitch)
			return;
		inst->sceneClickSwitch.enabled = checked;
		SceneClickSwitchSettings eff =
			inst->effective_scene_click_switch(config_->global_settings().sceneClickSwitch);
		detail_scene_click_effective_->setText(
			amv::text("AMVPlugin.Manager.SceneClick.Effective")
				.arg(amv::text(eff.enabled ? "AMVPlugin.Common.On" : "AMVPlugin.Common.Off"))
				.arg(amv::text(eff.doubleClickProgramEnabled ? "AMVPlugin.Common.On"
									     : "AMVPlugin.Common.Off")));
		config_->save();
	});

	connect(detail_scene_double_click_program_, &QCheckBox::toggled, this, [this](bool checked) {
		MultiviewInstance *inst = config_->find_instance(current_detail_uuid_);
		if (!inst || inst->useGlobalSceneClickSwitch)
			return;
		inst->sceneClickSwitch.doubleClickProgramEnabled = checked;
		SceneClickSwitchSettings eff =
			inst->effective_scene_click_switch(config_->global_settings().sceneClickSwitch);
		detail_scene_click_effective_->setText(
			amv::text("AMVPlugin.Manager.SceneClick.Effective")
				.arg(amv::text(eff.enabled ? "AMVPlugin.Common.On" : "AMVPlugin.Common.Off"))
				.arg(amv::text(eff.doubleClickProgramEnabled ? "AMVPlugin.Common.On"
									     : "AMVPlugin.Common.Off")));
		config_->save();
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

		/* Check if we need to absorb existing spans */
		std::vector<int> absorbed;
		bool has_span = grid_preview_->selection_overlaps_span();
		if (has_span) {
			if (!grid_preview_->selection_can_absorb_spans(absorbed))
				return;
			/* Remove absorbed spans in reverse order */
			for (auto it = absorbed.rbegin(); it != absorbed.rend(); ++it)
				grid_edit_layout_.spans.erase(grid_edit_layout_.spans.begin() + *it);
		}

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

		std::vector<int> absorbed;
		bool can_absorb = mergeable && has_span && grid_preview_->selection_can_absorb_spans(absorbed);

		btn_add_span_->setEnabled(mergeable && (!has_span || can_absorb));
		btn_remove_span_->setEnabled(has_span);

		if (sel.size() == 1) {
			auto [r, c] = *sel.begin();
			grid_span_info_->setText(has_span ? amv::text("AMVPlugin.Manager.Grid.SpanCellAt").arg(r).arg(c)
							  : amv::text("AMVPlugin.Manager.Grid.CellAt").arg(r).arg(c));
		} else if (mergeable && !has_span) {
			grid_span_info_->setText(amv::text("AMVPlugin.Manager.Grid.ReadyToMerge")
							 .arg(sr.rowSpan)
							 .arg(sr.colSpan)
							 .arg(sr.row)
							 .arg(sr.col));
		} else if (can_absorb) {
			grid_span_info_->setText(amv::text("AMVPlugin.Manager.Grid.AbsorbSpans")
							 .arg(sr.rowSpan)
							 .arg(sr.colSpan)
							 .arg(sr.row)
							 .arg(sr.col)
							 .arg(absorbed.size()));
		} else if (!mergeable) {
			grid_span_info_->setText(amv::text("AMVPlugin.Manager.Grid.InvalidRectangle"));
		} else {
			grid_span_info_->setText(amv::text("AMVPlugin.Manager.Grid.OverlapsSpan"));
		}
	});

	layout->addWidget(right_stack_);
}

void ManagerDialog::setup_settings_tab(QWidget *tab)
{
	auto *layout = new QVBoxLayout(tab);
	layout->setContentsMargins(12, 12, 12, 12);

	auto *apply_label = new QLabel(amv::text("AMVPlugin.Manager.Settings.RequireApply"), tab);
	apply_label->setStyleSheet(QStringLiteral("font-weight: bold;"));
	layout->addWidget(apply_label);

	auto *gutter_row = new QHBoxLayout();
	gutter_row->addWidget(new QLabel(amv::text("AMVPlugin.Manager.Settings.DefaultGutter"), tab));
	spin_default_gutter_ = new QSpinBox(tab);
	spin_default_gutter_->setRange(0, 50);
	spin_default_gutter_->setValue(config_->global_settings().defaultGutterPx);
	gutter_row->addWidget(spin_default_gutter_);
	gutter_row->addStretch();
	layout->addLayout(gutter_row);

	/* Re-resolve interval */
	auto *resolve_row = new QHBoxLayout();
	chk_re_resolve_inherit_ = new QCheckBox(amv::text("AMVPlugin.Manager.Settings.InheritReResolve"), tab);
	chk_re_resolve_inherit_->setChecked(config_->global_settings().reResolveInheritObs);
	resolve_row->addWidget(chk_re_resolve_inherit_);

	resolve_row->addWidget(new QLabel(amv::text("AMVPlugin.Manager.Settings.CustomFps"), tab));
	spin_re_resolve_fps_ = new QDoubleSpinBox(tab);
	spin_re_resolve_fps_->setRange(1.0, 120.0);
	spin_re_resolve_fps_->setDecimals(2);
	spin_re_resolve_fps_->setSingleStep(1.0);
	spin_re_resolve_fps_->setMinimumWidth(80);
	spin_re_resolve_fps_->setValue(config_->global_settings().reResolveCustomFps);
	spin_re_resolve_fps_->setEnabled(!config_->global_settings().reResolveInheritObs);
	resolve_row->addWidget(spin_re_resolve_fps_);

	lbl_re_resolve_effective_ = new QLabel(tab);
	lbl_re_resolve_effective_->setStyleSheet(QString("color: %1;").arg(secondary_text_color()));
	resolve_row->addWidget(lbl_re_resolve_effective_);
	resolve_row->addStretch();
	layout->addLayout(resolve_row);

	/* Compute initial effective display */
	update_re_resolve_effective_label();

	connect(chk_re_resolve_inherit_, &QCheckBox::toggled, this, [this](bool checked) {
		spin_re_resolve_fps_->setEnabled(!checked);
		update_re_resolve_effective_label();
	});

	connect(spin_re_resolve_fps_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
		[this](double) { update_re_resolve_effective_label(); });

	auto *gs_apply = new QPushButton(amv::text("AMVPlugin.Common.Apply"), tab);
	layout->addWidget(gs_apply);

	connect(gs_apply, &QPushButton::clicked, this, [this]() {
		config_->global_settings().defaultGutterPx = spin_default_gutter_->value();
		config_->global_settings().reResolveInheritObs = chk_re_resolve_inherit_->isChecked();
		config_->global_settings().reResolveCustomFps = spin_re_resolve_fps_->value();
		config_->save();
		obs_log(LOG_INFO, "global settings saved (gutter=%d, reResolve=%s %.2f fps)",
			spin_default_gutter_->value(), chk_re_resolve_inherit_->isChecked() ? "inherit" : "custom",
			spin_re_resolve_fps_->value());
		update_re_resolve_effective_label();
		notify_multiview_layout_changed();
	});

	/* Visual Settings button */
	layout->addSpacing(12);
	auto *vs_label = new QLabel(amv::text("AMVPlugin.Manager.Settings.Immediate"), tab);
	vs_label->setStyleSheet(QStringLiteral("font-weight: bold;"));
	layout->addWidget(vs_label);

	/* Scene-click switch toggle (immediate save). Mirrors OBS built-in
	 * MultiviewMouseSwitch: when on, left-clicking a scene cell calls
	 * obs_frontend_set_current_scene which routes to PRVW under Studio Mode
	 * or to PGM otherwise. Non-scene cells are silently ignored. */
	chk_scene_click_switch_ = new QCheckBox(amv::text("AMVPlugin.Manager.SceneClick.GlobalEnable"), tab);
	chk_scene_click_switch_->setChecked(config_->global_settings().sceneClickSwitch.enabled);
	chk_scene_click_switch_->setToolTip(amv::text("AMVPlugin.Manager.SceneClick.GlobalTooltip"));
	layout->addWidget(chk_scene_click_switch_);
	chk_scene_double_click_program_ =
		new QCheckBox(amv::text("AMVPlugin.Manager.SceneClick.GlobalDoubleClickProgram"), tab);
	chk_scene_double_click_program_->setChecked(
		config_->global_settings().sceneClickSwitch.doubleClickProgramEnabled);
	chk_scene_double_click_program_->setToolTip(
		amv::text("AMVPlugin.Manager.SceneClick.GlobalDoubleClickProgramTooltip"));
	layout->addWidget(chk_scene_double_click_program_);

	connect(chk_scene_click_switch_, &QCheckBox::toggled, this, [this](bool checked) {
		config_->global_settings().sceneClickSwitch.enabled = checked;
		config_->save();
		obs_log(LOG_INFO, "scene click switch %s (global)", checked ? "enabled" : "disabled");
	});

	connect(chk_scene_double_click_program_, &QCheckBox::toggled, this, [this](bool checked) {
		config_->global_settings().sceneClickSwitch.doubleClickProgramEnabled = checked;
		config_->save();
		obs_log(LOG_INFO, "scene double-click program switch %s (global)", checked ? "enabled" : "disabled");
	});

	auto *btn_global_visual = new QPushButton(amv::text("AMVPlugin.Manager.Settings.EditGlobalVisual"), tab);
	layout->addWidget(btn_global_visual);

	connect(btn_global_visual, &QPushButton::clicked, this, [this]() {
		CellDisplaySettingsDialog dlg(CellDisplaySettingsDialog::Mode::Global, this);
		dlg.set_global_settings(config_->global_settings().visualSettings);
		if (dlg.exec() == QDialog::Accepted) {
			config_->global_settings().visualSettings = dlg.get_global_settings();
			config_->save();
			notify_multiview_visual_settings_changed();
		}
	});

	/* Phase 3 / M5.2: Global Signal Lost Settings entry. Sits next to the
	 * existing Global Visual Settings button so users can reach both project-
	 * wide defaults from the same place. Per-cell overrides live in the
	 * MultiviewWindow right-click menu (Signal Lost Settings...). */
	auto *btn_global_signal_lost =
		new QPushButton(amv::text("AMVPlugin.Manager.Settings.EditGlobalSignalLost"), tab);
	layout->addWidget(btn_global_signal_lost);

	connect(btn_global_signal_lost, &QPushButton::clicked, this, [this]() {
		SignalLostSettingsDialog dlg(SignalLostSettingsDialog::Mode::Global, this);
		dlg.set_global_settings(config_->global_settings().lostSignal);
		if (dlg.exec() == QDialog::Accepted) {
			config_->global_settings().lostSignal = dlg.get_global_settings();
			config_->save();
			notify_multiview_signal_settings_changed();
		}
	});

	/* Phase 3 hardening tail: Detailed logs toggle. Off by default; gates
	 * high-frequency diagnostic INFO logs ([perf] every 5s, [health] retry,
	 * [fill] aspect/snap, VU rebuild summaries, provider "created private
	 * source" success). WARNING / ERROR are not affected. */
	layout->addSpacing(12);
	auto *diagnostics_label = new QLabel(amv::text("AMVPlugin.Manager.Settings.Diagnostics"), tab);
	diagnostics_label->setStyleSheet(QStringLiteral("font-weight: bold;"));
	layout->addWidget(diagnostics_label);
	chk_detailed_logs_ = new QCheckBox(amv::text("AMVPlugin.Manager.Settings.DetailedLogs"), tab);
	chk_detailed_logs_->setChecked(config_->global_settings().detailedLogs);
	chk_detailed_logs_->setToolTip(amv::text("AMVPlugin.Manager.Settings.DetailedLogsTooltip"));
	layout->addWidget(chk_detailed_logs_);

	connect(chk_detailed_logs_, &QCheckBox::toggled, this, [this](bool checked) {
		config_->global_settings().detailedLogs = checked;
		amv::set_detailed_logs_enabled(checked);
		config_->save();
		obs_log(LOG_INFO, "detailed logs %s", checked ? "enabled" : "disabled");
	});

	layout->addSpacing(12);
	auto *about_label = new QLabel(amv::text("AMVPlugin.Manager.Settings.About"), tab);
	about_label->setStyleSheet(QStringLiteral("font-weight: bold;"));
	layout->addWidget(about_label);

	auto *about_text = new QLabel(tab);
	about_text->setTextFormat(Qt::RichText);
	about_text->setTextInteractionFlags(Qt::TextBrowserInteraction);
	about_text->setOpenExternalLinks(false);
	about_text->setText(QStringLiteral("<a href=\"https://vtb.link/obs-amv\">OBS Advanced Multiview</a> "
					   "(%1) by <a href=\"https://vtb.link\">VTB-LINK</a>")
				    .arg(QString::fromUtf8(PLUGIN_VERSION).toHtmlEscaped()));
	layout->addWidget(about_text);

	connect(about_text, &QLabel::linkActivated, this,
		[](const QString &link) { QDesktopServices::openUrl(QUrl(link)); });

	layout->addStretch();
}

void ManagerDialog::update_re_resolve_effective_label()
{
	double fps;
	if (chk_re_resolve_inherit_->isChecked()) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi))
			fps = (double)ovi.fps_num / (double)ovi.fps_den;
		else
			fps = 30.0;
	} else {
		fps = spin_re_resolve_fps_->value();
	}
	lbl_re_resolve_effective_->setText(amv::text("AMVPlugin.Manager.EffectiveFps").arg(fps, 0, 'f', 2));
}

/* ---- instance list ---- */

void ManagerDialog::refresh_instance_list()
{
	instance_tree_->clear();

	for (auto &inst : config_->instances()) {
		auto *item = new QTreeWidgetItem();
		item->setText(0, QString::fromStdString(inst.name));
		item->setData(0, Qt::UserRole, QString::fromStdString(inst.uuid));
		/* Issue #11: an instance with any external output (Spout/NDI)
		 * enabled is shown italic + bold so it stands out in the list. */
		if (inst.outputSettings.any_enabled()) {
			QFont f = item->font(0);
			f.setItalic(true);
			f.setBold(true);
			item->setFont(0, f);
		}
		instance_tree_->addTopLevelItem(item);
	}

	update_button_states();
}

void ManagerDialog::select_instance_by_uuid(const std::string &uuid)
{
	if (uuid.empty())
		return;
	QString target = QString::fromStdString(uuid);
	for (int i = 0; i < instance_tree_->topLevelItemCount(); i++) {
		auto *item = instance_tree_->topLevelItem(i);
		if (item->data(0, Qt::UserRole).toString() == target) {
			instance_tree_->blockSignals(true);
			instance_tree_->setCurrentItem(item);
			instance_tree_->blockSignals(false);
			return;
		}
	}
}

std::string ManagerDialog::get_item_uuid(QTreeWidgetItem *item) const
{
	if (!item)
		return {};
	return item->data(0, Qt::UserRole).toString().toStdString();
}

void ManagerDialog::update_button_states()
{
	auto selected = instance_tree_->selectedItems();
	bool has_selection = !selected.isEmpty();
	bool single = (selected.size() == 1);

	btn_clone_->setEnabled(single);
	btn_delete_->setEnabled(has_selection);
	btn_open_->setEnabled(has_selection);

	/* Move Up/Down: enabled when single selection and not at boundary */
	if (single) {
		int row = instance_tree_->indexOfTopLevelItem(selected.first());
		int count = instance_tree_->topLevelItemCount();
		btn_move_up_->setEnabled(row > 0);
		btn_move_down_->setEnabled(row >= 0 && row < count - 1);
	} else {
		btn_move_up_->setEnabled(false);
		btn_move_down_->setEnabled(false);
	}
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
	std::string uuid = get_item_uuid(item);
	if (!uuid.empty())
		show_instance_detail(uuid);
}

void ManagerDialog::on_new_instance()
{
	bool ok;
	QString name = QInputDialog::getText(this, amv::text("AMVPlugin.Manager.Instance.New"),
					     amv::text("AMVPlugin.Manager.Instance.NamePrompt"), QLineEdit::Normal,
					     QString(), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	MultiviewInstance *inst = config_->add_instance(name.trimmed().toStdString());
	(void)inst;
	config_->save();
	refresh_instance_list();

	/* Select the new item (last item) */
	int count = instance_tree_->topLevelItemCount();
	if (count > 0)
		instance_tree_->setCurrentItem(instance_tree_->topLevelItem(count - 1));
}

void ManagerDialog::on_rename_instance()
{
	auto selected = instance_tree_->selectedItems();
	if (selected.size() != 1)
		return;
	auto *item = selected.first();

	std::string uuid = get_item_uuid(item);

	bool ok;
	QString name = QInputDialog::getText(this, amv::text("AMVPlugin.Manager.Instance.Rename"),
					     amv::text("AMVPlugin.Manager.Instance.NewNamePrompt"), QLineEdit::Normal,
					     item->text(0), &ok);
	if (!ok || name.trimmed().isEmpty())
		return;

	config_->rename_instance(uuid, name.trimmed().toStdString());
	config_->save();
	refresh_instance_list();
	select_instance_by_uuid(uuid);
	/* refresh_instance_list() cleared the tree, which fired the selection-changed
	 * slot and flipped the detail pane to PAGE_EMPTY; select_instance_by_uuid()
	 * re-selects with signals blocked, so restore the detail pane explicitly
	 * (mirrors the detail-area rename path). Without this the row reselects but
	 * the right-hand detail/selection appears lost. */
	show_instance_detail(uuid);
	notify_multiview_name_changed(uuid);
}

void ManagerDialog::on_clone_instance()
{
	auto selected = instance_tree_->selectedItems();
	if (selected.size() != 1)
		return;
	auto *item = selected.first();

	std::string uuid = get_item_uuid(item);

	bool ok;
	QString name = QInputDialog::getText(this, amv::text("AMVPlugin.Manager.Instance.Clone"),
					     amv::text("AMVPlugin.Manager.Instance.CloneNamePrompt"), QLineEdit::Normal,
					     item->text(0) + amv::text("AMVPlugin.Manager.Instance.CopySuffix"), &ok);
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

	/* Collect UUIDs */
	QStringList names;
	std::vector<std::string> uuids;
	for (auto *item : selected) {
		std::string u = get_item_uuid(item);
		if (!u.empty()) {
			uuids.push_back(u);
			names.append(item->text(0));
		}
	}
	if (uuids.empty())
		return;

	/* Build confirmation message */
	QString msg;
	if (uuids.size() == 1)
		msg = amv::text("AMVPlugin.Manager.Instance.DeleteOneConfirm").arg(names.first());
	else
		msg = amv::text("AMVPlugin.Manager.Instance.DeleteManyConfirm").arg(uuids.size());

	auto ret = QMessageBox::question(this, amv::text("AMVPlugin.Common.Delete"), msg,
					 QMessageBox::Yes | QMessageBox::No);
	if (ret != QMessageBox::Yes)
		return;

	/* Close open windows for instances being deleted */
	for (auto &uuid : uuids)
		close_multiview_window(uuid);

	for (auto &uuid : uuids) {
		MultiviewInstance *inst = config_->find_instance(uuid);
		obs_log(LOG_INFO, "[manager] deleting instance '%s' (%s)", inst ? inst->name.c_str() : "?",
			uuid.c_str());
		config_->delete_instance(uuid);
	}

	config_->save();
	refresh_instance_list();
	current_detail_uuid_.clear();
	right_stack_->setCurrentIndex(PAGE_EMPTY);
}

void ManagerDialog::on_open_instance()
{
	auto selected = instance_tree_->selectedItems();
	for (auto *item : selected) {
		std::string uuid = get_item_uuid(item);
		if (!uuid.empty())
			open_multiview_window(uuid);
	}
}

/* ---- context menu ---- */

void ManagerDialog::show_context_menu(const QPoint &pos)
{
	QMenu menu(this);

	auto *item = instance_tree_->itemAt(pos);
	auto selected = instance_tree_->selectedItems();
	bool has_selection = !selected.isEmpty();
	bool single = (selected.size() == 1);
	bool is_on_item = (item != nullptr);

	QAction *act_new = menu.addAction(amv::text("AMVPlugin.Manager.Instance.New"));
	menu.addSeparator();

	QAction *act_open = menu.addAction(amv::text("AMVPlugin.Common.Open"));
	QAction *act_rename = menu.addAction(amv::text("AMVPlugin.Common.Rename"));
	QAction *act_clone = menu.addAction(amv::text("AMVPlugin.Common.Clone"));
	menu.addSeparator();
	QAction *act_delete = menu.addAction(amv::text("AMVPlugin.Common.Delete"));

	act_open->setEnabled(is_on_item && has_selection);
	act_rename->setEnabled(is_on_item && single);
	act_clone->setEnabled(is_on_item && single);
	act_delete->setEnabled(has_selection);

	QAction *chosen = menu.exec(instance_tree_->mapToGlobal(pos));
	if (!chosen)
		return;

	if (chosen == act_new)
		on_new_instance();
	else if (chosen == act_open)
		on_open_instance();
	else if (chosen == act_rename)
		on_rename_instance();
	else if (chosen == act_clone)
		on_clone_instance();
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
	detail_gutter_effective_->setText(amv::text("AMVPlugin.Manager.EffectivePx").arg(eff));

	detail_use_global_gutter_->blockSignals(false);
	detail_gutter_spin_->blockSignals(false);

	/* Scene-click switching */
	detail_use_global_scene_click_->blockSignals(true);
	detail_scene_click_enabled_->blockSignals(true);
	detail_scene_double_click_program_->blockSignals(true);
	detail_use_global_scene_click_->setChecked(inst->useGlobalSceneClickSwitch);
	detail_scene_click_enabled_->setChecked(inst->sceneClickSwitch.enabled);
	detail_scene_double_click_program_->setChecked(inst->sceneClickSwitch.doubleClickProgramEnabled);
	detail_scene_click_enabled_->setEnabled(!inst->useGlobalSceneClickSwitch);
	detail_scene_double_click_program_->setEnabled(!inst->useGlobalSceneClickSwitch);
	{
		SceneClickSwitchSettings eff =
			inst->effective_scene_click_switch(config_->global_settings().sceneClickSwitch);
		detail_scene_click_effective_->setText(
			amv::text("AMVPlugin.Manager.SceneClick.Effective")
				.arg(amv::text(eff.enabled ? "AMVPlugin.Common.On" : "AMVPlugin.Common.Off"))
				.arg(amv::text(eff.doubleClickProgramEnabled ? "AMVPlugin.Common.On"
									     : "AMVPlugin.Common.Off")));
	}
	detail_use_global_scene_click_->blockSignals(false);
	detail_scene_click_enabled_->blockSignals(false);
	detail_scene_double_click_program_->blockSignals(false);

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
	grid_span_info_->setText(amv::text("AMVPlugin.Manager.Grid.SpanCount").arg(grid_edit_layout_.spans.size()));
}
