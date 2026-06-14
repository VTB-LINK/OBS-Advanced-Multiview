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

#include "source-picker.hpp"
#include "amv-i18n.hpp"
#include "provider-settings-forms.hpp"
#include "signal-provider.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QVBoxLayout>

SourcePicker::SourcePicker(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(amv::text("AMVPlugin.SourcePicker.Title"));
	setMinimumSize(480, 540);
	resize(560, 720);

	auto *mainLayout = new QVBoxLayout(this);

	/* Filter */
	auto *filterLayout = new QHBoxLayout;
	filterLayout->addWidget(new QLabel(amv::text("AMVPlugin.SourcePicker.Filter")));
	filter_edit_ = new QLineEdit;
	filter_edit_->setPlaceholderText(amv::text("AMVPlugin.SourcePicker.FilterPlaceholder"));
	filterLayout->addWidget(filter_edit_);
	mainLayout->addLayout(filterLayout);

	/* Tabs: Special | Scenes | Sources */
	tabs_ = new QTabWidget;
	mainLayout->addWidget(tabs_);

	special_list_ = new QListWidget;
	tabs_->addTab(special_list_, amv::text("AMVPlugin.SourcePicker.Tab.Special"));

	scene_list_ = new QListWidget;
	tabs_->addTab(scene_list_, amv::text("AMVPlugin.SourcePicker.Tab.Scenes"));

	source_list_ = new QListWidget;
	tabs_->addTab(source_list_, amv::text("AMVPlugin.SourcePicker.Tab.Sources"));

	/* Phase 3 / M6: external-provider tabs.
	 *
	 * Media (M6.1) is the first real provider tab — it ships a URL
	 * input that feeds OBS's built-in `ffmpeg_source` via a private
	 * source created at refresh_sources() time. The other four stay
	 * placeholders until their milestones land. We keep all tabs visible
	 * at all times so the user knows the capability exists and the tab
	 * index numbering stays stable across builds. */
	media_tab_ = build_media_tab();
	tabs_->addTab(media_tab_, amv::text("AMVPlugin.SourcePicker.Tab.Media"));

	ndi_tab_ = build_ndi_tab();
	tabs_->addTab(ndi_tab_, amv::text("AMVPlugin.SourcePicker.Tab.NDI"));

	spout_tab_ = build_spout_tab();
	tabs_->addTab(spout_tab_, amv::text("AMVPlugin.SourcePicker.Tab.Spout"));

	vlc_tab_ = build_vlc_tab();
	tabs_->addTab(vlc_tab_, amv::text("AMVPlugin.SourcePicker.Tab.VLC"));

	webrtc_tab_ = build_external_placeholder(SignalProviderType::WebRtcReserved,
						 amv::text("AMVPlugin.SourcePicker.WebRTC.Milestone"),
						 amv::text("AMVPlugin.SourcePicker.WebRTC.Description"));
	tabs_->addTab(webrtc_tab_, amv::text("AMVPlugin.SourcePicker.Tab.WebRTC"));

	/* Buttons */
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	mainLayout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &SourcePicker::on_accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(filter_edit_, &QLineEdit::textChanged, this, &SourcePicker::on_filter_changed);
	connect(special_list_, &QListWidget::itemDoubleClicked, this, &SourcePicker::on_item_double_clicked);
	connect(scene_list_, &QListWidget::itemDoubleClicked, this, &SourcePicker::on_item_double_clicked);
	connect(source_list_, &QListWidget::itemDoubleClicked, this, &SourcePicker::on_item_double_clicked);

	populate_list();
}

