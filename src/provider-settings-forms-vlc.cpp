/*
OBS Advanced Multiview - VLC media provider settings form (Phase 3 / M6.4)

Split out of provider-settings-forms.cpp for maintainability. The form's
class declaration lives in provider-settings-forms.hpp; this TU only
implements its members.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "provider-settings-forms.hpp"
#include "provider-settings-forms-common.hpp"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidgetItem>
#include <QVBoxLayout>

using mv_forms::set_or_default_bool;
using mv_forms::set_or_default_int;
/* ---------- VlcMediaForm (Phase 3 / M6.4) ---------- */

namespace {

constexpr const char *kVlcKeyPlaylist = "playlist";
constexpr const char *kVlcKeyLoop = "loop";
constexpr const char *kVlcKeyShuffle = "shuffle";
constexpr const char *kVlcKeyBehavior = "playback_behavior";
constexpr const char *kVlcKeyNetworkCaching = "network_caching";
constexpr const char *kVlcKeyTrack = "track";
constexpr const char *kVlcKeySubtitleEnable = "subtitle_enable";
constexpr const char *kVlcKeySubtitleTrack = "subtitle_track";
constexpr const char *kVlcPlaylistItemValueKey = "value";

constexpr const char *kVlcBehaviorStopRestart = "stop_restart";
constexpr const char *kVlcBehaviorPauseUnpause = "pause_unpause";
constexpr const char *kVlcBehaviorAlwaysPlay = "always_play";

constexpr int kVlcDefaultNetworkCaching = 400;
constexpr int kVlcDefaultTrack = 1;

} // namespace

