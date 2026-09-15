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

EditSourceDialog::EditSourceDialog(const SignalConfig &cfg, ConfigManager *config, const std::string &self_uuid,
				   QWidget *parent)
	: QDialog(parent)
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
	case SignalProviderType::AmvInstance:
		title = amv::text("AMVPlugin.EditSource.Heading.AmvInstance");
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

	/* The type -> form choice lives in make_provider_settings_form(); this
	 * dialog no longer switches on the provider to pick a form. Spout's
	 * platform gate generalizes to "disable the form when the provider is
	 * unsupported on this platform" — provider_platform_supported is only
	 * ever false for Spout on non-Windows, so this is a no-op for the
	 * other providers (see signal_provider_supported_on_platform). */
	form_ = make_provider_settings_form(cfg.provider, nullptr, config, self_uuid);
	if (form_) {
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		form_->load_from(cfg);
		if (!provider_platform_supported)
			form_->setEnabled(false);
		scroll->setWidget(form_);
		root->addWidget(scroll, 1);
	} else {
		/* Providers without a settings form (e.g. the reserved WebRTC
		 * slot) fall through here. Show a plain message so the user
		 * knows nothing was saved. */
		auto *msg = new QLabel(amv::text("AMVPlugin.EditSource.NotImplemented"), this);
		msg->setWordWrap(true);
		root->addWidget(msg);
	}

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	if (!form_ || !provider_available) {
		/* No editable form for this provider yet (form_ == nullptr), OR
		 * the cell's provider is missing in this OBS install. Either way
		 * disable Save so cancel is the only safe action. */
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
	if (form_ && !form_->is_valid()) {
		QMessageBox::information(this, amv::text(form_->invalid_title_key()), form_->invalid_reason());
		return;
	}
	accept();
}

SignalConfig EditSourceDialog::signal_config() const
{
	return form_ ? form_->to_signal_config() : SignalConfig();
}
