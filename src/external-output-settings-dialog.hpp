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
	/* Per-backend control bundle (one set per tab). The Spout/NDI tabs use the
	 * resMode/customW/customH/fps controls; the DeckLink tab uses the deck*
	 * controls instead (its resolution is locked to the selected mode's raster)
	 * and leaves the former null. */
	struct BackendWidgets {
		QCheckBox *enabled = nullptr;
		QComboBox *resMode = nullptr;
		QSpinBox *customW = nullptr;
		QSpinBox *customH = nullptr;
		QComboBox *fps = nullptr;
		QComboBox *audioMode = nullptr;
		QSpinBox *audioTrack = nullptr;

		/* DeckLink-only (issue #16). */
		QComboBox *deckDevice = nullptr; /* data = device_hash (QString) */
		QComboBox *deckMode = nullptr;   /* data = mode_id (qlonglong) */
		QComboBox *deckKeyer = nullptr;  /* data = 0/1/2 */
		QCheckBox *deckForceSdr = nullptr;
	};

	void setup_ui();
	/* Builds one backend tab. `available` false => whole tab disabled with a
	 * tooltip (non-Windows Spout, or NDI runtime missing). `supportsAudio`
	 * false => the audio controls are present but disabled (Spout has no
	 * audio path). */
	QWidget *build_backend_tab(BackendWidgets &w, bool available, const QString &unavailableReason,
				   bool supportsAudio);
	/* Builds the DeckLink tab (device/mode/keyer/forceSdr + audio; no
	 * resMode/custom/fps — the mode fixes the raster). */
	QWidget *build_decklink_tab(BackendWidgets &w, bool available, const QString &unavailableReason);
	/* Repopulate the mode list for the currently selected device, filtered by
	 * the canvas fps (via the OBS decklink_output property callback). */
	static void populate_decklink_modes(const BackendWidgets &w, const QString &deviceHash);
	static void load_backend(const BackendWidgets &w, const OutputBackendSettings &s);
	static OutputBackendSettings read_backend(const BackendWidgets &w);
	static void load_decklink(const BackendWidgets &w, const OutputBackendSettings &s);
	static OutputBackendSettings read_decklink(const BackendWidgets &w);

	BackendWidgets spout_;
	BackendWidgets ndi_;
	BackendWidgets decklink_;
};
