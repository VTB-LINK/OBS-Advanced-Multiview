/*
OBS Advanced Multiview - FFmpeg media provider settings form (Phase 3 / M6.1)

Split out of provider-settings-forms.cpp for maintainability. The form's
class declaration lives in provider-settings-forms.hpp; this TU only
implements its members.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "provider-settings-forms.hpp"
#include "amv-i18n.hpp"
#include "provider-settings-forms-common.hpp"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

using mv_forms::set_or_default_bool;
using mv_forms::set_or_default_int;
using mv_forms::set_or_default_string;

/* ---------- FfmpegMediaForm ---------- */

namespace {

/* Defaults pinned to the M6.1 docs/ROADMAP.md values so a fresh form matches the
 * canonical first-slice config. Persistence keys mirror obs-ffmpeg-source.c
 * verbatim so providerSettings can be passed straight to
 * obs_source_create_private without translation. */
constexpr int kDefaultReconnectDelaySec = 10;
constexpr int kDefaultBufferingMb = 2;
constexpr int kDefaultSpeedPercent = 100;

} // namespace

FfmpegMediaForm::FfmpegMediaForm(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(8);

	/* Phase 3 / M6.1+ post-9.1.B: flat layout (no Advanced foldout).
	 * Mirrors OBS's own ffmpeg_source properties dialog: pick a mode at
	 * the top, the rows that don't apply to that mode are hidden. Keys
	 * we hard-lock in FfmpegProvider::create_private_source are NOT
	 * shown at all (restart_on_activate, close_when_inactive, plus
	 * looping/seekable/speed_percent in network mode) so users don't
	 * see disabled controls and wonder why. */

	auto *form = new QFormLayout();

	chk_local_file_ = new QCheckBox(amv::text("AMVPlugin.Provider.FFmpeg.LocalFile"), this);
	chk_local_file_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.LocalFileTooltip"));
	form->addRow(QString(), chk_local_file_);

	url_edit_ = new QLineEdit(this);
	url_edit_->setPlaceholderText(amv::text("AMVPlugin.Provider.FFmpeg.UrlPlaceholder"));
	url_edit_->setClearButtonEnabled(true);
	form->addRow(amv::text("AMVPlugin.Provider.FFmpeg.Url"), url_edit_);

	auto *local_row = new QHBoxLayout();
	local_path_edit_ = new QLineEdit(this);
	local_path_edit_->setPlaceholderText(amv::text("AMVPlugin.Provider.FFmpeg.LocalPathPlaceholder"));
	local_path_edit_->setClearButtonEnabled(true);
	local_row->addWidget(local_path_edit_, 1);
	local_browse_btn_ = new QToolButton(this);
	local_browse_btn_->setText(QStringLiteral("..."));
	local_browse_btn_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.BrowseLocalTooltip"));
	local_row->addWidget(local_browse_btn_);
	auto *local_row_widget = new QWidget(this);
	local_row_widget->setLayout(local_row);
	form->addRow(amv::text("AMVPlugin.Provider.FFmpeg.LocalFilePath"), local_row_widget);

	/* Reconnect delay is network-only. Local files don't need a retry
	 * interval \u2014 if a local file fails to open, retrying every N seconds
	 * doesn't help (the file isn't going to materialize). */
	spin_reconnect_delay_ = new QSpinBox(this);
	spin_reconnect_delay_->setRange(1, 60);
	spin_reconnect_delay_->setSuffix(QStringLiteral(" s"));
	spin_reconnect_delay_->setValue(kDefaultReconnectDelaySec);
	spin_reconnect_delay_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.ReconnectDelayTooltip"));
	lbl_reconnect_delay_ = new QLabel(amv::text("AMVPlugin.Provider.FFmpeg.ReconnectDelay"), this);
	form->addRow(lbl_reconnect_delay_, spin_reconnect_delay_);

	spin_buffering_mb_ = new QSpinBox(this);
	spin_buffering_mb_->setRange(0, 16);
	spin_buffering_mb_->setSuffix(QStringLiteral(" MB"));
	spin_buffering_mb_->setValue(kDefaultBufferingMb);
	spin_buffering_mb_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.BufferingTooltip"));
	form->addRow(amv::text("AMVPlugin.Provider.FFmpeg.Buffering"), spin_buffering_mb_);

	chk_hw_decode_ = new QCheckBox(amv::text("AMVPlugin.Provider.FFmpeg.HwDecode"), this);
	chk_hw_decode_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.HwDecodeTooltip"));
	form->addRow(QString(), chk_hw_decode_);

	cmb_color_range_ = new QComboBox(this);
	cmb_color_range_->addItem(amv::text("AMVPlugin.Common.Auto"), 0);
	cmb_color_range_->addItem(amv::text("AMVPlugin.Provider.Common.RangePartial"), 1);
	cmb_color_range_->addItem(amv::text("AMVPlugin.Provider.Common.RangeFull"), 2);
	cmb_color_range_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.ColorRangeTooltip"));
	form->addRow(amv::text("AMVPlugin.Provider.FFmpeg.ColorRange"), cmb_color_range_);

	chk_clear_on_end_ = new QCheckBox(amv::text("AMVPlugin.Provider.FFmpeg.ClearOnEnd"), this);
	chk_clear_on_end_->setChecked(true);
	chk_clear_on_end_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.ClearOnEndTooltip"));
	form->addRow(QString(), chk_clear_on_end_);

	chk_looping_ = new QCheckBox(amv::text("AMVPlugin.Provider.FFmpeg.Loop"), this);
	chk_looping_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.LoopTooltip"));
	form->addRow(QString(), chk_looping_);

	spin_speed_percent_ = new QSpinBox(this);
	spin_speed_percent_->setRange(1, 200);
	spin_speed_percent_->setSuffix(QStringLiteral(" %"));
	spin_speed_percent_->setValue(kDefaultSpeedPercent);
	spin_speed_percent_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.SpeedTooltip"));
	lbl_speed_percent_ = new QLabel(amv::text("AMVPlugin.Provider.FFmpeg.Speed"), this);
	form->addRow(lbl_speed_percent_, spin_speed_percent_);

	chk_linear_alpha_ = new QCheckBox(amv::text("AMVPlugin.Provider.FFmpeg.LinearAlpha"), this);
	chk_linear_alpha_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.LinearAlphaTooltip"));
	form->addRow(QString(), chk_linear_alpha_);

	ffmpeg_options_edit_ = new QLineEdit(this);
	ffmpeg_options_edit_->setPlaceholderText(amv::text("AMVPlugin.Provider.FFmpeg.OptionsPlaceholder"));
	ffmpeg_options_edit_->setToolTip(amv::text("AMVPlugin.Provider.FFmpeg.OptionsTooltip"));
	lbl_ffmpeg_options_ = new QLabel(amv::text("AMVPlugin.Provider.FFmpeg.Options"), this);
	form->addRow(lbl_ffmpeg_options_, ffmpeg_options_edit_);

	root->addLayout(form);

	/* Wire visibility + browse */
	connect(chk_local_file_, &QCheckBox::toggled, this, &FfmpegMediaForm::on_local_file_toggled);
	connect(local_browse_btn_, &QToolButton::clicked, this, &FfmpegMediaForm::on_browse_local_file);

	apply_local_visibility(false);
}

