/*
OBS Advanced Multiview - External Output (Spout/NDI) settings dialog (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "external-output-settings-dialog.hpp"
#include "amv-i18n.hpp"
#include "multiview-output.hpp"

#include <obs.h>
#include <obs.hpp>
#include <media-io/video-io.h>

#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

namespace {

/* OBS global frame rate. base fps > 30 unlocks the Half divisor (e.g. 60->30,
 * 59.94->29.97); 30 and below only offer Full. */
double obs_base_fps()
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_den > 0)
		return (double)ovi.fps_num / (double)ovi.fps_den;
	return 60.0;
}

QString dims_suffix(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0)
		return QString();
	return QStringLiteral(" (%1×%2)").arg(w).arg(h);
}

} /* namespace */

ExternalOutputSettingsDialog::ExternalOutputSettingsDialog(QWidget *parent) : QDialog(parent)
{
	setup_ui();
}

void ExternalOutputSettingsDialog::setup_ui()
{
	setWindowTitle(amv::text("AMVPlugin.Output.Dialog.Title"));
	setMinimumWidth(360);

	auto *mainLayout = new QVBoxLayout(this);

	auto *tabs = new QTabWidget(this);

	const bool spoutAvailable = signal_provider_supported_on_platform(SignalProviderType::Spout);
	QString spoutReason;
	if (!spoutAvailable)
		spoutReason = QString::fromUtf8(signal_provider_unsupported_platform_reason(SignalProviderType::Spout));

	/* Spout shares only a GPU texture — no audio path (supportsAudio = false). */
	tabs->addTab(build_backend_tab(spout_, spoutAvailable, spoutReason, /*supportsAudio=*/false),
		     amv::text("AMVPlugin.Output.Tab.Spout"));

	/* NDI requires the runtime DLL to be installed; grey the tab out with a
	 * hint when it isn't (or when the plugin was built without NDI support). */
	const bool ndiAvailable = MultiviewOutputManager::ndi_supported();
	const QString ndiReason = ndiAvailable ? QString() : amv::text("AMVPlugin.Output.NDI.Unavailable");
	tabs->addTab(build_backend_tab(ndi_, ndiAvailable, ndiReason, /*supportsAudio=*/true),
		     amv::text("AMVPlugin.Output.Tab.NDI"));

	/* DeckLink needs OBS's decklink_output type registered (the OBS DeckLink
	 * plugin present); grey the tab out with a hint otherwise. */
	const bool deckAvailable = MultiviewOutputManager::decklink_supported();
	const QString deckReason = deckAvailable ? QString() : amv::text("AMVPlugin.Output.DeckLink.Unavailable");
	tabs->addTab(build_decklink_tab(decklink_, deckAvailable, deckReason),
		     amv::text("AMVPlugin.Output.Tab.DeckLink"));

	mainLayout->addWidget(tabs);

	auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
	mainLayout->addWidget(btnBox);
}

