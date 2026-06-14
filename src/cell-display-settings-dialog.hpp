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

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSpinBox>

class QLabel;
class QGroupBox;
class QPushButton;
class QVBoxLayout;

class CellDisplaySettingsDialog : public QDialog {
	Q_OBJECT

public:
	/* mode: "global" = no inheritance UI, "instance" = inherits global,
	 * "cell" = inherits instance */
	enum class Mode { Global, Instance, Cell };

	CellDisplaySettingsDialog(Mode mode, QWidget *parent = nullptr);

	/* Set/get visual settings - use the appropriate pair based on mode */
	void set_global_settings(const GlobalVisualSettings &gs);
	GlobalVisualSettings get_global_settings() const;

	void set_instance_settings(const InstanceVisualSettings &is);
	InstanceVisualSettings get_instance_settings() const;

	void set_cell_settings(const CellVisualSettings &cs);
	CellVisualSettings get_cell_settings() const;

	/* Cell row/col, for display purposes */
	void set_cell_position(int row, int col);
	void set_external_cell(bool external);

signals:
	void settings_changed();

private:
	void setup_ui();
	QGroupBox *create_background_group();
	QGroupBox *create_label_group();
	QGroupBox *create_safe_area_group();
	QGroupBox *create_vu_meter_group();
	QGroupBox *create_overlay_group();
	QGroupBox *create_highlight_group();
	void update_inheritance_visibility();
	void update_label_control_states();
	void update_vu_meter_control_states();

	Mode mode_;
	int cell_row_ = -1;
	int cell_col_ = -1;
	bool is_external_cell_ = false;
	bool dirty_ = false;

	/* Inheritance combos (only for Instance/Cell mode) */
	QComboBox *cmb_bg_inherit_ = nullptr;
	QComboBox *cmb_label_inherit_ = nullptr;
	QComboBox *cmb_safe_area_inherit_ = nullptr;
	QComboBox *cmb_vu_meter_inherit_ = nullptr;
	QComboBox *cmb_overlay_inherit_ = nullptr;
	QComboBox *cmb_highlight_inherit_ = nullptr;

	/* Background group */
	QGroupBox *grp_background_ = nullptr;
	QCheckBox *chk_bg_color_enabled_ = nullptr;
	QLineEdit *edit_bg_color_ = nullptr;
	QComboBox *cmb_bg_fill_mode_ = nullptr;
	QCheckBox *chk_bg_label_fill_ = nullptr;
	QCheckBox *chk_bg_image_enabled_ = nullptr;
	QLineEdit *edit_bg_image_path_ = nullptr;
	QComboBox *cmb_bg_image_fit_ = nullptr;

	/* Label group */
	QGroupBox *grp_label_ = nullptr;
	QComboBox *cmb_label_display_ = nullptr;
	QComboBox *cmb_label_position_ = nullptr;
	QPushButton *btn_label_font_ = nullptr;
	QSpinBox *spin_label_font_size_ = nullptr;
	QComboBox *cmb_label_scale_mode_ = nullptr;
	QSpinBox *spin_label_min_font_ = nullptr;
	QSpinBox *spin_label_max_font_ = nullptr;
	QLineEdit *edit_label_text_color_ = nullptr;
	QDoubleSpinBox *spin_label_bg_opacity_ = nullptr;
	QCheckBox *chk_label_bg_rounded_ = nullptr;
	QSpinBox *spin_label_margin_ = nullptr;

	/* Safe Area group */
	QGroupBox *grp_safe_area_ = nullptr;
	QCheckBox *chk_safe_area_enabled_ = nullptr;
	QComboBox *cmb_safe_area_anchor_ = nullptr;
	QLineEdit *edit_safe_area_color_ = nullptr;
	QDoubleSpinBox *spin_safe_area_opacity_ = nullptr;

	/* VU Meter group */
	QGroupBox *grp_vu_meter_ = nullptr;
	QCheckBox *chk_vu_enabled_ = nullptr;
	QComboBox *cmb_vu_position_ = nullptr;
	QComboBox *cmb_vu_anchor_ = nullptr;
	QSpinBox *spin_vu_width_ = nullptr;
	QDoubleSpinBox *spin_vu_opacity_ = nullptr;
	QDoubleSpinBox *spin_vu_length_ratio_ = nullptr;
	QComboBox *cmb_vu_alignment_ = nullptr;
	QDoubleSpinBox *spin_vu_warning_db_ = nullptr;
	QDoubleSpinBox *spin_vu_error_db_ = nullptr;
	QComboBox *cmb_vu_decay_ = nullptr;
	QCheckBox *chk_vu_flip_ = nullptr;
	QComboBox *cmb_vu_track_mode_ = nullptr;
	QSpinBox *spin_vu_manual_track_ = nullptr;
	/* Peak Hold */
	QCheckBox *chk_vu_peak_hold_ = nullptr;
	QSpinBox *spin_vu_peak_hold_ms_ = nullptr;
	QDoubleSpinBox *spin_vu_peak_hold_decay_ = nullptr;
	QSpinBox *spin_vu_peak_hold_width_ = nullptr;
	/* dB Scale */
	QCheckBox *chk_vu_scale_ = nullptr;
	QLineEdit *edit_vu_scale_ticks_ = nullptr;
	QCheckBox *chk_vu_scale_labels_ = nullptr;
	QComboBox *cmb_vu_scale_side_ = nullptr;

	/* Overlay group */
	QGroupBox *grp_overlay_ = nullptr;
	QCheckBox *chk_overlay_enabled_ = nullptr;
	QLineEdit *edit_overlay_path_ = nullptr;
	QDoubleSpinBox *spin_overlay_opacity_ = nullptr;
	QComboBox *cmb_overlay_fit_ = nullptr;
	QComboBox *cmb_overlay_anchor_ = nullptr;

	/* Highlight group (PGM/PRVW cell borders).
	 *
	 * Highlight is intentionally a window-wide concept driven by the OBS
	 * frontend scene tree, so this group is editable only in Global and
	 * Instance scopes. In Cell scope the group is force-disabled and
	 * accompanied by an explanatory label. */
	QGroupBox *grp_highlight_ = nullptr;
	QLabel *lbl_highlight_cell_note_ = nullptr;
	QCheckBox *chk_highlight_enabled_ = nullptr;
	QLineEdit *edit_highlight_pgm_color_ = nullptr;
	QLineEdit *edit_highlight_prvw_color_ = nullptr;
	QCheckBox *chk_highlight_nested_dashed_ = nullptr;
	QSpinBox *spin_highlight_dash_length_ = nullptr;
	QSpinBox *spin_highlight_dash_gap_ = nullptr;
	QSpinBox *spin_highlight_min_thickness_ = nullptr;

	/* Copy/Paste/Reset */
	static QByteArray s_clipboard_; /* shared clipboard for copy/paste */
	void on_copy();
	void on_paste();
	void on_reset();
};
