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
#include <QListWidgetItem>
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

/* ---------- NdiSourceForm (Phase 3 / M6.2) ---------- */

namespace {

/* DistroAV setting keys + integer enum values, kept verbatim from
 * DistroAV's ndi-source.cpp so the form persists identical JSON to what
 * the user would see in OBS's own ndi_source dialog. */
constexpr const char *kNdiKeySourceName = "ndi_source_name";
constexpr const char *kNdiKeyBehavior = "ndi_behavior";
constexpr const char *kNdiKeyBehaviorTimeout = "ndi_behavior_timeout";
constexpr const char *kNdiKeyBandwidth = "ndi_bw_mode";
constexpr const char *kNdiKeySync = "ndi_sync";
constexpr const char *kNdiKeyLatency = "latency";
constexpr const char *kNdiKeyFramesync = "ndi_framesync";
constexpr const char *kNdiKeyHwAccel = "ndi_recv_hw_accel";
constexpr const char *kNdiKeyAudio = "ndi_audio";
constexpr const char *kNdiKeyYuvRange = "yuv_range";
constexpr const char *kNdiKeyYuvColorspace = "yuv_colorspace";
constexpr const char *kNdiKeyFixAlpha = "ndi_fix_alpha_blending";

constexpr int kNdiBwHighest = 0;
constexpr int kNdiBwLowest = 1;
constexpr int kNdiBwAudioOnly = 2;

constexpr int kNdiBehaviorKeepActive = 0;
constexpr int kNdiBehaviorStopBlank = 1;
constexpr int kNdiBehaviorStopLastFrame = 2;

constexpr int kNdiTimeoutClearContent = 0;
constexpr int kNdiTimeoutKeepContent = 1;

constexpr int kNdiSyncInternal = 0; /* legacy, removed in DistroAV but kept for round-trip */
constexpr int kNdiSyncNDITimestamp = 1;
constexpr int kNdiSyncNDISourceTimecode = 2;

constexpr int kNdiLatencyNormal = 0;
constexpr int kNdiLatencyLow = 1;
constexpr int kNdiLatencyLowest = 2;

constexpr int kNdiYuvRangePartial = 1;
constexpr int kNdiYuvRangeFull = 2;

constexpr int kNdiYuvSpaceBT601 = 1;
constexpr int kNdiYuvSpaceBT709 = 2;
constexpr int kNdiYuvSpaceBT2100 = 3;

} // namespace

