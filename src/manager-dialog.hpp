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

class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QToolButton;
class QStackedWidget;
class QLabel;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QSplitter;
class QMenu;
class GridPreviewWidget;

class ManagerDialog : public QDialog {
	Q_OBJECT

public:
	explicit ManagerDialog(ConfigManager *config, QWidget *parent = nullptr);
	~ManagerDialog() override;

	void refresh_instance_list();

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
	void on_new_instance();
	void on_rename_instance();
	void on_clone_instance();
	void on_delete_instance();
	void on_open_instance();
	void on_instance_selection_changed();

private:
	void setup_ui();
	void setup_instances_tab(QWidget *tab);
	void setup_left_panel(QWidget *panel);
	void setup_right_panel(QWidget *panel);
	void setup_settings_tab(QWidget *tab);
	void show_instance_detail(const std::string &uuid);
	void update_button_states();
	void update_grid_preview();
	void auto_save_layout();
	void select_instance_by_uuid(const std::string &uuid);
	void show_context_menu(const QPoint &pos);
	std::string get_item_uuid(QTreeWidgetItem *item) const;

	ConfigManager *config_;

	/* Top-level tabs */
	QTabWidget *tab_widget_;

	/* Left panel (Instances tab) */
	QSplitter *splitter_;
	QTreeWidget *instance_tree_;
	QToolButton *btn_new_;
	QToolButton *btn_clone_;
	QToolButton *btn_delete_;
	QToolButton *btn_open_;
	QToolButton *btn_move_up_;
	QToolButton *btn_move_down_;

	/* Right panel (Instances tab) */
	QStackedWidget *right_stack_;
	QWidget *page_empty_;
	QWidget *page_instance_;

	/* Instance page widgets */
	QLineEdit *detail_name_edit_;
	QLabel *detail_uuid_label_;
	QToolButton *btn_detail_open_;
	QToolButton *btn_detail_delete_;
	QToolButton *btn_detail_clone_;
	QString name_edit_original_; /* for cancel on Esc/focus-out */

	/* Gutter section */
	QCheckBox *detail_use_global_gutter_;
	QSpinBox *detail_gutter_spin_;
	QLabel *detail_gutter_effective_;

	/* Grid editor (inline) */
	QSpinBox *grid_rows_spin_;
	QSpinBox *grid_cols_spin_;
	GridPreviewWidget *grid_preview_;
	QPushButton *btn_add_span_;
	QPushButton *btn_remove_span_;
	QPushButton *btn_reset_all_;
	QLabel *grid_span_info_;
	std::string current_detail_uuid_;
	LayoutData grid_edit_layout_; /* working copy */

	/* Global settings page widgets */
	QSpinBox *spin_default_gutter_;
	QCheckBox *chk_re_resolve_inherit_;
	QDoubleSpinBox *spin_re_resolve_fps_;
	QLabel *lbl_re_resolve_effective_;
	QCheckBox *chk_safe_area_enabled_;

	void update_re_resolve_effective_label();

	enum { PAGE_EMPTY = 0, PAGE_INSTANCE };
};