void FfmpegMediaForm::apply_local_visibility(bool is_local)
{
	/* Mode selector + URL/path swap */
	url_edit_->setVisible(!is_local);
	local_path_edit_->setVisible(is_local);
	local_browse_btn_->setVisible(is_local);

	/* Reconnect delay + Network buffering: network only.
	 * Reconnect_delay_sec for a local file doesn't make sense \u2014 if the
	 * file fails to open, retrying every N seconds doesn't help. */
	spin_reconnect_delay_->setVisible(!is_local);
	spin_buffering_mb_->setVisible(!is_local);

	/* Loop + Speed: local only. The provider locks looping=false and
	 * speed_percent=100 for network mode, so showing them disabled in
	 * network mode would just add noise. */
	chk_looping_->setVisible(is_local);
	spin_speed_percent_->setVisible(is_local);

	/* QFormLayout doesn't auto-hide labels for hidden field widgets; we
	 * walk the layout once to flip label visibility paired with each
	 * field that just got hidden. */
	if (auto *form = qobject_cast<QFormLayout *>(layout()->itemAt(0)->layout())) {
		for (int i = 0; i < form->rowCount(); i++) {
			auto *labelItem = form->itemAt(i, QFormLayout::LabelRole);
			auto *fieldItem = form->itemAt(i, QFormLayout::FieldRole);
			if (!labelItem || !fieldItem)
				continue;
			QWidget *fw = fieldItem->widget();
			if (!fw && fieldItem->layout()) {
				/* The local-file row uses a layout container.
				 * Find the line edit inside it so the row tracks
				 * local_path_edit_'s visibility. */
				if (fieldItem->layout()->indexOf(local_path_edit_) >= 0)
					fw = local_path_edit_;
			}
			if (!fw)
				continue;
			QWidget *lw = labelItem->widget();
			if (!lw)
				continue;
			lw->setVisible(fw->isVisibleTo(this));
		}
	}
}

void FfmpegMediaForm::on_local_file_toggled(bool checked)
{
	apply_local_visibility(checked);
}

void FfmpegMediaForm::on_browse_local_file()
{
	const QString picked = QFileDialog::getOpenFileName(this,
							    amv::text("AMVPlugin.Provider.FFmpeg.SelectMediaFile"),
							    local_path_edit_->text(),
							    amv::text("AMVPlugin.Provider.FFmpeg.MediaFileFilter"));
	if (!picked.isEmpty())
		local_path_edit_->setText(picked);
}

