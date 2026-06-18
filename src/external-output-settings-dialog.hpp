/*
OBS Advanced Multiview - External Output (Spout/NDI) settings dialog (issue #11)

A small multi-tab dialog (one tab per output backend, like the source picker)
to configure per-backend enable / resolution / frame-rate. Edits an
InstanceOutputSettings value; the caller persists it and re-applies.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "multiview-instance.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QSpinBox;
class QWidget;

class ExternalOutputSettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit ExternalOutputSettingsDialog(QWidget *parent = nullptr);

	void set_settings(const InstanceOutputSettings &s);
	InstanceOutputSettings get_settings() const;

private:
	/* Per-backend control bundle (one set per tab). */
	struct BackendWidgets {
		QCheckBox *enabled = nullptr;
		QComboBox *resMode = nullptr;
		QSpinBox *customW = nullptr;
		QSpinBox *customH = nullptr;
		QComboBox *fps = nullptr;
		QComboBox *audioMode = nullptr;
		QSpinBox *audioTrack = nullptr;
	};

	void setup_ui();
	/* Builds one backend tab. `available` false => whole tab disabled with a
	 * tooltip (non-Windows Spout, or NDI runtime missing). `supportsAudio`
	 * false => the audio controls are present but disabled (Spout has no
	 * audio path). */
	QWidget *build_backend_tab(BackendWidgets &w, bool available, const QString &unavailableReason,
				   bool supportsAudio);
	static void load_backend(const BackendWidgets &w, const OutputBackendSettings &s);
	static OutputBackendSettings read_backend(const BackendWidgets &w);

	BackendWidgets spout_;
	BackendWidgets ndi_;
};