QWidget *ExternalOutputSettingsDialog::build_backend_tab(BackendWidgets &w, bool available,
							 const QString &unavailableReason, bool supportsAudio)
{
	auto *tab = new QWidget(this);
	auto *form = new QFormLayout(tab);

	w.enabled = new QCheckBox(amv::text("AMVPlugin.Output.Enable"), tab);
	form->addRow(w.enabled);

	/* Resolution mode + custom dimensions. */
	struct obs_video_info ovi;
	const bool haveOvi = obs_get_video_info(&ovi);

	w.resMode = new QComboBox(tab);
	w.resMode->addItem(amv::text("AMVPlugin.Output.Res.CanvasBase") +
				   (haveOvi ? dims_suffix(ovi.base_width, ovi.base_height) : QString()),
			   (int)OutputResolutionMode::CanvasBase);
	w.resMode->addItem(amv::text("AMVPlugin.Output.Res.ObsOutput") +
				   (haveOvi ? dims_suffix(ovi.output_width, ovi.output_height) : QString()),
			   (int)OutputResolutionMode::ObsOutput);

	/* OBS advanced streaming/recording encoder "Rescale Output". Only
	 * selectable when actually enabled in OBS; otherwise keep the row present
	 * but greyed out (per user request) so the capability is discoverable. */
	auto addRescaleItem = [&](const char *labelKey, const char *disabledTipKey, bool active, uint32_t rw,
				  uint32_t rh, OutputResolutionMode mode) {
		w.resMode->addItem(amv::text(labelKey) + (active ? dims_suffix(rw, rh) : QString()), (int)mode);
		if (active)
			return;
		const int idx = w.resMode->count() - 1;
		if (auto *model = qobject_cast<QStandardItemModel *>(w.resMode->model())) {
			if (QStandardItem *item = model->item(idx)) {
				item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
				item->setToolTip(amv::text(disabledTipKey));
			}
		}
	};

	uint32_t sw = 0, sh = 0;
	const bool streamActive = obs_stream_rescale_dimensions(sw, sh);
	addRescaleItem("AMVPlugin.Output.Res.StreamRescale", "AMVPlugin.Output.Res.StreamRescaleDisabled", streamActive,
		       sw, sh, OutputResolutionMode::ObsStreamRescale);

	uint32_t recw = 0, rech = 0;
	const bool recActive = obs_record_rescale_dimensions(recw, rech);
	addRescaleItem("AMVPlugin.Output.Res.RecordRescale", "AMVPlugin.Output.Res.RecordRescaleDisabled", recActive,
		       recw, rech, OutputResolutionMode::ObsRecordRescale);

	w.resMode->addItem(amv::text("AMVPlugin.Output.Res.Custom"), (int)OutputResolutionMode::Custom);
	form->addRow(amv::text("AMVPlugin.Output.Resolution"), w.resMode);

	w.customW = new QSpinBox(tab);
	w.customW->setRange(16, 7680);
	w.customW->setValue(1920);
	form->addRow(amv::text("AMVPlugin.Output.CustomWidth"), w.customW);

	w.customH = new QSpinBox(tab);
	w.customH->setRange(16, 4320);
	w.customH->setValue(1080);
	form->addRow(amv::text("AMVPlugin.Output.CustomHeight"), w.customH);

	/* Custom spinboxes only matter in Custom mode. */
	auto syncCustom = [&w]() {
		const bool custom = w.resMode->currentData().toInt() == (int)OutputResolutionMode::Custom;
		w.customW->setEnabled(custom);
		w.customH->setEnabled(custom);
	};
	connect(w.resMode, QOverload<int>::of(&QComboBox::currentIndexChanged), tab,
		[syncCustom](int) { syncCustom(); });
	syncCustom();

	/* Frame rate divisor. */
	const double base = obs_base_fps();
	w.fps = new QComboBox(tab);
	w.fps->addItem(amv::text("AMVPlugin.Output.Fps.Full").arg(QString::number(base, 'g', 5)), 1);
	if (base > 30.5)
		w.fps->addItem(amv::text("AMVPlugin.Output.Fps.Half").arg(QString::number(base / 2.0, 'g', 5)), 2);
	form->addRow(amv::text("AMVPlugin.Output.Framerate"), w.fps);

	/* Audio source — mirrors the VU meter's track selection (follow streaming /
	 * manual track 1..6). Spout shares only a GPU texture and has no audio path,
	 * so its controls are present but disabled (supportsAudio == false). */
	w.audioMode = new QComboBox(tab);
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.FollowStreaming"),
			     (int)OutputAudioMode::FollowStreaming);
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.ManualTrack"), (int)OutputAudioMode::ManualTrack);
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.None"), (int)OutputAudioMode::None);
	form->addRow(amv::text("AMVPlugin.Output.Audio"), w.audioMode);

	w.audioTrack = new QSpinBox(tab);
	w.audioTrack->setRange(1, 6);
	w.audioTrack->setPrefix(amv::text("AMVPlugin.Output.Audio.TrackPrefix"));
	form->addRow(amv::text("AMVPlugin.Output.Audio.ManualTrackLabel"), w.audioTrack);

	/* Manual track only matters in ManualTrack mode; whole block off for Spout. */
	auto syncAudio = [&w, supportsAudio]() {
		const bool manual = w.audioMode->currentData().toInt() == (int)OutputAudioMode::ManualTrack;
		w.audioMode->setEnabled(supportsAudio);
		w.audioTrack->setEnabled(supportsAudio && manual);
	};
	connect(w.audioMode, QOverload<int>::of(&QComboBox::currentIndexChanged), tab,
		[syncAudio](int) { syncAudio(); });
	syncAudio();
	if (!supportsAudio)
		w.audioMode->setToolTip(amv::text("AMVPlugin.Output.Audio.Unsupported"));

	if (!available) {
		tab->setEnabled(false);
		if (!unavailableReason.isEmpty())
			tab->setToolTip(unavailableReason);
	}

	return tab;
}