void FfmpegMediaForm::load_from(const SignalConfig &cfg)
{
	obs_data_t *src = cfg.providerSettings;

	bool is_local = src ? obs_data_get_bool(src, "is_local_file") : false;
	chk_local_file_->setChecked(is_local);

	url_edit_->setText(src ? QString::fromUtf8(obs_data_get_string(src, "input")) : QString());
	local_path_edit_->setText(src ? QString::fromUtf8(obs_data_get_string(src, "local_file")) : QString());

	if (src && obs_data_has_user_value(src, "reconnect_delay_sec"))
		spin_reconnect_delay_->setValue((int)obs_data_get_int(src, "reconnect_delay_sec"));
	else
		spin_reconnect_delay_->setValue(kDefaultReconnectDelaySec);

	if (src && obs_data_has_user_value(src, "buffering_mb"))
		spin_buffering_mb_->setValue((int)obs_data_get_int(src, "buffering_mb"));
	else
		spin_buffering_mb_->setValue(kDefaultBufferingMb);

	chk_hw_decode_->setChecked(src ? obs_data_get_bool(src, "hw_decode") : false);

	int color_range = src ? (int)obs_data_get_int(src, "color_range") : 0;
	{
		int idx = cmb_color_range_->findData(color_range);
		cmb_color_range_->setCurrentIndex(idx >= 0 ? idx : 0);
	}

	chk_looping_->setChecked(src ? obs_data_get_bool(src, "looping") : false);

	if (src && obs_data_has_user_value(src, "clear_on_media_end"))
		chk_clear_on_end_->setChecked(obs_data_get_bool(src, "clear_on_media_end"));
	else
		chk_clear_on_end_->setChecked(true);
	chk_linear_alpha_->setChecked(src ? obs_data_get_bool(src, "linear_alpha") : false);

	if (src && obs_data_has_user_value(src, "speed_percent"))
		spin_speed_percent_->setValue((int)obs_data_get_int(src, "speed_percent"));
	else
		spin_speed_percent_->setValue(kDefaultSpeedPercent);

	ffmpeg_options_edit_->setText(src ? QString::fromUtf8(obs_data_get_string(src, "ffmpeg_options")) : QString());

	apply_local_visibility(is_local);
}

bool FfmpegMediaForm::is_valid() const
{
	if (chk_local_file_->isChecked())
		return !local_path_edit_->text().trimmed().isEmpty();
	return !url_edit_->text().trimmed().isEmpty();
}

QString FfmpegMediaForm::invalid_reason() const
{
	if (is_valid())
		return QString();
	return chk_local_file_->isChecked() ? amv::text("AMVPlugin.Provider.FFmpeg.ErrorLocalFileRequired")
					    : amv::text("AMVPlugin.Provider.FFmpeg.ErrorUrlRequired");
}

SignalConfig FfmpegMediaForm::to_signal_config() const
{
	SignalConfig cfg;
	if (!is_valid())
		return cfg;

	cfg.provider = SignalProviderType::Ffmpeg;
	const bool is_local = chk_local_file_->isChecked();
	const QString url = url_edit_->text().trimmed();
	const QString lpath = local_path_edit_->text().trimmed();

	cfg.providerSettings = obs_data_create();
	obs_data_t *d = cfg.providerSettings;

	/* Phase 3 / M6.1+ post-9.1.B fix: ALWAYS write is_local_file
	 * explicitly. ffmpeg_source's get_defaults sets is_local_file=true
	 * by default, so omitting the key on a network URL makes the
	 * source try to open the URL as a local file path and fail
	 * silently. The original M6.1 first slice (4c6052c) wrote both
	 * mode flags unconditionally; the form regression hid the value
	 * in network mode. */
	obs_data_set_bool(d, "is_local_file", is_local);

	if (is_local) {
		obs_data_set_string(d, "local_file", lpath.toUtf8().constData());
		cfg.displayName = lpath.toStdString();
	} else {
		obs_data_set_string(d, "input", url.toUtf8().constData());
		cfg.displayName = url.toStdString();
	}

	/* Network-only keys: only persist when in network mode so a future
	 * local-file edit doesn't carry stale buffering values. The provider
	 * re-applies sane defaults if missing. */
	if (!is_local) {
		set_or_default_int(d, "reconnect_delay_sec", spin_reconnect_delay_->value(), kDefaultReconnectDelaySec);
		set_or_default_int(d, "buffering_mb", spin_buffering_mb_->value(), kDefaultBufferingMb);
	}

	set_or_default_bool(d, "hw_decode", chk_hw_decode_->isChecked(), false);

	int color_range = cmb_color_range_->currentData().toInt();
	set_or_default_int(d, "color_range", color_range, 0);

	set_or_default_bool(d, "clear_on_media_end", chk_clear_on_end_->isChecked(), true);
	set_or_default_bool(d, "linear_alpha", chk_linear_alpha_->isChecked(), false);
	set_or_default_string(d, "ffmpeg_options", ffmpeg_options_edit_->text(), "");

	/* Local-only keys: only persist when in local mode. The provider
	 * force-clears looping=false / seekable=false / speed_percent=100
	 * for network mode regardless of what's persisted, but keeping the
	 * JSON clean prevents user confusion when inspecting settings.json. */
	if (is_local) {
		set_or_default_bool(d, "looping", chk_looping_->isChecked(), false);
		set_or_default_int(d, "speed_percent", spin_speed_percent_->value(), kDefaultSpeedPercent);
	}

	return cfg;
}
