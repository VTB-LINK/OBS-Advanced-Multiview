/*
OBS Advanced Multiview - Provider settings form widget implementations

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "provider-settings-forms.hpp"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

/* ---------- FfmpegMediaForm ---------- */

namespace {

/* Defaults pinned to the M6.1 plan.md values so a fresh form matches the
 * canonical first-slice config. Persistence keys mirror obs-ffmpeg-source.c
 * verbatim so providerSettings can be passed straight to
 * obs_source_create_private without translation. */
constexpr int kDefaultReconnectDelaySec = 10;
constexpr int kDefaultBufferingMb = 2;
constexpr int kDefaultSpeedPercent = 100;

void set_or_default_int(obs_data_t *data, const char *key, int val, int def)
{
	if (val == def) {
		/* Don't bloat persisted JSON with default-valued keys. The
		 * provider re-applies defaults on create so missing keys
		 * resolve to the same value at runtime. */
		obs_data_unset_user_value(data, key);
	} else {
		obs_data_set_int(data, key, (long long)val);
	}
}

void set_or_default_bool(obs_data_t *data, const char *key, bool val, bool def)
{
	if (val == def)
		obs_data_unset_user_value(data, key);
	else
		obs_data_set_bool(data, key, val);
}

void set_or_default_string(obs_data_t *data, const char *key, const QString &val, const char *def)
{
	const std::string utf8 = val.toStdString();
	if (utf8 == (def ? def : "")) {
		obs_data_unset_user_value(data, key);
	} else {
		obs_data_set_string(data, key, utf8.c_str());
	}
}

} // namespace

FfmpegMediaForm::FfmpegMediaForm(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(8);

	/* ---------- Common ---------- */

	auto *common_form = new QFormLayout();

	chk_local_file_ = new QCheckBox(QStringLiteral("Local file"), this);
	chk_local_file_->setToolTip(
		QStringLiteral("Switch between a network URL (RTMP / HLS / FLV / SRT / http) and a local media file."));
	common_form->addRow(QString(), chk_local_file_);

	url_edit_ = new QLineEdit(this);
	url_edit_->setPlaceholderText(QStringLiteral("e.g. https://example.com/live.m3u8"));
	url_edit_->setClearButtonEnabled(true);
	common_form->addRow(QStringLiteral("URL:"), url_edit_);

	auto *local_row = new QHBoxLayout();
	local_path_edit_ = new QLineEdit(this);
	local_path_edit_->setPlaceholderText(QStringLiteral("Path to a local media file"));
	local_path_edit_->setClearButtonEnabled(true);
	local_row->addWidget(local_path_edit_, 1);
	local_browse_btn_ = new QToolButton(this);
	local_browse_btn_->setText(QStringLiteral("..."));
	local_browse_btn_->setToolTip(QStringLiteral("Browse for a local media file"));
	local_row->addWidget(local_browse_btn_);
	auto *local_row_widget = new QWidget(this);
	local_row_widget->setLayout(local_row);
	common_form->addRow(QStringLiteral("Local file:"), local_row_widget);

	spin_reconnect_delay_ = new QSpinBox(this);
	spin_reconnect_delay_->setRange(1, 60);
	spin_reconnect_delay_->setSuffix(QStringLiteral(" s"));
	spin_reconnect_delay_->setValue(kDefaultReconnectDelaySec);
	spin_reconnect_delay_->setToolTip(QStringLiteral(
		"Seconds to wait before retrying a network stream that drops or fails to open. Default 10."));
	common_form->addRow(QStringLiteral("Reconnect delay:"), spin_reconnect_delay_);

	spin_buffering_mb_ = new QSpinBox(this);
	spin_buffering_mb_->setRange(0, 16);
	spin_buffering_mb_->setSuffix(QStringLiteral(" MB"));
	spin_buffering_mb_->setValue(kDefaultBufferingMb);
	spin_buffering_mb_->setToolTip(QStringLiteral(
		"Network buffering, in megabytes. Larger values smooth out network jitter at the cost of latency."));
	common_form->addRow(QStringLiteral("Network buffering:"), spin_buffering_mb_);

	chk_hw_decode_ = new QCheckBox(QStringLiteral("Use hardware decoding when available"), this);
	chk_hw_decode_->setToolTip(QStringLiteral(
		"Hand decoding off to the GPU when supported. Reduces CPU but may fail on some hardware/codecs."));
	common_form->addRow(QString(), chk_hw_decode_);

	cmb_color_range_ = new QComboBox(this);
	cmb_color_range_->addItem(QStringLiteral("Auto"), 0);
	cmb_color_range_->addItem(QStringLiteral("Partial (TV)"), 1);
	cmb_color_range_->addItem(QStringLiteral("Full (PC)"), 2);
	cmb_color_range_->setToolTip(QStringLiteral(
		"YUV color range. Auto uses the codec's signaling; override only if your stream looks washed-out or crushed."));
	common_form->addRow(QStringLiteral("Color range:"), cmb_color_range_);

	chk_looping_ = new QCheckBox(QStringLiteral("Loop playback when media ends"), this);
	chk_looping_->setToolTip(QStringLiteral("Restart the file from the beginning when it finishes."));
	common_form->addRow(QString(), chk_looping_);

	root->addLayout(common_form);

	/* ---------- Advanced (collapsible) ---------- */

	grp_advanced_ = new QGroupBox(QStringLiteral("Advanced"), this);
	grp_advanced_->setCheckable(true);
	grp_advanced_->setChecked(false);
	auto *adv_form = new QFormLayout(grp_advanced_);

	chk_clear_on_end_ = new QCheckBox(QStringLiteral("Show nothing when playback ends"), this);
	chk_clear_on_end_->setChecked(true);
	chk_clear_on_end_->setToolTip(QStringLiteral(
		"After the media ends (non-looping), draw nothing instead of the last frame. Required for live streams that legitimately end."));
	adv_form->addRow(QString(), chk_clear_on_end_);

	chk_linear_alpha_ = new QCheckBox(QStringLiteral("Apply alpha in linear space"), this);
	chk_linear_alpha_->setToolTip(QStringLiteral("Niche; leave off unless you know you need it."));
	adv_form->addRow(QString(), chk_linear_alpha_);

	chk_seekable_ = new QCheckBox(QStringLiteral("Seekable"), this);
	chk_seekable_->setToolTip(QStringLiteral("Allows OBS to seek the stream. Live streams should leave this off."));
	adv_form->addRow(QString(), chk_seekable_);

	spin_speed_percent_ = new QSpinBox(this);
	spin_speed_percent_->setRange(1, 200);
	spin_speed_percent_->setSuffix(QStringLiteral(" %"));
	spin_speed_percent_->setValue(kDefaultSpeedPercent);
	spin_speed_percent_->setToolTip(
		QStringLiteral("Local-file playback speed. Network streams ignore this and play at real time."));
	adv_form->addRow(QStringLiteral("Speed:"), spin_speed_percent_);

	ffmpeg_options_edit_ = new QLineEdit(this);
	ffmpeg_options_edit_->setPlaceholderText(QStringLiteral("e.g. probesize=2000000 stimeout=5000000"));
	ffmpeg_options_edit_->setToolTip(
		QStringLiteral("Raw FFmpeg options passed to the decoder. Power-user, leave blank if unsure."));
	adv_form->addRow(QStringLiteral("FFmpeg options:"), ffmpeg_options_edit_);

	root->addWidget(grp_advanced_);

	/* Wire visibility + browse */
	connect(chk_local_file_, &QCheckBox::toggled, this, &FfmpegMediaForm::on_local_file_toggled);
	connect(local_browse_btn_, &QToolButton::clicked, this, &FfmpegMediaForm::on_browse_local_file);

	apply_local_visibility(false);
}