/* Fill `w.deckMode` with the output modes of `deviceHash` that match the canvas
 * frame rate. We drive OBS's own decklink_output property machinery: query its
 * properties, write the device into a scratch obs_data, fire the device's
 * modified callback (which clears + fills the mode list, already fps-filtered —
 * decklink_output requires an exactly equal frame rate), then copy the items. */
void ExternalOutputSettingsDialog::populate_decklink_modes(const BackendWidgets &w, const QString &deviceHash)
{
	if (!w.deckMode)
		return;
	w.deckMode->clear();
	if (deviceHash.isEmpty())
		return;

	obs_properties_t *props = obs_get_output_properties("decklink_output");
	if (!props)
		return;

	obs_property_t *deviceProp = obs_properties_get(props, "device_hash");
	obs_property_t *modeProp = obs_properties_get(props, "mode_id");
	if (deviceProp && modeProp) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "device_hash", deviceHash.toUtf8().constData());
		/* Runs decklink_output_device_changed: fills mode_id filtered by fps. */
		obs_property_modified(deviceProp, settings);

		const size_t count = obs_property_list_item_count(modeProp);
		for (size_t i = 0; i < count; i++) {
			const char *modeName = obs_property_list_item_name(modeProp, i);
			const long long modeId = obs_property_list_item_int(modeProp, i);
			w.deckMode->addItem(QString::fromUtf8(modeName ? modeName : ""),
					    QVariant::fromValue<qlonglong>(modeId));
		}
	}

	obs_properties_destroy(props);
}