void SourcePicker::populate_list()
{
	/* Special entries */
	auto *pgmItem = new QListWidgetItem(amv::text("AMVPlugin.SourcePicker.Special.Program"));
	pgmItem->setData(Qt::UserRole, QStringLiteral("pgm"));
	pgmItem->setData(Qt::UserRole + 1, QString());
	special_list_->addItem(pgmItem);

	auto *prvwItem = new QListWidgetItem(amv::text("AMVPlugin.SourcePicker.Special.Preview"));
	prvwItem->setData(Qt::UserRole, QStringLiteral("prvw"));
	prvwItem->setData(Qt::UserRole + 1, QString());
	special_list_->addItem(prvwItem);

	/* Scenes */
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *src = scenes.sources.array[i];
		const char *name = obs_source_get_name(src);
		if (!name)
			continue;

		auto *item = new QListWidgetItem(QString::fromUtf8(name));
		item->setData(Qt::UserRole, QStringLiteral("scene"));
		item->setData(Qt::UserRole + 1, QString::fromUtf8(name));
		scene_list_->addItem(item);
	}

	obs_frontend_source_list_free(&scenes);

	/* Sources (non-scene) */
	auto enum_cb = [](void *param, obs_source_t *src) -> bool {
		auto *list = static_cast<QListWidget *>(param);

		/* Skip scenes, they're already in scene tab */
		if (obs_source_get_type(src) == OBS_SOURCE_TYPE_SCENE)
			return true;

		/* Show any renderable signal source: video-capable sources and
		 * audio-only sources (Audio Input/Output Capture, Application
		 * Audio Capture, etc.). Audio-only cells render an AUDIO ONLY
		 * status band in the multiview window. */
		uint32_t flags = obs_source_get_output_flags(src);
		if (!(flags & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)))
			return true;

		const char *name = obs_source_get_name(src);
		if (!name || name[0] == '\0')
			return true;

		auto *item = new QListWidgetItem(QString::fromUtf8(name));
		item->setData(Qt::UserRole, QStringLiteral("source"));
		item->setData(Qt::UserRole + 1, QString::fromUtf8(name));
		list->addItem(item);

		return true;
	};

	obs_enum_sources(enum_cb, source_list_);
}

void SourcePicker::on_filter_changed(const QString &text)
{
	auto filter_list = [&text](QListWidget *list) {
		for (int i = 0; i < list->count(); i++) {
			auto *item = list->item(i);
			bool visible = text.isEmpty() || item->text().contains(text, Qt::CaseInsensitive);
			item->setHidden(!visible);
		}
	};

	filter_list(special_list_);
	filter_list(scene_list_);
	filter_list(source_list_);
}

void SourcePicker::on_item_double_clicked(QListWidgetItem *item)
{
	Q_UNUSED(item);
	on_accept();
}

void SourcePicker::on_accept()
{
	QListWidget *activeList = nullptr;
	int idx = tabs_->currentIndex();
	if (idx == 0)
		activeList = special_list_;
	else if (idx == 1)
		activeList = scene_list_;
	else if (idx == 2)
		activeList = source_list_;
	else if (tabs_->widget(idx) == media_tab_) {
		/* Phase 3 / M6.1+ task 9.1.B: full ffmpeg parity. The form
		 * builds a ready-to-persist SignalConfig with provider=Ffmpeg
		 * and providerSettings carrying every key the user touched
		 * (defaults are dropped to keep persisted JSON compact). */
		if (!media_form_ || !media_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.MediaRequired"),
						 media_form_ ? media_form_->invalid_reason() : QString());
			return;
		}
		result_ = CellAssignment{};
		result_.signalConfig = media_form_->to_signal_config();
		accept();
		return;
	} else if (tabs_->widget(idx) == ndi_tab_) {
		/* Phase 3 / M6.2: NDI tab. Form yields a SignalConfig with
		 * provider=Ndi and providerSettings carrying ndi_source_name +
		 * the user's choice of bandwidth / latency / audio / framesync
		 * / hardware acceleration. */
		if (!ndi_form_ || !ndi_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.NDIRequired"),
						 ndi_form_ ? ndi_form_->invalid_reason() : QString());
			return;
		}
		result_ = CellAssignment{};
		result_.signalConfig = ndi_form_->to_signal_config();
		accept();
		return;
	} else if (tabs_->widget(idx) == spout_tab_) {
		/* Phase 3 / M6.3: Spout tab. Form yields a SignalConfig with
		 * provider=Spout and providerSettings carrying spoutsenders
		 * (or "usefirstavailablesender") + composite mode + tick
		 * speed. */
		if (!spout_form_ || !spout_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.SpoutRequired"),
						 spout_form_ ? spout_form_->invalid_reason() : QString());
			return;
		}
		result_ = CellAssignment{};
		result_.signalConfig = spout_form_->to_signal_config();
		accept();
		return;
	} else if (tabs_->widget(idx) == vlc_tab_) {
		/* Phase 3 / M6.4: VLC tab. Form yields a SignalConfig with
		 * provider=Vlc and providerSettings carrying playlist +
		 * loop / shuffle / behavior / network_caching / track. */
		if (!vlc_form_ || !vlc_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.PlaylistRequired"),
						 vlc_form_ ? vlc_form_->invalid_reason() : QString());
			return;
		}
		result_ = CellAssignment{};
		result_.signalConfig = vlc_form_->to_signal_config();
		accept();
		return;
	} else {
		/* Other external provider tabs are still placeholders. Surface
		 * a clear message instead of silently rejecting so the user
		 * knows the tab is a real capability that just is not
		 * implemented yet, then keep the dialog open so they can pick
		 * something else. */
		QMessageBox::information(this, amv::text("AMVPlugin.SourcePicker.ExternalUnavailable.Title"),
					 amv::text("AMVPlugin.SourcePicker.ExternalUnavailable.Message"));
		return;
	}

	auto *current = activeList->currentItem();
	if (!current) {
		reject();
		return;
	}

	result_.type = current->data(Qt::UserRole).toString().toStdString();
	result_.name = current->data(Qt::UserRole + 1).toString().toStdString();

	accept();
}