VlcMediaForm::VlcMediaForm(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(8);

	/* Playlist editor: ordered list of paths/URLs, with add/remove/
	 * reorder buttons. The order matters because vlc-video plays
	 * items in playlist order (and shuffles them when shuffle is
	 * checked). */
	auto *list_label = new QLabel(QStringLiteral("Playlist (files and/or URLs):"), this);
	root->addWidget(list_label);

	playlist_list_ = new QListWidget(this);
	playlist_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	/* Long file paths previously triggered a horizontal scrollbar.
	 * Elide the middle of each row so the user still sees both the
	 * directory hint and the filename; combined with the explicit
	 * Off scroll policy this keeps the list strictly vertical. */
	playlist_list_->setTextElideMode(Qt::ElideMiddle);
	playlist_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	/* Drag-to-reorder replaces the previous ↑ / ↓ buttons. InternalMove
	 * tells Qt to move QListWidgetItem instances in-place rather than
	 * serialize through MIME data, so the user's row identity (path
	 * string) survives the drop unchanged. */
	playlist_list_->setDragEnabled(true);
	playlist_list_->setAcceptDrops(true);
	playlist_list_->setDropIndicatorShown(true);
	playlist_list_->setDragDropMode(QAbstractItemView::InternalMove);
	playlist_list_->setDefaultDropAction(Qt::MoveAction);
	playlist_list_->setToolTip(
		QStringLiteral("libVLC plays entries in order. Mixed local files + network URLs are supported; "
			       "any entry containing '://' is treated as a URL. Drag rows to reorder."));
	root->addWidget(playlist_list_, 1);

	auto *btn_row = new QHBoxLayout();
	btn_add_file_ = new QPushButton(QStringLiteral("Add file"), this);
	btn_add_files_ = new QPushButton(QStringLiteral("Add files..."), this);
	btn_add_url_ = new QPushButton(QStringLiteral("Add URL"), this);
	btn_edit_ = new QPushButton(QStringLiteral("Edit"), this);
	btn_edit_->setToolTip(
		QStringLiteral("Edit the selected playlist entry's path or URL. Double-click a row to do the same."));
	btn_remove_ = new QPushButton(QStringLiteral("Remove"), this);
	btn_clear_ = new QPushButton(QStringLiteral("Clear"), this);
	btn_row->addWidget(btn_add_file_);
	btn_row->addWidget(btn_add_files_);
	btn_row->addWidget(btn_add_url_);
	btn_row->addWidget(btn_edit_);
	btn_row->addWidget(btn_remove_);
	btn_row->addWidget(btn_clear_);
	btn_row->addStretch(1);
	root->addLayout(btn_row);

	auto *form = new QFormLayout();

	chk_loop_ = new QCheckBox(QStringLiteral("Loop playlist"), this);
	chk_loop_->setChecked(true);
	chk_loop_->setToolTip(QStringLiteral("Restart the playlist from the beginning when the last entry ends."));
	form->addRow(QString(), chk_loop_);

	chk_shuffle_ = new QCheckBox(QStringLiteral("Shuffle"), this);
	chk_shuffle_->setToolTip(QStringLiteral("Randomize playlist order on each cycle."));
	form->addRow(QString(), chk_shuffle_);

	cmb_behavior_ = new QComboBox(this);
	cmb_behavior_->addItem(QStringLiteral("Stop when inactive, restart on activate"),
			       QString::fromUtf8(kVlcBehaviorStopRestart));
	cmb_behavior_->addItem(QStringLiteral("Pause when inactive, unpause on activate"),
			       QString::fromUtf8(kVlcBehaviorPauseUnpause));
	cmb_behavior_->addItem(QStringLiteral("Always play (ignore activate state)"),
			       QString::fromUtf8(kVlcBehaviorAlwaysPlay));
	cmb_behavior_->setToolTip(
		QStringLiteral("How vlc_source reacts to OBS activate/deactivate signals. "
			       "For a multiview cell, Stop+Restart matches the FFmpeg provider's behavior."));
	form->addRow(QStringLiteral("On activate/deactivate:"), cmb_behavior_);

	spin_network_caching_ = new QSpinBox(this);
	spin_network_caching_->setRange(50, 60000);
	spin_network_caching_->setSingleStep(50);
	spin_network_caching_->setSuffix(QStringLiteral(" ms"));
	spin_network_caching_->setValue(kVlcDefaultNetworkCaching);
	spin_network_caching_->setToolTip(
		QStringLiteral("libVLC network cache size (--network-caching). Smaller = lower latency, "
			       "larger = smoother on lossy networks."));
	form->addRow(QStringLiteral("Network caching:"), spin_network_caching_);

	spin_track_ = new QSpinBox(this);
	spin_track_->setRange(1, 32);
	spin_track_->setValue(kVlcDefaultTrack);
	spin_track_->setToolTip(QStringLiteral("Audio track index to play (1 = first track)."));
	form->addRow(QStringLiteral("Audio track:"), spin_track_);

	root->addLayout(form);

	/* Button wiring. The add/remove handlers are all small inline
	 * lambdas because none have anything reusable about them. */
	connect(btn_add_file_, &QPushButton::clicked, this, [this]() {
		const QString path = QFileDialog::getOpenFileName(
			this, QStringLiteral("Add media file"), QString(),
			QStringLiteral("Media files (*.mp4 *.mov *.mkv *.webm *.avi *.flv *.ts *.mp3 "
				       "*.wav *.flac *.m3u *.m3u8 *.pls);;All files (*)"));
		if (!path.isEmpty())
			playlist_list_->addItem(new QListWidgetItem(path));
	});
	connect(btn_add_files_, &QPushButton::clicked, this, [this]() {
		const QStringList paths = QFileDialog::getOpenFileNames(
			this, QStringLiteral("Add media files"), QString(),
			QStringLiteral("Media files (*.mp4 *.mov *.mkv *.webm *.avi *.flv *.ts *.mp3 "
				       "*.wav *.flac *.m3u *.m3u8 *.pls);;All files (*)"));
		for (const auto &p : paths)
			playlist_list_->addItem(new QListWidgetItem(p));
	});
	connect(btn_add_url_, &QPushButton::clicked, this, [this]() {
		bool ok = false;
		const QString url =
			QInputDialog::getText(this, QStringLiteral("Add URL"),
					      QStringLiteral("Stream URL (RTMP, HLS, SRT, http(s), rtsp, etc.):"),
					      QLineEdit::Normal, QString(), &ok);
		if (ok && !url.trimmed().isEmpty())
			playlist_list_->addItem(new QListWidgetItem(url.trimmed()));
	});
	connect(btn_remove_, &QPushButton::clicked, this, [this]() {
		const auto items = playlist_list_->selectedItems();
		for (auto *it : items)
			delete playlist_list_->takeItem(playlist_list_->row(it));
	});
	connect(btn_clear_, &QPushButton::clicked, this, [this]() { playlist_list_->clear(); });

	/* Edit handler: shared by the Edit button and the double-click
	 * activation. Local file entries open a Qt file dialog seeded with
	 * the existing path so the user can pick a different file in the
	 * same directory; URL entries open a text input dialog seeded with
	 * the existing URL. "Local" is detected the same way the provider
	 * decides on unbuffered policy: any '://' substring marks a URL. */
	auto edit_item = [this](QListWidgetItem *item) {
		if (!item)
			return;
		const QString existing = item->text();
		const bool is_url = existing.contains(QStringLiteral("://"));
		if (is_url) {
			bool ok = false;
			const QString url = QInputDialog::getText(
				this, QStringLiteral("Edit URL"),
				QStringLiteral("Stream URL (RTMP, HLS, SRT, http(s), rtsp, etc.):"), QLineEdit::Normal,
				existing, &ok);
			if (ok && !url.trimmed().isEmpty())
				item->setText(url.trimmed());
		} else {
			const QString path = QFileDialog::getOpenFileName(
				this, QStringLiteral("Edit media file"), existing,
				QStringLiteral("Media files (*.mp4 *.mov *.mkv *.webm *.avi *.flv *.ts *.mp3 "
					       "*.wav *.flac *.m3u *.m3u8 *.pls);;All files (*)"));
			if (!path.isEmpty())
				item->setText(path);
		}
	};
	connect(btn_edit_, &QPushButton::clicked, this, [this, edit_item]() {
		auto *cur = playlist_list_->currentItem();
		if (!cur) {
			const auto items = playlist_list_->selectedItems();
			if (!items.isEmpty())
				cur = items.first();
		}
		edit_item(cur);
	});
	connect(playlist_list_, &QListWidget::itemDoubleClicked, this,
		[edit_item](QListWidgetItem *item) { edit_item(item); });
}