QWidget *ExternalOutputSettingsDialog::build_decklink_tab(BackendWidgets &w, bool available,
							  const QString &unavailableReason)
{
	auto *tab = new QWidget(this);
	auto *form = new QFormLayout(tab);

	w.enabled = new QCheckBox(amv::text("AMVPlugin.Output.Enable"), tab);
	form->addRow(w.enabled);

	/* Device list (data = device_hash). Enumerated from the OBS decklink_output
	 * properties; empty when no DeckLink device is present. */
	w.deckDevice = new QComboBox(tab);
	w.deckMode = new QComboBox(tab);

	if (available) {
		obs_properties_t *props = obs_get_output_properties("decklink_output");
		if (props) {
			obs_property_t *deviceProp = obs_properties_get(props, "device_hash");
			if (deviceProp) {
				const size_t count = obs_property_list_item_count(deviceProp);
				for (size_t i = 0; i < count; i++) {
					const char *name = obs_property_list_item_name(deviceProp, i);
					const char *hash = obs_property_list_item_string(deviceProp, i);
					w.deckDevice->addItem(QString::fromUtf8(name ? name : ""),
							      QString::fromUtf8(hash ? hash : ""));
				}
			}
			obs_properties_destroy(props);
		}
	}

	form->addRow(amv::text("AMVPlugin.Output.DeckLink.Device"), w.deckDevice);
	form->addRow(amv::text("AMVPlugin.Output.DeckLink.Mode"), w.deckMode);

	/* Repopulate modes whenever the device changes. */
	connect(w.deckDevice, QOverload<int>::of(&QComboBox::currentIndexChanged), tab,
		[&w](int) { populate_decklink_modes(w, w.deckDevice->currentData().toString()); });

	/* Populate modes for the initially selected device (if any). */
	if (available && w.deckDevice->count() > 0)
		populate_decklink_modes(w, w.deckDevice->currentData().toString());

	/* Keyer: Disabled / External / Internal (0 / 1 / 2). */
	w.deckKeyer = new QComboBox(tab);
	w.deckKeyer->addItem(amv::text("AMVPlugin.Output.DeckLink.Keyer.Disabled"), 0);
	w.deckKeyer->addItem(amv::text("AMVPlugin.Output.DeckLink.Keyer.External"), 1);
	w.deckKeyer->addItem(amv::text("AMVPlugin.Output.DeckLink.Keyer.Internal"), 2);
	form->addRow(amv::text("AMVPlugin.Output.DeckLink.Keyer"), w.deckKeyer);

	w.deckForceSdr = new QCheckBox(amv::text("AMVPlugin.Output.DeckLink.ForceSDR"), tab);
	form->addRow(w.deckForceSdr);

	/* Audio (DeckLink embeds SDI/HDMI audio, so the full audio path applies). */
	w.audioMode = new QComboBox(tab);
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.FollowStreaming"),
			     (int)OutputAudioMode::FollowStreaming);
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.ManualTrack"), (int)OutputAudioMode::ManualTrack);
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.None"), (int)OutputAudioMode::None);
	form->addRow(amv::text("AMVPlugin.Output.Audio"), w.audioMode);

	w.audioTrack = new QSpinBox(tab);
	w.audioTrack->setRange(1, 6);
	w.audioTrack->setPrefix(amv::text("AMVPlugin.Output.Audio.TrackPrefix"));
	form->addRow(amv::text("AMVPlugin.Output.Audio.ManualTrackLabel"), w.audioTrack);

	auto syncAudio = [&w]() {
		const bool manual = w.audioMode->currentData().toInt() == (int)OutputAudioMode::ManualTrack;
		w.audioTrack->setEnabled(manual);
	};
	connect(w.audioMode, QOverload<int>::of(&QComboBox::currentIndexChanged), tab,
		[syncAudio](int) { syncAudio(); });
	syncAudio();

	/* A note that the mode list is already filtered to the canvas frame rate. */
	auto *fpsNote = new QLabel(amv::text("AMVPlugin.Output.DeckLink.FpsNote"), tab);
	fpsNote->setWordWrap(true);
	form->addRow(fpsNote);

	if (!available) {
		tab->setEnabled(false);
		if (!unavailableReason.isEmpty())
			tab->setToolTip(unavailableReason);
	}

	return tab;
}

void ExternalOutputSettingsDialog::load_decklink(const BackendWidgets &w, const OutputBackendSettings &s)
{
	w.enabled->setChecked(s.enabled);

	/* Device: select the saved hash if still present; else leave on the first. */
	int devIdx = w.deckDevice->findData(QString::fromStdString(s.deckDeviceHash));
	if (devIdx >= 0)
		w.deckDevice->setCurrentIndex(devIdx);

	/* currentIndexChanged already repopulated modes for the selected device; make
	 * sure they match the current selection (covers the no-change case too). */
	populate_decklink_modes(w, w.deckDevice->currentData().toString());
	int modeIdx = w.deckMode->findData(QVariant::fromValue<qlonglong>(s.deckModeId));
	if (modeIdx >= 0)
		w.deckMode->setCurrentIndex(modeIdx);

	int keyerIdx = w.deckKeyer->findData(s.deckKeyer);
	w.deckKeyer->setCurrentIndex(keyerIdx >= 0 ? keyerIdx : 0);

	w.deckForceSdr->setChecked(s.deckForceSdr);

	int audIdx = w.audioMode->findData((int)s.audioMode);
	w.audioMode->setCurrentIndex(audIdx >= 0 ? audIdx : 0);
	w.audioTrack->setValue(s.audioTrackIndex);
}

