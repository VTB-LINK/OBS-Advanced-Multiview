/*
OBS Advanced Multiview - Edit Source dialog (Phase 3 / M6.1+)

Modal dialog used by the cell context-menu's "Edit Source..." entry on
external-provider cells. Lets the user re-configure provider settings
without going through the SourcePicker flow.

Internal cells (PGM/PRVW/Scene/Source) do not use this dialog \u2014 their
context menu still offers Change Source... which reopens SourcePicker.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "multiview-instance.hpp"

#include <QDialog>

class FfmpegMediaForm;

class EditSourceDialog : public QDialog {
	Q_OBJECT
public:
	/* Construct the dialog populated from `cfg`. The caller is responsible
	 * for showing it modally with exec() and then reading the new config
	 * via signal_config() if exec() returned QDialog::Accepted. */
	EditSourceDialog(const SignalConfig &cfg, QWidget *parent = nullptr);

	/* Build a SignalConfig from the dialog state. Only valid after exec()
	 * returns Accepted; on Cancel/Close the cell's existing config should
	 * stay untouched (the caller never reads this in that case). */
	SignalConfig signal_config() const;

private slots:
	void on_accept();

private:
	SignalProviderType provider_ = SignalProviderType::Unknown;
	FfmpegMediaForm *ffmpeg_form_ = nullptr;
};