NdiSourceForm::NdiSourceForm(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(8);

	auto *list_label = new QLabel(QStringLiteral("Discovered NDI sources on the network:"), this);
	root->addWidget(list_label);

	discovered_list_ = new QListWidget(this);
	discovered_list_->setSelectionMode(QAbstractItemView::SingleSelection);
	discovered_list_->setToolTip(
		QStringLiteral("DistroAV scans the LAN once when this dialog opens. Click Refresh to scan again. "
			       "If no sources appear, ensure your NDI sender is running on the same network."));
	root->addWidget(discovered_list_, 1);

	auto *refresh_row = new QHBoxLayout();
	refresh_btn_ = new QPushButton(QStringLiteral("Refresh"), this);
	refresh_btn_->setToolTip(
		QStringLiteral("Re-scan for NDI sources. Discovery uses DistroAV's NDIFinder which caches "
			       "results for 5 seconds; clicking Refresh more often than that is harmless."));
	refresh_row->addWidget(refresh_btn_);
	refresh_row->addStretch(1);
	root->addLayout(refresh_row);

	auto *form = new QFormLayout();

	manual_name_edit_ = new QLineEdit(this);
	manual_name_edit_->setPlaceholderText(QStringLiteral("e.g. MACHINE (Source Name)"));
	manual_name_edit_->setToolTip(
		QStringLiteral("Fallback for sources DistroAV's discovery hasn't found yet (firewall, routing issue, "
			       "or sender just started). Manual name overrides the list selection."));
	form->addRow(QStringLiteral("Manual name:"), manual_name_edit_);

	resolved_label_ = new QLabel(QStringLiteral("(no source selected)"), this);
	resolved_label_->setStyleSheet(QStringLiteral("color: #888;"));
	form->addRow(QStringLiteral("Will bind to:"), resolved_label_);

	cmb_behavior_ = new QComboBox(this);
	cmb_behavior_->addItem(QStringLiteral("Keep active (always receive)"), kNdiBehaviorKeepActive);
	cmb_behavior_->addItem(QStringLiteral("Pause when not visible (blank on resume)"), kNdiBehaviorStopBlank);
	cmb_behavior_->addItem(QStringLiteral("Pause when not visible (keep last frame)"), kNdiBehaviorStopLastFrame);
	cmb_behavior_->setToolTip(QStringLiteral(
		"What DistroAV does with the receiver when the source is not visible. For Multiview cells "
		"this rarely matters \u2014 the cell is always 'visible' for monitoring purposes."));
	form->addRow(QStringLiteral("Behavior:"), cmb_behavior_);

	cmb_bandwidth_ = new QComboBox(this);
	cmb_bandwidth_->addItem(QStringLiteral("Highest (full quality)"), kNdiBwHighest);
	cmb_bandwidth_->addItem(QStringLiteral("Lowest (proxy)"), kNdiBwLowest);
	cmb_bandwidth_->addItem(QStringLiteral("Audio only"), kNdiBwAudioOnly);
	cmb_bandwidth_->setToolTip(
		QStringLiteral("Highest = full-resolution stream. Lowest = DistroAV's built-in proxy stream "
			       "for monitoring at low bitrate. Audio only = no video frames at all."));
	form->addRow(QStringLiteral("Bandwidth:"), cmb_bandwidth_);

	cmb_sync_ = new QComboBox(this);
	cmb_sync_->addItem(QStringLiteral("Source timecode (recommended)"), kNdiSyncNDISourceTimecode);
	cmb_sync_->addItem(QStringLiteral("NDI timestamp"), kNdiSyncNDITimestamp);
	cmb_sync_->setToolTip(
		QStringLiteral("How DistroAV aligns NDI frames to OBS's clock. Source timecode follows the sender's "
			       "own timecode; NDI timestamp uses NDI library's internal timing."));
	form->addRow(QStringLiteral("A/V sync:"), cmb_sync_);

	cmb_latency_ = new QComboBox(this);
	cmb_latency_->addItem(QStringLiteral("Normal"), kNdiLatencyNormal);
	cmb_latency_->addItem(QStringLiteral("Low"), kNdiLatencyLow);
	cmb_latency_->addItem(QStringLiteral("Lowest"), kNdiLatencyLowest);
	cmb_latency_->setToolTip(QStringLiteral("Lower latency means less buffering on receive; can stutter "
						"on a flaky network."));
	form->addRow(QStringLiteral("Latency:"), cmb_latency_);

	cmb_yuv_range_ = new QComboBox(this);
	cmb_yuv_range_->addItem(QStringLiteral("Limited (TV)"), kNdiYuvRangePartial);
	cmb_yuv_range_->addItem(QStringLiteral("Full (PC)"), kNdiYuvRangeFull);
	cmb_yuv_range_->setToolTip(
		QStringLiteral("YUV range the sender uses. Override only if the cell looks washed-out or crushed."));
	form->addRow(QStringLiteral("YUV range:"), cmb_yuv_range_);

	cmb_yuv_colorspace_ = new QComboBox(this);
	cmb_yuv_colorspace_->addItem(QStringLiteral("BT.709 (HD)"), kNdiYuvSpaceBT709);
	cmb_yuv_colorspace_->addItem(QStringLiteral("BT.601 (SD)"), kNdiYuvSpaceBT601);
	cmb_yuv_colorspace_->addItem(QStringLiteral("BT.2100 (HDR)"), kNdiYuvSpaceBT2100);
	cmb_yuv_colorspace_->setToolTip(
		QStringLiteral("YUV colorspace. Mostly relevant for SD content (BT.601) or HDR content (BT.2100)."));
	form->addRow(QStringLiteral("YUV colorspace:"), cmb_yuv_colorspace_);

	chk_audio_ = new QCheckBox(QStringLiteral("Receive audio"), this);
	chk_audio_->setChecked(true);
	chk_audio_->setToolTip(QStringLiteral("When off, the cell is video-only."));
	form->addRow(QString(), chk_audio_);

	chk_framesync_ = new QCheckBox(QStringLiteral("Frame sync"), this);
	chk_framesync_->setChecked(false);
	chk_framesync_->setToolTip(
		QStringLiteral("Resample audio/video to match OBS's frame clock. Helps on senders with "
			       "drifting timing; adds ~1 frame of latency."));
	form->addRow(QString(), chk_framesync_);

	chk_hw_accel_ = new QCheckBox(QStringLiteral("Hardware acceleration (if available)"), this);
	chk_hw_accel_->setChecked(false);
	chk_hw_accel_->setToolTip(QStringLiteral("Request DistroAV use GPU decoding. Falls back to CPU silently."));
	form->addRow(QString(), chk_hw_accel_);

	chk_fix_alpha_ = new QCheckBox(QStringLiteral("Fix alpha blending"), this);
	chk_fix_alpha_->setChecked(false);
	chk_fix_alpha_->setToolTip(
		QStringLiteral("DistroAV adds an alpha-fix filter to compensate for senders that produce "
			       "premultiplied alpha. Leave off unless the cell looks washed-out on edges."));
	form->addRow(QString(), chk_fix_alpha_);

	root->addLayout(form);

	connect(refresh_btn_, &QPushButton::clicked, this, &NdiSourceForm::refresh_discovery);
	connect(discovered_list_, &QListWidget::currentItemChanged, this,
		[this](QListWidgetItem *current, QListWidgetItem *) {
			if (current)
				update_resolved_name(current->text());
		});
	connect(manual_name_edit_, &QLineEdit::textChanged, this,
		[this](const QString &) { update_resolved_name(QString()); });

	/* Kick the first scan so the list isn't empty on open. The scan
	 * itself goes through DistroAV's NDIFinder cache; the first call in
	 * a fresh OBS session returns whatever's in the 5-second cache (may
	 * be empty), and triggers an async refresh in the background. The
	 * user clicking Refresh after a few seconds will populate the list. */
	refresh_discovery();
}

