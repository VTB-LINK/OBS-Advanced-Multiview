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

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QWidget>

class ProviderSettingsForm;

class SourcePicker : public QDialog {
	Q_OBJECT

public:
	explicit SourcePicker(QWidget *parent = nullptr);

	CellAssignment result_assignment() const { return result_; }

private slots:
	void on_filter_changed(const QString &text);
	void on_item_double_clicked(QListWidgetItem *item);
	void on_accept();

private:
	void populate_list();

	/* Phase 3 / M6: build a placeholder tab widget for an external
	 * provider whose runtime has not landed yet (or whose host plugin
	 * is missing). The placeholder reuses real-shape controls so future
	 * milestones replace it without disturbing the rest of the dialog.
	 *
	 * `provider` selects the SignalProviderRegistry entry queried for
	 * availability; `coming_in` is the milestone label ("M6.1" etc.);
	 * `description` is the body text shown above the disabled controls. */
	QWidget *build_external_placeholder(SignalProviderType provider, const QString &coming_in,
					    const QString &description);

	/* Phase 3 / M6.1: real Media tab — user enters an FFmpeg-accepted
	 * URL and on accept the dialog returns a CellAssignment whose
	 * signalConfig is populated with provider=Ffmpeg and the URL stored
	 * under the `input` key. */
	QWidget *build_media_tab();

	/* Phase 3 / M6.2: real NDI tab — discovered list + Refresh button
	 * + manual fallback + bandwidth/latency/audio/framesync/hw-accel
	 * controls. Returns a CellAssignment with provider=Ndi when the
	 * user picks a source. */
	QWidget *build_ndi_tab();

	/* Phase 3 / M6.3: real Spout tab — first-available checkbox +
	 * discovered sender list + composite mode + tick speed. Returns a
	 * CellAssignment with provider=Spout. */
	QWidget *build_spout_tab();

	/* Phase 3 / M6.4: real VLC tab — playlist editor (files + URLs) +
	 * loop / shuffle / behavior / network_caching / track. Returns a
	 * CellAssignment with provider=Vlc. */
	QWidget *build_vlc_tab();

	QTabWidget *tabs_;
	QLineEdit *filter_edit_;
	QListWidget *special_list_;
	QListWidget *scene_list_;
	QListWidget *source_list_;

	/* Phase 3 / M6: external provider placeholder tabs. Stored so the
	 * tab index lookup in on_accept() can recognize them and reject
	 * politely until the actual provider implementations land. */
	QWidget *media_tab_ = nullptr;
	QWidget *ndi_tab_ = nullptr;
	QWidget *spout_tab_ = nullptr;
	QWidget *vlc_tab_ = nullptr;
	QWidget *webrtc_tab_ = nullptr;

	/* Phase 3 / M6.1: Media tab URL line edit (kept as a member so
	 * on_accept can read its value). Null until build_media_tab runs. */
	QLineEdit *media_url_edit_ = nullptr;

	/* Per-tab provider settings forms. Each is created via
	 * make_provider_settings_form() and owned by its tab page; lifetime
	 * ends with the dialog. Typed as the common base so on_accept()
	 * validates and reads them back uniformly — the concrete type is
	 * chosen only in the factory.
	 *
	 * Phase 3 / M6.1+ task 9.1.B: full ffmpeg parity form (Media tab). */
	ProviderSettingsForm *media_form_ = nullptr;

	/* Phase 3 / M6.2: NDI form (NDI tab). */
	ProviderSettingsForm *ndi_form_ = nullptr;

	/* Phase 3 / M6.3: Spout form (Spout tab). */
	ProviderSettingsForm *spout_form_ = nullptr;

	/* Phase 3 / M6.4: VLC form (VLC tab). */
	ProviderSettingsForm *vlc_form_ = nullptr;

	CellAssignment result_;
};
