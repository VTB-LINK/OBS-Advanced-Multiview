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

	/* Phase 3 / M6.1+ post-9.1.B: flat layout (no Advanced foldout).
	 * Mirrors OBS's own ffmpeg_source properties dialog: pick a mode at
	 * the top, the rows that don't apply to that mode are hidden. Keys
	 * we hard-lock in FfmpegProvider::create_private_source are NOT
	 * shown at all (restart_on_activate, close_when_inactive, plus
	 * looping/seekable/speed_percent in network mode) so users don't
	 * see disabled controls and wonder why. */

	auto *form = new QFormLayout();

	chk_local_file_ = new QCheckBox(QStringLiteral("Local file"), this);
	chk_local_file_->setToolTip(
		QStringLiteral("Switch between a network URL (RTMP / HLS / FLV / SRT / http) and a local media file."));
	form->addRow(QString(), chk_local_file_);

	url_edit_ = new QLineEdit(this);
	url_edit_->setPlaceholderText(QStringLiteral("e.g. https://example.com/live.m3u8"));
	url_edit_->setClearButtonEnabled(true);
	form->addRow(QStringLiteral("URL:"), url_edit_);

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
	form->addRow(QStringLiteral("Local file:"), local_row_widget);

	/* Reconnect delay is network-only. Local files don't need a retry
	 * interval \u2014 if a local file fails to open, retrying every N seconds
	 * doesn't help (the file isn't going to materialize). */
	spin_reconnect_delay_ = new QSpinBox(this);
	spin_reconnect_delay_->setRange(1, 60);
	spin_reconnect_delay_->setSuffix(QStringLiteral(" s"));
	spin_reconnect_delay_->setValue(kDefaultReconnectDelaySec);
	spin_reconnect_delay_->setToolTip(QStringLiteral(
		"Seconds to wait before retrying a network stream that drops or fails to open. Default 10."));
	lbl_reconnect_delay_ = new QLabel(QStringLiteral("Reconnect delay:"), this);
	form->addRow(lbl_reconnect_delay_, spin_reconnect_delay_);

	spin_buffering_mb_ = new QSpinBox(this);
	spin_buffering_mb_->setRange(0, 16);
	spin_buffering_mb_->setSuffix(QStringLiteral(" MB"));
	spin_buffering_mb_->setValue(kDefaultBufferingMb);
	spin_buffering_mb_->setToolTip(QStringLiteral(
		"Network buffering, in megabytes. Larger values smooth out network jitter at the cost of latency."));
	form->addRow(QStringLiteral("Network buffering:"), spin_buffering_mb_);

	chk_hw_decode_ = new QCheckBox(QStringLiteral("Use hardware decoding when available"), this);
	chk_hw_decode_->setToolTip(QStringLiteral(
		"Hand decoding off to the GPU when supported. Reduces CPU but may fail on some hardware/codecs."));
	form->addRow(QString(), chk_hw_decode_);

	cmb_color_range_ = new QComboBox(this);
	cmb_color_range_->addItem(QStringLiteral("Auto"), 0);
	cmb_color_range_->addItem(QStringLiteral("Partial (TV)"), 1);
	cmb_color_range_->addItem(QStringLiteral("Full (PC)"), 2);
	cmb_color_range_->setToolTip(QStringLiteral(
		"YUV color range. Auto uses the codec's signaling; override only if your stream looks washed-out or crushed."));
	form->addRow(QStringLiteral("Color range:"), cmb_color_range_);

	chk_clear_on_end_ = new QCheckBox(QStringLiteral("Show nothing when playback ends"), this);
	chk_clear_on_end_->setChecked(true);
	chk_clear_on_end_->setToolTip(QStringLiteral(
		"After the media ends (non-looping), draw nothing instead of the last frame. Required for live streams that legitimately end."));
	form->addRow(QString(), chk_clear_on_end_);

	chk_looping_ = new QCheckBox(QStringLiteral("Loop playback when media ends"), this);
	chk_looping_->setToolTip(QStringLiteral("Restart the file from the beginning when it finishes."));
	form->addRow(QString(), chk_looping_);

	spin_speed_percent_ = new QSpinBox(this);
	spin_speed_percent_->setRange(1, 200);
	spin_speed_percent_->setSuffix(QStringLiteral(" %"));
	spin_speed_percent_->setValue(kDefaultSpeedPercent);
	spin_speed_percent_->setToolTip(QStringLiteral("Local-file playback speed."));
	lbl_speed_percent_ = new QLabel(QStringLiteral("Speed:"), this);
	form->addRow(lbl_speed_percent_, spin_speed_percent_);

	chk_linear_alpha_ = new QCheckBox(QStringLiteral("Apply alpha in linear space"), this);
	chk_linear_alpha_->setToolTip(QStringLiteral("Niche; leave off unless you know you need it."));
	form->addRow(QString(), chk_linear_alpha_);

	ffmpeg_options_edit_ = new QLineEdit(this);
	ffmpeg_options_edit_->setPlaceholderText(QStringLiteral("e.g. probesize=2000000 stimeout=5000000"));
	ffmpeg_options_edit_->setToolTip(
		QStringLiteral("Raw FFmpeg options passed to the decoder. Power-user, leave blank if unsure."));
	lbl_ffmpeg_options_ = new QLabel(QStringLiteral("FFmpeg options:"), this);
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