void NdiSourceForm::refresh_discovery()
{
	const QString previously_selected = discovered_list_->currentItem() ? discovered_list_->currentItem()->text()
									    : QString();
	discovered_list_->clear();

	const auto names = signal_provider_ndi_discover_sources();
	for (const auto &name : names) {
		auto *item = new QListWidgetItem(QString::fromStdString(name));
		discovered_list_->addItem(item);
		if (item->text() == previously_selected)
			discovered_list_->setCurrentItem(item);
	}

	if (names.empty()) {
		/* Surface an explanatory placeholder so an empty list doesn't
		 * read as a bug. DistroAV's NDIFinder cache may legitimately
		 * be empty for the first 5 s on cold start. */
		auto *item = new QListWidgetItem(QStringLiteral("(scanning... click Refresh again in a few seconds)"));
		item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
		QFont f = item->font();
		f.setItalic(true);
		item->setFont(f);
		discovered_list_->addItem(item);
	}

	update_resolved_name(QString());
}

void NdiSourceForm::update_resolved_name(const QString &name_hint)
{
	QString resolved = manual_name_edit_->text().trimmed();
	if (resolved.isEmpty()) {
		if (!name_hint.isEmpty()) {
			resolved = name_hint;
		} else if (discovered_list_->currentItem()) {
			const QString sel = discovered_list_->currentItem()->text();
			if (discovered_list_->currentItem()->flags() & Qt::ItemIsSelectable)
				resolved = sel;
		}
	}
	if (resolved.isEmpty())
		resolved_label_->setText(QStringLiteral("(no source selected)"));
	else
		resolved_label_->setText(resolved);
}

