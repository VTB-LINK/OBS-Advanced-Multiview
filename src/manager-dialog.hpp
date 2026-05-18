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

#include "config-manager.hpp"

#include <QDialog>

class QListWidget;
class QPushButton;
class QStackedWidget;
class QLabel;
class QSpinBox;
class QCheckBox;
class GridPreviewWidget;

class ManagerDialog : public QDialog {
	Q_OBJECT

public:
	explicit ManagerDialog(ConfigManager *config,
			       QWidget *parent = nullptr);
	~ManagerDialog() override;

	void refresh_instance_list();

private slots:
	void on_new_instance();
	void on_rename_instance();
	void on_clone_instance();
	void on_delete_instance();
	void on_open_instance();
	void on_instance_selection_changed();
	void on_global_settings_clicked();
	void on_edit_grid_clicked();

private:
	void setup_ui();
	void setup_left_panel(QWidget *panel);
	void setup_right_panel(QWidget *panel);
	void show_instance_detail(const std::string &uuid);
	void show_global_settings();
	void show_grid_editor(const std::string &uuid);
	void update_button_states();
	void update_grid_preview();

	ConfigManager *config_;

	/* Left panel */
	QListWidget *instance_list_;
	QPushButton *btn_new_;
	QPushButton *btn_rename_;
	QPushButton *btn_clone_;
	QPushButton *btn_delete_;
	QPushButton *btn_open_;
	QPushButton *btn_global_settings_;

	/* Right panel */
	QStackedWidget *right_stack_;
	QWidget *page_empty_;
	QWidget *page_instance_detail_;
	QWidget *page_global_settings_;
	QWidget *page_grid_editor_;

	/* Instance detail page widgets */
	QLabel *detail_name_label_;
	QLabel *detail_uuid_label_;
	QLabel *detail_layout_label_;
	QCheckBox *detail_use_global_gutter_;
	QSpinBox *detail_gutter_spin_;
	QLabel *detail_gutter_effective_;
	QPushButton *btn_edit_grid_;
	std::string current_detail_uuid_;

	/* Grid editor page widgets */
	QLabel *grid_editor_title_;
	QSpinBox *grid_rows_spin_;
	QSpinBox *grid_cols_spin_;
	GridPreviewWidget *grid_preview_;
	QPushButton *btn_grid_save_;
	QPushButton *btn_grid_back_;
	QPushButton *btn_add_span_;
	QPushButton *btn_remove_span_;
	QLabel *grid_span_info_;
	std::string grid_edit_uuid_;
	LayoutData grid_edit_layout_; /* working copy */

	/* Global settings page widgets */
	QSpinBox *spin_default_gutter_;

	enum {
		PAGE_EMPTY = 0,
		PAGE_INSTANCE_DETAIL,
		PAGE_GLOBAL_SETTINGS,
		PAGE_GRID_EDITOR
	};
};