QWidget *SourcePicker::build_external_placeholder(SignalProviderType provider, const QString &coming_in,
						  const QString &description)
{
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	/* Heading: which milestone delivers this provider. */
	auto *heading = new QLabel(amv::text("AMVPlugin.SourcePicker.ComingIn").arg(coming_in), page);
	{
		QFont f = heading->font();
		f.setBold(true);
		heading->setFont(f);
	}
	layout->addWidget(heading);

	/* Description body. */
	auto *body = new QLabel(description, page);
	body->setWordWrap(true);
	layout->addWidget(body);

	/* Provider availability hint. The registry only carries internal
	 * adapters in Phase C, so external providers will report "not yet
	 * registered". Once the M6.x milestone wires the concrete provider
	 * into the registry, this line will switch to the host plugin's
	 * actual availability state without further UI changes here. */
	const auto &reg = SignalProviderRegistry::instance();
	const auto *p = reg.find(provider);
	QString status;
	if (!p) {
		status = amv::text("AMVPlugin.SourcePicker.Provider.NotRegistered");
	} else if (p->is_available()) {
		status = amv::text("AMVPlugin.SourcePicker.Provider.Available");
	} else {
		const std::string reason = p->unavailable_reason();
		status = amv::text("AMVPlugin.SourcePicker.Provider.Unavailable")
				 .arg(reason.empty() ? amv::text("AMVPlugin.SourcePicker.Provider.HostPluginMissing")
						     : QString::fromStdString(reason));
	}
	auto *availLabel = new QLabel(status, page);
	availLabel->setStyleSheet(QStringLiteral("color: #888;"));
	layout->addWidget(availLabel);

	layout->addStretch(1);
	return page;
}

QWidget *SourcePicker::build_media_tab()
{
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	auto *heading = new QLabel(amv::text("AMVPlugin.SourcePicker.Media.Heading"), page);
	{
		QFont f = heading->font();
		f.setBold(true);
		heading->setFont(f);
	}
	layout->addWidget(heading);

	auto *body = new QLabel(amv::text("AMVPlugin.SourcePicker.Media.Description"), page);
	body->setWordWrap(true);
	layout->addWidget(body);

	/* The form is moderately tall once Advanced expands; embed it in a
	 * scroll area so the dialog stays usable on small screens. */
	auto *scroll = new QScrollArea(page);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	media_form_ = new FfmpegMediaForm();
	scroll->setWidget(media_form_);
	layout->addWidget(scroll, 1);

	/* Availability hint mirrors the placeholder tabs so all external tabs
	 * use the same diagnostic surface. ffmpeg_source is built into OBS so
	 * the typical render here is "Provider available". */
	const auto &reg = SignalProviderRegistry::instance();
	const auto *p = reg.find(SignalProviderType::Ffmpeg);
	QString status;
	if (!p) {
		status = amv::text("AMVPlugin.SourcePicker.Provider.NotRegistered");
	} else if (p->is_available()) {
		status = amv::text("AMVPlugin.SourcePicker.Media.ProviderAvailable");
	} else {
		const std::string reason = p->unavailable_reason();
		status = amv::text("AMVPlugin.SourcePicker.Provider.Unavailable")
				 .arg(reason.empty() ? amv::text("AMVPlugin.SourcePicker.Provider.HostPluginMissing")
						     : QString::fromStdString(reason));
	}
	auto *availLabel = new QLabel(status, page);
	availLabel->setStyleSheet(QStringLiteral("color: #888;"));
	layout->addWidget(availLabel);

	return page;
}