void NdiSourceForm::load_from(const SignalConfig &cfg)
{
	obs_data_t *src = cfg.providerSettings;

	const QString persisted_name = src ? QString::fromUtf8(obs_data_get_string(src, kNdiKeySourceName)) : QString();

	/* If the persisted name appears in the discovery list, select it;
	 * otherwise treat it as a manual entry. */
	bool selected_in_list = false;
	for (int i = 0; i < discovered_list_->count(); i++) {
		auto *item = discovered_list_->item(i);
		if (!(item->flags() & Qt::ItemIsSelectable))
			continue;
		if (item->text() == persisted_name) {
			discovered_list_->setCurrentItem(item);
			selected_in_list = true;
			break;
		}
	}
	manual_name_edit_->setText(selected_in_list ? QString() : persisted_name);

	if (src && obs_data_has_user_value(src, kNdiKeyBehavior)) {
		int idx = cmb_behavior_->findData((int)obs_data_get_int(src, kNdiKeyBehavior));
		if (idx >= 0)
			cmb_behavior_->setCurrentIndex(idx);
	}
	if (src && obs_data_has_user_value(src, kNdiKeyBandwidth)) {
		int idx = cmb_bandwidth_->findData((int)obs_data_get_int(src, kNdiKeyBandwidth));
		if (idx >= 0)
			cmb_bandwidth_->setCurrentIndex(idx);
	}
	if (src && obs_data_has_user_value(src, kNdiKeySync)) {
		int idx = cmb_sync_->findData((int)obs_data_get_int(src, kNdiKeySync));
		if (idx >= 0)
			cmb_sync_->setCurrentIndex(idx);
	}
	if (src && obs_data_has_user_value(src, kNdiKeyLatency)) {
		int idx = cmb_latency_->findData((int)obs_data_get_int(src, kNdiKeyLatency));
		if (idx >= 0)
			cmb_latency_->setCurrentIndex(idx);
	}
	if (src && obs_data_has_user_value(src, kNdiKeyYuvRange)) {
		int idx = cmb_yuv_range_->findData((int)obs_data_get_int(src, kNdiKeyYuvRange));
		if (idx >= 0)
			cmb_yuv_range_->setCurrentIndex(idx);
	}
	if (src && obs_data_has_user_value(src, kNdiKeyYuvColorspace)) {
		int idx = cmb_yuv_colorspace_->findData((int)obs_data_get_int(src, kNdiKeyYuvColorspace));
		if (idx >= 0)
			cmb_yuv_colorspace_->setCurrentIndex(idx);
	}
	chk_audio_->setChecked(src ? obs_data_get_bool(src, kNdiKeyAudio) : true);
	chk_framesync_->setChecked(src ? obs_data_get_bool(src, kNdiKeyFramesync) : false);
	chk_hw_accel_->setChecked(src ? obs_data_get_bool(src, kNdiKeyHwAccel) : false);
	chk_fix_alpha_->setChecked(src ? obs_data_get_bool(src, kNdiKeyFixAlpha) : false);

	update_resolved_name(QString());
}

bool NdiSourceForm::is_valid() const
{
	const QString manual = manual_name_edit_->text().trimmed();
	if (!manual.isEmpty())
		return true;
	auto *cur = discovered_list_->currentItem();
	if (cur && (cur->flags() & Qt::ItemIsSelectable))
		return true;
	return false;
}

QString NdiSourceForm::invalid_reason() const
{
	if (is_valid())
		return QString();
	return QStringLiteral("Select a discovered NDI source or enter a manual source name.");
}

SignalConfig NdiSourceForm::to_signal_config() const
{
	SignalConfig cfg;
	if (!is_valid())
		return cfg;

	cfg.provider = SignalProviderType::Ndi;

	QString name = manual_name_edit_->text().trimmed();
	if (name.isEmpty()) {
		auto *cur = discovered_list_->currentItem();
		if (cur && (cur->flags() & Qt::ItemIsSelectable))
			name = cur->text();
	}
	cfg.displayName = name.toStdString();

	cfg.providerSettings = obs_data_create();
	obs_data_t *d = cfg.providerSettings;
	obs_data_set_string(d, kNdiKeySourceName, name.toUtf8().constData());

	set_or_default_int(d, kNdiKeyBehavior, cmb_behavior_->currentData().toInt(), kNdiBehaviorKeepActive);
	set_or_default_int(d, kNdiKeyBandwidth, cmb_bandwidth_->currentData().toInt(), kNdiBwHighest);
	set_or_default_int(d, kNdiKeySync, cmb_sync_->currentData().toInt(), kNdiSyncNDISourceTimecode);
	set_or_default_int(d, kNdiKeyLatency, cmb_latency_->currentData().toInt(), kNdiLatencyNormal);
	set_or_default_int(d, kNdiKeyYuvRange, cmb_yuv_range_->currentData().toInt(), kNdiYuvRangePartial);
	set_or_default_int(d, kNdiKeyYuvColorspace, cmb_yuv_colorspace_->currentData().toInt(), kNdiYuvSpaceBT709);
	set_or_default_bool(d, kNdiKeyAudio, chk_audio_->isChecked(), true);
	set_or_default_bool(d, kNdiKeyFramesync, chk_framesync_->isChecked(), false);
	set_or_default_bool(d, kNdiKeyHwAccel, chk_hw_accel_->isChecked(), false);
	set_or_default_bool(d, kNdiKeyFixAlpha, chk_fix_alpha_->isChecked(), false);

	return cfg;
}
