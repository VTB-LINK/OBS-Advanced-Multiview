/*
OBS Advanced Multiview - DistroAV NDI provider settings form (Phase 3 / M6.2)

Split out of provider-settings-forms.cpp for maintainability. The form's
class declaration lives in provider-settings-forms.hpp; this TU only
implements its members. Discovery is provided by signal-provider-ndi.cpp
through signal_provider_ndi_discover_sources(); we never link DistroAV
or the NDI SDK directly.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "provider-settings-forms.hpp"
#include "provider-settings-forms-common.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QVBoxLayout>

using mv_forms::item_bare_name;
using mv_forms::list_contains_bare;
using mv_forms::make_lost_item;
using mv_forms::make_normal_item;
using mv_forms::set_or_default_bool;
using mv_forms::set_or_default_int;
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
			if (current && (current->flags() & Qt::ItemIsSelectable)) {
				const QString bare = item_bare_name(current);
				remembered_selection_ = bare;
				update_resolved_name(bare);
			}
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
	/* Use the persistent remembered_selection_ rather than reading
	 * back currentItem()->text() so the suffix decoration on a lost
	 * row never leaks into the bare name we're tracking. */
	discovered_list_->clear();

	const auto names = signal_provider_ndi_discover_sources();
	QListWidgetItem *to_select = nullptr;
	for (const auto &name : names) {
		const QString q = QString::fromStdString(name);
		auto *item = make_normal_item(q);
		discovered_list_->addItem(item);
		if (!remembered_selection_.isEmpty() && q == remembered_selection_)
			to_select = item;
	}

	/* Surface a previously-selected source that is currently missing
	 * from DistroAV's discovery cache as a lost-state row so the user
	 * can see what the cell is bound to. The row stays selectable and
	 * keeps the original ndi_source_name in UserRole; if the source
	 * returns on a later Refresh the row reverts to a normal entry
	 * with selection preserved. */
	if (!remembered_selection_.isEmpty() && !to_select) {
		auto *lost = make_lost_item(remembered_selection_);
		discovered_list_->addItem(lost);
		to_select = lost;
	}

	if (to_select)
		discovered_list_->setCurrentItem(to_select);

	if (names.empty() && remembered_selection_.isEmpty()) {
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
			auto *cur = discovered_list_->currentItem();
			if (cur->flags() & Qt::ItemIsSelectable)
				resolved = item_bare_name(cur);
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

	/* Track the persisted name across Refresh cycles. If it shows up
	 * in the current discovery list, refresh_discovery() will re-select
	 * it; otherwise it will be injected as a "signal lost" placeholder
	 * row so the user always sees what the cell is bound to. The
	 * manual_name_edit_ field is left empty in both cases since the
	 * lost-row carries the persisted value already. */
	remembered_selection_ = persisted_name;
	refresh_discovery();
	manual_name_edit_->setText(QString());

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
	chk_audio_->setChecked(src && obs_data_has_user_value(src, kNdiKeyAudio) ? obs_data_get_bool(src, kNdiKeyAudio)
										 : true);
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
			name = item_bare_name(cur);
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
