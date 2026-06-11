/*
OBS Advanced Multiview - Provider settings form widgets (Phase 3 / M6.1+)

Reusable Qt form widgets that read / write SignalConfig.providerSettings
for each external provider. Used by:

  - SourcePicker tabs (the user picks a provider and configures it).
  - Edit Source... context menu (the user re-configures a cell already
    bound to an external provider without picking again).

This file only declares forms for the providers we actually expose to
the user. NDI / Spout / VLC will add their own forms in their own
milestones (M6.2 / M6.3 / M6.4).

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "multiview-instance.hpp"

#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QToolButton>
#include <QWidget>

/* Form for the FFmpeg media provider.
 *
 * Models the full ffmpeg_source feature surface in two visual layers:
 *
 *   Common (always visible):
 *     - Local File checkbox        -> is_local_file
 *     - URL line edit              -> input          (network-only)
 *     - Local file picker          -> local_file     (local-only)
 *     - Reconnect Delay slider     -> reconnect_delay_sec  (network-only)
 *     - Network Buffering slider   -> buffering_mb         (network-only)
 *     - Hardware Decoding checkbox -> hw_decode
 *     - Color Range combo          -> color_range
 *     - Looping checkbox           -> looping              (local-only)
 *
 *   Advanced (collapsible QGroupBox, defaults collapsed):
 *     - Clear on media end checkbox     -> clear_on_media_end
 *     - Linear Alpha checkbox           -> linear_alpha
 *     - Seekable checkbox               -> seekable
 *     - Speed Percent slider            -> speed_percent
 *     - FFmpeg Options text             -> ffmpeg_options
 *
 * Two settings are NOT exposed because they conflict with our private-
 * source activation model:
 *     - restart_on_activate  (forced true; otherwise dec_active never
 *                             triggers reopen of the URL on Reconnect Now)
 *     - close_when_inactive  (forced false; otherwise the source closes
 *                             when not visible and the cell goes black)
 *
 * The provider's create_private_source is responsible for re-asserting
 * those locked values regardless of what's in providerSettings, so a
 * user editing the JSON by hand cannot break activation either.
 */
class FfmpegMediaForm : public QWidget {
	Q_OBJECT
public:
	explicit FfmpegMediaForm(QWidget *parent = nullptr);

	/* Populate the form from an existing SignalConfig (for Edit Source).
	 * Defaults are applied for keys not present in the data. */
	void load_from(const SignalConfig &cfg);

	/* Build a SignalConfig with provider == Ffmpeg, displayName from URL
	 * or local file path, and providerSettings populated from the form.
	 * Returns an empty SignalConfig() if the form is invalid (e.g.
	 * neither URL nor local file given). The caller should validate
	 * with `is_valid()` before calling this. */
	SignalConfig to_signal_config() const;

	/* Cheap validity check: at least one of input URL / local file path
	 * must be non-empty. */
	bool is_valid() const;

	/* User-visible reason when is_valid() returns false. Empty when
	 * valid. UI uses this in the "URL required" warning popup. */
	QString invalid_reason() const;

private slots:
	void on_local_file_toggled(bool checked);
	void on_browse_local_file();

private:
	void apply_local_visibility(bool is_local);

	/* Common section */
	QCheckBox *chk_local_file_ = nullptr;
	QLineEdit *url_edit_ = nullptr;
	QLineEdit *local_path_edit_ = nullptr;
	QToolButton *local_browse_btn_ = nullptr;
	QSpinBox *spin_reconnect_delay_ = nullptr;
	QSpinBox *spin_buffering_mb_ = nullptr;
	QCheckBox *chk_hw_decode_ = nullptr;
	QComboBox *cmb_color_range_ = nullptr;
	QCheckBox *chk_looping_ = nullptr;

	/* Advanced section */
	QGroupBox *grp_advanced_ = nullptr;
	QCheckBox *chk_clear_on_end_ = nullptr;
	QCheckBox *chk_linear_alpha_ = nullptr;
	QCheckBox *chk_seekable_ = nullptr;
	QSpinBox *spin_speed_percent_ = nullptr;
	QLineEdit *ffmpeg_options_edit_ = nullptr;
};
