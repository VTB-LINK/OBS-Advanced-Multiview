/*
OBS Advanced Multiview - Edit Source dialog implementation

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "edit-source-dialog.hpp"
#include "amv-i18n.hpp"
#include "provider-settings-forms.hpp"
#include "signal-provider.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

EditSourceDialog::EditSourceDialog(const SignalConfig &cfg, QWidget *parent) : QDialog(parent), provider_(cfg.provider)
{
	setWindowTitle(amv::text("AMVPlugin.EditSource.Title"));
	setModal(true);
	setMinimumSize(420, 480);
	resize(500, 600);

	auto *root = new QVBoxLayout(this);

	/* Heading line tells the user which provider this dialog is for so
	 * the surface stays clear when we add NDI / Spout / VLC variants. */
	QString title;
	switch (cfg.provider) {
	case SignalProviderType::Ffmpeg:
		title = amv::text("AMVPlugin.EditSource.Heading.FFmpeg");
		break;
	case SignalProviderType::Ndi:
		title = amv::text("AMVPlugin.EditSource.Heading.NDI");
		break;
	case SignalProviderType::Spout:
		title = amv::text("AMVPlugin.EditSource.Heading.Spout");
		break;
	case SignalProviderType::Vlc:
		title = amv::text("AMVPlugin.EditSource.Heading.VLC");
		break;
	default:
		title = amv::text("AMVPlugin.EditSource.Heading.External");
		break;
	}
	auto *heading = new QLabel(title, this);
	{
		QFont f = heading->font();
		f.setBold(true);
		heading->setFont(f);
	}
	root->addWidget(heading);

	/* Phase 3 / M6.2: when the cell's provider is not available in this
	 * OBS install (e.g. user opened a config saved in another OBS that
	 * had DistroAV but this one doesn't), surface that prominently and
	 * make Save unsafe. The form still loads so the user can inspect
	 * the existing config; we just refuse to write a new value through
	 * an absent provider. */
	bool provider_available = true;
	bool provider_platform_supported = true;
	QString provider_unavailable_reason;
	QString provider_unavailable_guidance;
	if (cfg.provider != SignalProviderType::Unknown && !signal_provider_is_internal(cfg.provider)) {
		provider_platform_supported = signal_provider_supported_on_platform(cfg.provider);
		if (!provider_platform_supported) {
			provider_available = false;
			provider_unavailable_reason =
				QString::fromUtf8(signal_provider_unsupported_platform_reason(cfg.provider));
			provider_unavailable_guidance = amv::text("AMVPlugin.EditSource.ProviderUnsupportedGuidance");
		} else {
			const auto *p = SignalProviderRegistry::instance().find(cfg.provider);
			if (!p || !p->is_available()) {
				provider_available = false;
				if (p) {
					const std::string r = p->unavailable_reason();
					provider_unavailable_reason = QString::fromStdString(r);
				}
				if (provider_unavailable_reason.isEmpty())
					provider_unavailable_reason =
						amv::text("AMVPlugin.EditSource.RequiredPluginMissing");
				provider_unavailable_guidance = amv::text("AMVPlugin.EditSource.PluginMissingGuidance");
			}
		}
	}
	if (!provider_available) {
		auto *banner = new QLabel(QStringLiteral("\u26A0  %1\n\n%2")
						  .arg(provider_unavailable_reason, provider_unavailable_guidance),
					  this);
		banner->setWordWrap(true);
		banner->setStyleSheet(QStringLiteral("color: #FFCC66; padding: 6px; "
						     "background: rgba(64,16,96,128); border-radius: 4px;"));
		root->addWidget(banner);
	}

	if (cfg.provider == SignalProviderType::Ffmpeg) {
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		ffmpeg_form_ = new FfmpegMediaForm();
		ffmpeg_form_->load_from(cfg);
		scroll->setWidget(ffmpeg_form_);
		root->addWidget(scroll, 1);
	} else if (cfg.provider == SignalProviderType::Ndi) {
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		ndi_form_ = new NdiSourceForm();
		ndi_form_->load_from(cfg);
		scroll->setWidget(ndi_form_);
		root->addWidget(scroll, 1);
	} else if (cfg.provider == SignalProviderType::Spout) {
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		spout_form_ = new SpoutSenderForm();
		spout_form_->load_from(cfg);
		if (!provider_platform_supported)
			spout_form_->setEnabled(false);
		scroll->setWidget(spout_form_);
		root->addWidget(scroll, 1);
	} else if (cfg.provider == SignalProviderType::Vlc) {
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		vlc_form_ = new VlcMediaForm();
		vlc_form_->load_from(cfg);
		scroll->setWidget(vlc_form_);
		root->addWidget(scroll, 1);
	} else {
		/* Other external providers don't have an editor yet; their
		 * own milestones (M6.3 Spout, M6.4 VLC) will add sibling
		 * forms. Until then, show a plain message so the user knows
		 * nothing was saved. */
		auto *msg = new QLabel(amv::text("AMVPlugin.EditSource.NotImplemented"), this);
		msg->setWordWrap(true);
		root->addWidget(msg);
	}

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	if ((cfg.provider != SignalProviderType::Ffmpeg && cfg.provider != SignalProviderType::Ndi &&
	     cfg.provider != SignalProviderType::Spout && cfg.provider != SignalProviderType::Vlc) ||
	    !provider_available) {
		/* No editable form for unsupported providers yet, OR the cell's
		 * provider is missing in this OBS install. Either way disable
		 * Save so cancel is the only safe action. */
		QPushButton *okBtn = buttons->button(QDialogButtonBox::Ok);
		if (okBtn)
			okBtn->setEnabled(false);
	}
	root->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &EditSourceDialog::on_accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void EditSourceDialog::on_accept()
{
	if (provider_ == SignalProviderType::Ffmpeg && ffmpeg_form_) {
		if (!ffmpeg_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.MediaRequired"),
						 ffmpeg_form_->invalid_reason());
			return;
		}
	} else if (provider_ == SignalProviderType::Ndi && ndi_form_) {
		if (!ndi_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.NDIRequired"),
						 ndi_form_->invalid_reason());
			return;
		}
	} else if (provider_ == SignalProviderType::Spout && spout_form_) {
		if (!spout_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.SpoutRequired"),
						 spout_form_->invalid_reason());
			return;
		}
	} else if (provider_ == SignalProviderType::Vlc && vlc_form_) {
		if (!vlc_form_->is_valid()) {
			QMessageBox::information(this, amv::text("AMVPlugin.EditSource.Error.PlaylistRequired"),
						 vlc_form_->invalid_reason());
			return;
		}
	}
	accept();
}

SignalConfig EditSourceDialog::signal_config() const
{
	if (provider_ == SignalProviderType::Ffmpeg && ffmpeg_form_)
		return ffmpeg_form_->to_signal_config();
	if (provider_ == SignalProviderType::Ndi && ndi_form_)
		return ndi_form_->to_signal_config();
	if (provider_ == SignalProviderType::Spout && spout_form_)
		return spout_form_->to_signal_config();
	if (provider_ == SignalProviderType::Vlc && vlc_form_)
		return vlc_form_->to_signal_config();
	return SignalConfig();
}
