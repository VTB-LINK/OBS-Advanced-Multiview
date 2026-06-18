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
	/* Issue #10: first-frame load timeout (network-only). */
	QCheckBox *chk_first_frame_timeout_ = nullptr;
	QLabel *lbl_first_frame_timeout_ = nullptr;
	QSpinBox *spin_first_frame_timeout_ = nullptr;
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
 * behavior, timeout behavior, framesync, hardware acceleration,
 * yuv_range / yuv_colorspace, and alpha fix.
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
	QComboBox *cmb_timeout_ = nullptr;
	QComboBox *cmb_bandwidth_ = nullptr;
	QComboBox *cmb_sync_ = nullptr;
	QComboBox *cmb_latency_ = nullptr;
	QComboBox *cmb_yuv_range_ = nullptr;
	QComboBox *cmb_yuv_colorspace_ = nullptr;
	QCheckBox *chk_audio_ = nullptr;
	QCheckBox *chk_framesync_ = nullptr;
	QCheckBox *chk_hw_accel_ = nullptr;
	QCheckBox *chk_fix_alpha_ = nullptr;

	/* Tracks the user's currently chosen NDI source name across
	 * Refresh cycles. Lets refresh_discovery() (a) restore the
	 * selection when the named source reappears, and (b) inject a
	 * "signal lost" placeholder row when the named source is
	 * temporarily missing from the LAN. Stored without any
	 * suffix — the bare ndi_source_name string. */
	QString remembered_selection_;
};

/* Discovery hook implemented in signal-provider-ndi.cpp. Returns the
 * NDI source names currently visible on the LAN, sorted and deduped.
 * Empty when DistroAV is not installed. Safe to call from the UI
 * thread; under the hood it triggers DistroAV's NDIFinder async refresh
 * via a private dormant ndi_source. */
std::vector<std::string> signal_provider_ndi_discover_sources();

/* Form for the obs-spout2 Spout provider (Phase 3 / M6.3).
 *
 * UI shape mirrors OBS's own spout_capture properties dialog: a
 * "first available" checkbox toggling between auto-pick and a
 * specific sender from the discovered list, plus composite mode
 * and tick speed. Spout has no audio so there's no audio toggle.
 *
 * Discovery is fully driven by signal_provider_spout_discover_senders()
 * which talks to obs-spout2 via a long-lived dormant spout_capture
 * probe. The form just renders the returned list. */
class SpoutSenderForm : public QWidget {
	Q_OBJECT
public:
	explicit SpoutSenderForm(QWidget *parent = nullptr);

	void load_from(const SignalConfig &cfg);
	SignalConfig to_signal_config() const;
	bool is_valid() const;
	QString invalid_reason() const;

public slots:
	void refresh_discovery();

private:
	void apply_first_available_visibility();

	QCheckBox *chk_first_available_ = nullptr;
	QListWidget *discovered_list_ = nullptr;
	QPushButton *refresh_btn_ = nullptr;
	QComboBox *cmb_composite_mode_ = nullptr;
	QComboBox *cmb_tick_speed_ = nullptr;

	/* Same role as NdiSourceForm::remembered_selection_: keeps the
	 * user's chosen sender across Refresh so a temporarily
	 * disappeared sender stays selected with a "signal lost"
	 * placeholder until it returns. Empty when first-available is
	 * checked or the user hasn't picked anything yet. */
	QString remembered_selection_;
};

/* Discovery hook implemented in signal-provider-spout.cpp. Returns
 * the Spout sender names currently registered on the local machine,
 * sorted and deduped, excluding the synthetic "first-available" token.
 * Empty when obs-spout2 is not installed. */
std::vector<std::string> signal_provider_spout_discover_senders();

/* Form for the OBS built-in vlc_source provider (Phase 3 / M6.4).
 *
 * VLC has no discovery surface (playlist entries are user-supplied
 * paths/URLs), so the form is a simple playlist editor + a handful of
 * playback knobs that mirror what OBS's own VLC source dialog
 * exposes: loop, behavior (stop_restart / pause_unpause /
 * always_play), network_caching, audio track index.
 *
 * Persisted JSON shape is identical to OBS's vlc_source so the
 * resulting providerSettings could in principle be moved into a real
 * OBS scene without translation. */
class VlcMediaForm : public QWidget {
	Q_OBJECT
public:
	explicit VlcMediaForm(QWidget *parent = nullptr);

	void load_from(const SignalConfig &cfg);
	SignalConfig to_signal_config() const;
	bool is_valid() const;
	QString invalid_reason() const;

private:
	/* Each playlist row's text() is the path/URL; we don't store a
	 * separate UserRole copy because the form has no "selected"
	 * semantics across edits (the whole list is the value). */
	QListWidget *playlist_list_ = nullptr;
	QPushButton *btn_add_file_ = nullptr;
	QPushButton *btn_add_files_ = nullptr;
	QPushButton *btn_add_url_ = nullptr;
	QPushButton *btn_edit_ = nullptr;
	QPushButton *btn_remove_ = nullptr;
	QPushButton *btn_clear_ = nullptr;

	QCheckBox *chk_loop_ = nullptr;
	QCheckBox *chk_shuffle_ = nullptr;
	QComboBox *cmb_behavior_ = nullptr;
	QSpinBox *spin_network_caching_ = nullptr;
	QSpinBox *spin_track_ = nullptr;
	/* Issue #10: first-frame load timeout (always shown — a VLC playlist may
	 * contain network items, which we can't reliably detect). */
	QCheckBox *chk_first_frame_timeout_ = nullptr;
	QLabel *lbl_first_frame_timeout_ = nullptr;
	QSpinBox *spin_first_frame_timeout_ = nullptr;
};