QWidget *SourcePicker::build_ndi_tab()
{
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	auto *heading = new QLabel(amv::text("AMVPlugin.SourcePicker.NDI.Heading"), page);
	{
		QFont f = heading->font();
		f.setBold(true);
		heading->setFont(f);
	}
	layout->addWidget(heading);

	/* Provider availability gate. If DistroAV isn't installed there's no
	 * point showing the discovery list \u2014 it will be empty forever \u2014 so
	 * fall back to the placeholder body that explains how to fix it. */
	const auto &reg = SignalProviderRegistry::instance();
	const auto *p = reg.find(SignalProviderType::Ndi);
	if (!p || !p->is_available()) {
		auto *body = new QLabel(amv::text("AMVPlugin.SourcePicker.NDI.Unavailable"), page);
		body->setWordWrap(true);
		layout->addWidget(body);
		layout->addStretch(1);
		return page;
	}

	auto *body = new QLabel(amv::text("AMVPlugin.SourcePicker.NDI.Description"), page);
	body->setWordWrap(true);
	layout->addWidget(body);

	auto *scroll = new QScrollArea(page);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	ndi_form_ = new NdiSourceForm();
	scroll->setWidget(ndi_form_);
	layout->addWidget(scroll, 1);

	auto *availLabel = new QLabel(amv::text("AMVPlugin.SourcePicker.NDI.ProviderAvailable"), page);
	availLabel->setStyleSheet(QStringLiteral("color: #888;"));
	layout->addWidget(availLabel);

	return page;
}

QWidget *SourcePicker::build_spout_tab()
{
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	auto *heading = new QLabel(amv::text("AMVPlugin.SourcePicker.Spout.Heading"), page);
	{
		QFont f = heading->font();
		f.setBold(true);
		heading->setFont(f);
	}
	layout->addWidget(heading);

	const auto &reg = SignalProviderRegistry::instance();
	const auto *p = reg.find(SignalProviderType::Spout);
	if (!p || !p->is_available()) {
		/* Two distinct unavailable cases share this branch:
		 *   - macOS / Linux: Spout is Windows-only (no obs-spout2
		 *     build exists), so we surface that as a permanent state
		 *     rather than a "please install" prompt the user can
		 *     never act on.
		 *   - Windows without obs-spout2: temporary, user can
		 *     install + restart OBS.
		 * Both paths come from the provider's own unavailable_reason
		 * so the UI text stays in sync with multiview-window-status
		 * and edit-source-dialog without per-call duplication. */
		QString reason;
		if (p) {
			const std::string r = p->unavailable_reason();
			reason = QString::fromStdString(r);
		}
		if (reason.isEmpty()) {
			if (!signal_provider_supported_on_platform(SignalProviderType::Spout))
				reason = QString::fromUtf8(
					signal_provider_unsupported_platform_reason(SignalProviderType::Spout));
			else
				reason = amv::text("AMVPlugin.SourcePicker.Spout.Unavailable");
		}
		auto *body = new QLabel(reason, page);
		body->setWordWrap(true);
		layout->addWidget(body);
		layout->addStretch(1);
		return page;
	}

	auto *body = new QLabel(amv::text("AMVPlugin.SourcePicker.Spout.Description"), page);
	body->setWordWrap(true);
	layout->addWidget(body);

	auto *scroll = new QScrollArea(page);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	spout_form_ = new SpoutSenderForm();
	scroll->setWidget(spout_form_);
	layout->addWidget(scroll, 1);

	auto *availLabel = new QLabel(amv::text("AMVPlugin.SourcePicker.Spout.ProviderAvailable"), page);
	availLabel->setStyleSheet(QStringLiteral("color: #888;"));
	layout->addWidget(availLabel);

	return page;
}

QWidget *SourcePicker::build_vlc_tab()
{
	auto *page = new QWidget(this);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	auto *heading = new QLabel(amv::text("AMVPlugin.SourcePicker.VLC.Heading"), page);
	{
		QFont f = heading->font();
		f.setBold(true);
		heading->setFont(f);
	}
	layout->addWidget(heading);

	const auto &reg = SignalProviderRegistry::instance();
	const auto *p = reg.find(SignalProviderType::Vlc);
	if (!p || !p->is_available()) {
		/* vlc-video is a conditionally built plugin: some OBS
		 * installs (especially custom / minimal builds) ship without
		 * it. Surface that as a clear gate rather than letting the
		 * user fill a playlist that can never be created. */
		auto *body = new QLabel(amv::text("AMVPlugin.SourcePicker.VLC.Unavailable"), page);
		body->setWordWrap(true);
		layout->addWidget(body);
		layout->addStretch(1);
		return page;
	}

	auto *body = new QLabel(amv::text("AMVPlugin.SourcePicker.VLC.Description"), page);
	body->setWordWrap(true);
	layout->addWidget(body);

	auto *scroll = new QScrollArea(page);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	vlc_form_ = new VlcMediaForm();
	scroll->setWidget(vlc_form_);
	layout->addWidget(scroll, 1);

	auto *availLabel = new QLabel(amv::text("AMVPlugin.SourcePicker.VLC.ProviderAvailable"), page);
	availLabel->setStyleSheet(QStringLiteral("color: #888;"));
	layout->addWidget(availLabel);

	return page;
}