OutputBackendSettings ExternalOutputSettingsDialog::read_decklink(const BackendWidgets &w)
{
	OutputBackendSettings s;
	s.enabled = w.enabled->isChecked();
	s.deckDeviceHash = w.deckDevice->currentData().toString().toStdString();
	s.deckModeId = w.deckMode->currentData().isValid() ? w.deckMode->currentData().toLongLong() : 0;
	s.deckKeyer = w.deckKeyer->currentData().toInt();
	s.deckForceSdr = w.deckForceSdr->isChecked();
	s.audioMode = (OutputAudioMode)w.audioMode->currentData().toInt();
	s.audioTrackIndex = w.audioTrack->value();

	/* Lock the composition to the selected mode's native raster (§3.4): resMode
	 * stays Custom and customWidth/customHeight carry the raster, so both the
	 * dialog and resolve_output_dimensions agree and there is zero scaling. A
	 * scratch output reports the raster via its video conversion. */
	s.resMode = OutputResolutionMode::Custom;
	s.fpsDivisor = 1; /* DeckLink runs at full canvas fps (FPS must match exactly) */

	/* H2: never persist enabled + device-but-no-mode. mode_id 0 (empty mode
	 * combo) would crash decklink_output_create (null DeckLinkDeviceMode deref),
	 * so refuse to enable instead of saving a config that can't start. */
	if (s.enabled && !s.deckDeviceHash.empty() && s.deckModeId == 0)
		s.enabled = false;

	/* Probe the selected device+mode for its native raster only when a real mode
	 * is chosen — a probe with mode_id 0 would hit the same crash as the live
	 * output. */
	if (!s.deckDeviceHash.empty() && s.deckModeId != 0) {
		OBSDataAutoRelease probeSettings = obs_data_create();
		obs_data_set_string(probeSettings, "device_hash", s.deckDeviceHash.c_str());
		obs_data_set_int(probeSettings, "mode_id", s.deckModeId);
		obs_data_set_bool(probeSettings, "force_sdr", s.deckForceSdr);
		OBSOutputAutoRelease probe =
			obs_output_create("decklink_output", "amv-decklink-probe", probeSettings, nullptr);
		if (probe) {
			const struct video_scale_info *conv = obs_output_get_video_conversion(probe);
			if (conv && conv->width > 0 && conv->height > 0) {
				s.customWidth = conv->width;
				s.customHeight = conv->height;
			}
		}
	}
	return s;
}

void ExternalOutputSettingsDialog::load_backend(const BackendWidgets &w, const OutputBackendSettings &s)
{
	w.enabled->setChecked(s.enabled);

	int resIdx = w.resMode->findData((int)s.resMode);
	w.resMode->setCurrentIndex(resIdx >= 0 ? resIdx : 0);

	w.customW->setValue((int)s.customWidth);
	w.customH->setValue((int)s.customHeight);

	int fpsIdx = w.fps->findData(s.fpsDivisor);
	w.fps->setCurrentIndex(fpsIdx >= 0 ? fpsIdx : 0); /* Half may be absent (<=30 fps) -> Full */

	int audIdx = w.audioMode->findData((int)s.audioMode);
	w.audioMode->setCurrentIndex(audIdx >= 0 ? audIdx : 0);
	w.audioTrack->setValue(s.audioTrackIndex);
}

OutputBackendSettings ExternalOutputSettingsDialog::read_backend(const BackendWidgets &w)
{
	OutputBackendSettings s;
	s.enabled = w.enabled->isChecked();
	s.resMode = (OutputResolutionMode)w.resMode->currentData().toInt();
	s.customWidth = (uint32_t)w.customW->value();
	s.customHeight = (uint32_t)w.customH->value();
	s.fpsDivisor = w.fps->currentData().toInt() == 2 ? 2 : 1;
	s.audioMode = (OutputAudioMode)w.audioMode->currentData().toInt();
	s.audioTrackIndex = w.audioTrack->value();
	return s;
}

void ExternalOutputSettingsDialog::set_settings(const InstanceOutputSettings &s)
{
	load_backend(spout_, s.spout);
	load_backend(ndi_, s.ndi);
	load_decklink(decklink_, s.decklink);
}

InstanceOutputSettings ExternalOutputSettingsDialog::get_settings() const
{
	InstanceOutputSettings s;
	s.spout = read_backend(spout_);
	s.ndi = read_backend(ndi_);
	s.decklink = read_decklink(decklink_);
	return s;
}
