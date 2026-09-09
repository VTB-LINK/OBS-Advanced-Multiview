/*
OBS Advanced Multiview - Provider settings form factory

The single place that maps a SignalProviderType to its concrete Qt
settings form. Both hosting dialogs (EditSourceDialog and SourcePicker)
create their form through make_provider_settings_form(), so neither
carries its own type -> form switch: a new provider's form dispatch is
one case here (a dialog may still carry other per-provider bits).

Providers without a settings form (e.g. the reserved WebRTC slot) return
nullptr; the dialogs then fall back to their "no editable form" path.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "provider-settings-forms.hpp"

ProviderSettingsForm *make_provider_settings_form(SignalProviderType type, QWidget *parent)
{
	switch (type) {
	case SignalProviderType::Ffmpeg:
		return new FfmpegMediaForm(parent);
	case SignalProviderType::Ndi:
		return new NdiSourceForm(parent);
	case SignalProviderType::Spout:
		return new SpoutSenderForm(parent);
	case SignalProviderType::Vlc:
		return new VlcMediaForm(parent);
	default:
		return nullptr;
	}
}
