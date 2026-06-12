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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QWidget>

#include <string>
#include <vector>

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

	/* Form fields. ffmpeg_source's setting keys are persisted verbatim
	 * (see provider-settings-forms.cpp). The form is flat \u2014 no Advanced
	 * foldout \u2014 and per-mode visibility hides keys that don't apply to
	 * the current mode. Keys we hard-lock in FfmpegProvider
	 * (restart_on_activate / close_when_inactive, plus seekable in any
	 * mode and looping/speed_percent in network mode) are NOT in this
	 * form; users never see them and cannot break activation. */
	QCheckBox *chk_local_file_ = nullptr;
	QLineEdit *url_edit_ = nullptr;
	QLineEdit *local_path_edit_ = nullptr;
	QToolButton *local_browse_btn_ = nullptr;
	QLabel *lbl_reconnect_delay_ = nullptr;
	QSpinBox *spin_reconnect_delay_ = nullptr;
	QSpinBox *spin_buffering_mb_ = nullptr;
	QCheckBox *chk_hw_decode_ = nullptr;
	QComboBox *cmb_color_range_ = nullptr;
	QCheckBox *chk_clear_on_end_ = nullptr;
	QCheckBox *chk_looping_ = nullptr;
	QLabel *lbl_speed_percent_ = nullptr;
	QSpinBox *spin_speed_percent_ = nullptr;
	QCheckBox *chk_linear_alpha_ = nullptr;
	QLabel *lbl_ffmpeg_options_ = nullptr;
	QLineEdit *ffmpeg_options_edit_ = nullptr;
};
/* Form for the DistroAV NDI provider (Phase 3 / M6.2).
 *
 * UI shape mirrors OBS's own ndi_source properties dialog for the keys
 * that matter on a multiview cell: source name (with live discovery +
 * Refresh + manual fallback), bandwidth mode, audio, latency,
 * framesync, hardware acceleration. Advanced DistroAV keys
 * (yuv_range / yuv_colorspace / behavior / behavior_timeout /
 * fix_alpha) stay at the provider defaults; round-tripping a config
 * edited by hand still preserves them.
 *
 * Discovery is fully driven by signal_provider_ndi_discover_sources()
 * which talks to DistroAV's NDIFinder via a long-lived dormant
 * ndi_source probe. The form just renders the returned list. */
class NdiSourceForm : public QWidget {
	Q_OBJECT
public:
	explicit NdiSourceForm(QWidget *parent = nullptr);

	void load_from(const SignalConfig &cfg);
	SignalConfig to_signal_config() const;
	bool is_valid() const;
	QString invalid_reason() const;

public slots:
	void refresh_discovery();

private:
	void update_resolved_name(const QString &name);

	QListWidget *discovered_list_ = nullptr;
	QPushButton *refresh_btn_ = nullptr;
	QLineEdit *manual_name_edit_ = nullptr;
	QLabel *resolved_label_ = nullptr;
	QComboBox *cmb_behavior_ = nullptr;
	QComboBox *cmb_bandwidth_ = nullptr;
	QComboBox *cmb_sync_ = nullptr;
	QComboBox *cmb_latency_ = nullptr;
	QComboBox *cmb_yuv_range_ = nullptr;
	QComboBox *cmb_yuv_colorspace_ = nullptr;
	QCheckBox *chk_audio_ = nullptr;
	QCheckBox *chk_framesync_ = nullptr;
	QCheckBox *chk_hw_accel_ = nullptr;
	QCheckBox *chk_fix_alpha_ = nullptr;
};

/* Discovery hook implemented in signal-provider-ndi.cpp. Returns the
 * NDI source names currently visible on the LAN, sorted and deduped.
 * Empty when DistroAV is not installed. Safe to call from the UI
 * thread; under the hood it triggers DistroAV's NDIFinder async refresh
 * via a private dormant ndi_source. */
std::vector<std::string> signal_provider_ndi_discover_sources();