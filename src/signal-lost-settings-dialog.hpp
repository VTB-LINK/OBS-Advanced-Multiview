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

#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QString>

/* Phase 3 / M5.2: Signal Lost Settings dialog.
 *
 * Two-layer model only (Global + Cell). Per the design document
 * [docs/phase-3-signal-lost-and-external-sources-design.md] §5/§9 we keep
 * this dialog completely separate from CellDisplaySettingsDialog so visual
 * config and runtime/strategy config don't bleed into each other.
 */
class SignalLostSettingsDialog : public QDialog {
	Q_OBJECT
public:
	enum class Mode {
		Global, /* edit project-wide default */
		Cell,   /* edit per-cell override */
	};

	explicit SignalLostSettingsDialog(Mode mode, QWidget *parent = nullptr);

	void set_cell_position(int row, int col);

	/* Global mode: payload is the LostSignalSettings struct directly. */
	void set_global_settings(const LostSignalSettings &s);
	LostSignalSettings get_global_settings() const;

	/* Cell mode: payload is the CellLostSignalSettings wrapper (mode + payload). */
	void set_cell_settings(const CellLostSignalSettings &c);
	CellLostSignalSettings get_cell_settings() const;

private slots:
	void on_inheritance_changed(int idx);
	void on_browse_placeholder_image();
	void on_browse_signal_lost_image();
	void on_fallback_type_changed(int idx);
	void on_browse_fallback_image();

private:
	void build_ui();
	void apply_settings(const LostSignalSettings &s);
	LostSignalSettings collect_settings() const;
	void update_enabled_state();

	const Mode mode_;
	int cell_row_ = -1;
	int cell_col_ = -1;

	/* Inheritance combo \u2014 only meaningful in Cell mode; hidden in Global. */
	QComboBox *cmb_inherit_ = nullptr;

	/* Internal missing behavior */
	QComboBox *cmb_internal_behavior_ = nullptr;
	QLineEdit *edit_placeholder_path_ = nullptr;
	QComboBox *cmb_placeholder_fit_ = nullptr;

	/* External lost behavior */
	QComboBox *cmb_external_behavior_ = nullptr;
	QLineEdit *edit_signal_lost_path_ = nullptr;
	QComboBox *cmb_signal_lost_fit_ = nullptr;

	/* Signal-Lost v2 axis A: recovery policy (auto-reconnect / manual-only). */
	QComboBox *cmb_recovery_policy_ = nullptr;

	/* Fallback assignment */
	QComboBox *cmb_fallback_type_ = nullptr;
	QLineEdit *edit_fallback_name_ = nullptr;
	QComboBox *cmb_fallback_image_fit_ = nullptr;

	/* Backoff timing */
	QSpinBox *spin_retry_initial_ = nullptr;
	QSpinBox *spin_retry_max_ = nullptr;
	QSpinBox *spin_manual_cooldown_ = nullptr;
};