void VlcMediaForm::load_from(const SignalConfig &cfg)
{
	obs_data_t *src = cfg.providerSettings;
	if (!src)
		return;

	/* Playlist round-trip: vlc-video stores each entry as an
	 * obs_data_t with a "value" string. Skip empty entries because
	 * vlc-video itself would silently no-op on them and we don't want
	 * to surface ghost rows to the user. */
	playlist_list_->clear();
	obs_data_array_t *arr = obs_data_get_array(src, kVlcKeyPlaylist);
	if (arr) {
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			if (item) {
				const char *val = obs_data_get_string(item, kVlcPlaylistItemValueKey);
				if (val && *val)
					playlist_list_->addItem(new QListWidgetItem(QString::fromUtf8(val)));
				obs_data_release(item);
			}
		}
		obs_data_array_release(arr);
	}

	chk_loop_->setChecked(obs_data_has_user_value(src, kVlcKeyLoop) ? obs_data_get_bool(src, kVlcKeyLoop) : true);
	chk_shuffle_->setChecked(obs_data_get_bool(src, kVlcKeyShuffle));

	const QString behavior = obs_data_has_user_value(src, kVlcKeyBehavior)
					 ? QString::fromUtf8(obs_data_get_string(src, kVlcKeyBehavior))
					 : QString::fromUtf8(kVlcBehaviorStopRestart);
	{
		int idx = cmb_behavior_->findData(behavior);
		if (idx >= 0)
			cmb_behavior_->setCurrentIndex(idx);
	}

	spin_network_caching_->setValue(obs_data_has_user_value(src, kVlcKeyNetworkCaching)
						? (int)obs_data_get_int(src, kVlcKeyNetworkCaching)
						: kVlcDefaultNetworkCaching);
	spin_track_->setValue(obs_data_has_user_value(src, kVlcKeyTrack) ? (int)obs_data_get_int(src, kVlcKeyTrack)
									 : kVlcDefaultTrack);
}

bool VlcMediaForm::is_valid() const
{
	return playlist_list_->count() > 0;
}

QString VlcMediaForm::invalid_reason() const
{
	if (is_valid())
		return QString();
	return QStringLiteral("Add at least one file or URL to the playlist.");
}

SignalConfig VlcMediaForm::to_signal_config() const
{
	SignalConfig cfg;
	if (!is_valid())
		return cfg;

	cfg.provider = SignalProviderType::Vlc;

	/* Cell label: use the first entry's basename / URL for a short
	 * recognizable hint. The full playlist lives in providerSettings. */
	{
		const QString first = playlist_list_->item(0)->text();
		QString hint = first;
		const int slash = first.lastIndexOf(QChar('/'));
		const int back = first.lastIndexOf(QChar('\\'));
		const int sep = std::max(slash, back);
		if (sep >= 0 && sep + 1 < first.size())
			hint = first.mid(sep + 1);
		if (playlist_list_->count() > 1)
			hint += QStringLiteral(" (+%1 more)").arg(playlist_list_->count() - 1);
		cfg.displayName = hint.toStdString();
	}

	cfg.providerSettings = obs_data_create();
	obs_data_t *d = cfg.providerSettings;

	/* Build the playlist obs_data_array_t in the exact shape
	 * vlc-video-source.c expects: each item is an obs_data_t with a
	 * "value" string. */
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < playlist_list_->count(); i++) {
		const QString text = playlist_list_->item(i)->text();
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, kVlcPlaylistItemValueKey, text.toUtf8().constData());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(d, kVlcKeyPlaylist, arr);
	obs_data_array_release(arr);

	set_or_default_bool(d, kVlcKeyLoop, chk_loop_->isChecked(), true);
	set_or_default_bool(d, kVlcKeyShuffle, chk_shuffle_->isChecked(), false);

	const QString behavior = cmb_behavior_->currentData().toString();
	if (behavior != QString::fromUtf8(kVlcBehaviorStopRestart))
		obs_data_set_string(d, kVlcKeyBehavior, behavior.toUtf8().constData());
	else
		obs_data_unset_user_value(d, kVlcKeyBehavior);

	set_or_default_int(d, kVlcKeyNetworkCaching, spin_network_caching_->value(), kVlcDefaultNetworkCaching);
	set_or_default_int(d, kVlcKeyTrack, spin_track_->value(), kVlcDefaultTrack);

	return cfg;
}