void FfmpegMediaForm::apply_local_visibility(bool is_local)
{
	url_edit_->setVisible(!is_local);
	local_path_edit_->setVisible(is_local);
	local_browse_btn_->setVisible(is_local);
	spin_reconnect_delay_->setVisible(!is_local);
	spin_buffering_mb_->setVisible(!is_local);
	chk_looping_->setVisible(is_local);

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
				/* The local-file row uses a layout container. Find
				 * the line edit inside it. */
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
	const QString picked = QFileDialog::getOpenFileName(
		this, QStringLiteral("Select media file"), local_path_edit_->text(),
		QStringLiteral(
			"Media files (*.mp4 *.mkv *.mov *.avi *.flv *.ts *.m3u8 *.webm *.mp3 *.aac *.wav);;All files (*)"));
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
	chk_seekable_->setChecked(src ? obs_data_get_bool(src, "seekable") : false);

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
	return chk_local_file_->isChecked()
		       ? QStringLiteral("Pick a local media file before adding the source.")
		       : QStringLiteral(
				 "Enter a media URL (RTMP / HLS / FLV / SRT / file URL) before adding the source.");
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

	if (is_local) {
		obs_data_set_bool(d, "is_local_file", true);
		obs_data_set_string(d, "local_file", lpath.toUtf8().constData());
		cfg.displayName = lpath.toStdString();
	} else {
		/* is_local_file defaults to false when missing — omit. */
		obs_data_set_string(d, "input", url.toUtf8().constData());
		cfg.displayName = url.toStdString();
	}

	set_or_default_int(d, "reconnect_delay_sec", spin_reconnect_delay_->value(), kDefaultReconnectDelaySec);
	set_or_default_int(d, "buffering_mb", spin_buffering_mb_->value(), kDefaultBufferingMb);
	set_or_default_bool(d, "hw_decode", chk_hw_decode_->isChecked(), false);

	int color_range = cmb_color_range_->currentData().toInt();
	set_or_default_int(d, "color_range", color_range, 0);

	set_or_default_bool(d, "looping", chk_looping_->isChecked(), false);

	set_or_default_bool(d, "clear_on_media_end", chk_clear_on_end_->isChecked(), true);
	set_or_default_bool(d, "linear_alpha", chk_linear_alpha_->isChecked(), false);
	set_or_default_bool(d, "seekable", chk_seekable_->isChecked(), false);
	set_or_default_int(d, "speed_percent", spin_speed_percent_->value(), kDefaultSpeedPercent);
	set_or_default_string(d, "ffmpeg_options", ffmpeg_options_edit_->text(), "");

	return cfg;
}
