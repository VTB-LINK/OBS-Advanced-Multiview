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
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

namespace {

/* OBS aja_output property ids (obs-studio/plugins/aja/aja-ui-props.hpp), used to
 * drive its property callbacks and read the enumerated lists — zero NTV2 SDK. */
constexpr const char *kAjaPropDevice = "ui_prop_device";
constexpr const char *kAjaPropOutput = "ui_prop_output";
constexpr const char *kAjaPropVideoFormat = "ui_prop_vid_fmt";
constexpr const char *kAjaPropPixelFormat = "ui_prop_pix_fmt";
constexpr const char *kAjaPropSDITransport = "ui_prop_sdi_transport";
constexpr const char *kAjaPropSDITransport4K = "ui_prop_sdi_transport_4k";

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

	/* AJA needs OBS's aja_output type registered (the OBS AJA plugin present, which
	 * itself requires a card at startup); grey the tab out with a hint otherwise. */
	const bool ajaAvailable = MultiviewOutputManager::aja_supported();
	const QString ajaReason = ajaAvailable ? QString() : amv::text("AMVPlugin.Output.AJA.Unavailable");
	tabs->addTab(build_aja_tab(aja_, ajaAvailable, ajaReason), amv::text("AMVPlugin.Output.Tab.AJA"));

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

void ExternalOutputSettingsDialog::load_decklink(const BackendWidgets &w, const OutputBackendSettings &common,
						 const DeckLinkBackendSettings &hw)
{
	w.enabled->setChecked(common.enabled);

	/* Device: select the saved hash if still present; else leave on the first. */
	int devIdx = w.deckDevice->findData(QString::fromStdString(hw.deviceHash));
	if (devIdx >= 0)
		w.deckDevice->setCurrentIndex(devIdx);

	/* currentIndexChanged already repopulated modes for the selected device; make
	 * sure they match the current selection (covers the no-change case too). */
	populate_decklink_modes(w, w.deckDevice->currentData().toString());
	int modeIdx = w.deckMode->findData(QVariant::fromValue<qlonglong>(hw.modeId));
	if (modeIdx >= 0)
		w.deckMode->setCurrentIndex(modeIdx);

	int keyerIdx = w.deckKeyer->findData(hw.keyer);
	w.deckKeyer->setCurrentIndex(keyerIdx >= 0 ? keyerIdx : 0);

	w.deckForceSdr->setChecked(hw.forceSdr);

	int audIdx = w.audioMode->findData((int)common.audioMode);
	w.audioMode->setCurrentIndex(audIdx >= 0 ? audIdx : 0);
	w.audioTrack->setValue(common.audioTrackIndex);
}

void ExternalOutputSettingsDialog::read_decklink(const BackendWidgets &w, OutputBackendSettings &common,
						 DeckLinkBackendSettings &hw)
{
	common = OutputBackendSettings{};
	hw = DeckLinkBackendSettings{};

	common.enabled = w.enabled->isChecked();
	hw.deviceHash = w.deckDevice->currentData().toString().toStdString();
	hw.modeId = w.deckMode->currentData().isValid() ? w.deckMode->currentData().toLongLong() : 0;
	hw.keyer = w.deckKeyer->currentData().toInt();
	hw.forceSdr = w.deckForceSdr->isChecked();
	common.audioMode = (OutputAudioMode)w.audioMode->currentData().toInt();
	common.audioTrackIndex = w.audioTrack->value();

	/* Lock the composition to the selected mode's native raster (§3.4): resMode
	 * stays Custom and customWidth/customHeight carry the raster, so both the
	 * dialog and resolve_output_dimensions agree and there is zero scaling. A
	 * scratch output reports the raster via its video conversion. */
	common.resMode = OutputResolutionMode::Custom;
	common.fpsDivisor = 1; /* DeckLink runs at full canvas fps (FPS must match exactly) */

	/* H2: never persist enabled + device-but-no-mode. mode_id 0 (empty mode
	 * combo) would crash decklink_output_create (null DeckLinkDeviceMode deref),
	 * so refuse to enable instead of saving a config that can't start. */
	if (common.enabled && !hw.deviceHash.empty() && hw.modeId == 0)
		common.enabled = false;

	/* Probe the selected device+mode for its native raster only when a real mode
	 * is chosen — a probe with mode_id 0 would hit the same crash as the live
	 * output. */
	if (!hw.deviceHash.empty() && hw.modeId != 0) {
		OBSDataAutoRelease probeSettings = obs_data_create();
		obs_data_set_string(probeSettings, "device_hash", hw.deviceHash.c_str());
		obs_data_set_int(probeSettings, "mode_id", hw.modeId);
		obs_data_set_bool(probeSettings, "force_sdr", hw.forceSdr);
		OBSOutputAutoRelease probe =
			obs_output_create("decklink_output", "amv-decklink-probe", probeSettings, nullptr);
		if (probe) {
			const struct video_scale_info *conv = obs_output_get_video_conversion(probe);
			if (conv && conv->width > 0 && conv->height > 0) {
				common.customWidth = conv->width;
				common.customHeight = conv->height;
			}
		}
	}
}

/* Refill the io / videoFormat / pixelFormat / SDI transport combos for `cardID` by
 * firing OBS's aja_output device-changed callback (which enumerates the card and
 * fills those lists, videoFormat already fps-filtered) and copying the items. Zero
 * NTV2 SDK. Change signals on the refilled combos are blocked so refilling doesn't
 * cascade into a visibility refresh against half-built lists; callers refresh once
 * afterwards. */
void ExternalOutputSettingsDialog::populate_aja_lists(const BackendWidgets &w, const QString &cardID)
{
	if (!w.ajaIo || !w.ajaVideoFormat || !w.ajaPixelFormat || !w.ajaSdiTransport || !w.ajaSdi4kTransport)
		return;

	const QSignalBlocker bIo(w.ajaIo);
	const QSignalBlocker bVf(w.ajaVideoFormat);
	const QSignalBlocker bPf(w.ajaPixelFormat);
	const QSignalBlocker bSdi(w.ajaSdiTransport);
	const QSignalBlocker bSdi4k(w.ajaSdi4kTransport);

	w.ajaIo->clear();
	w.ajaVideoFormat->clear();
	w.ajaPixelFormat->clear();
	w.ajaSdiTransport->clear();
	w.ajaSdi4kTransport->clear();

	if (cardID.isEmpty())
		return;

	obs_properties_t *props = obs_get_output_properties("aja_output");
	if (!props)
		return;

	obs_property_t *deviceProp = obs_properties_get(props, kAjaPropDevice);
	obs_property_t *ioProp = obs_properties_get(props, kAjaPropOutput);
	obs_property_t *vfProp = obs_properties_get(props, kAjaPropVideoFormat);
	obs_property_t *pfProp = obs_properties_get(props, kAjaPropPixelFormat);
	obs_property_t *sdiProp = obs_properties_get(props, kAjaPropSDITransport);
	obs_property_t *sdi4kProp = obs_properties_get(props, kAjaPropSDITransport4K);
	if (deviceProp && ioProp && vfProp && pfProp && sdiProp && sdi4kProp) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, kAjaPropDevice, cardID.toUtf8().constData());
		/* Runs aja_output_device_changed: fills the five dependent lists. */
		obs_property_modified(deviceProp, settings);

		auto copyInt = [](obs_property_t *p, QComboBox *combo) {
			const size_t n = obs_property_list_item_count(p);
			for (size_t i = 0; i < n; i++) {
				const char *name = obs_property_list_item_name(p, i);
				const long long v = obs_property_list_item_int(p, i);
				combo->addItem(QString::fromUtf8(name ? name : ""), QVariant::fromValue<qlonglong>(v));
			}
		};
		copyInt(ioProp, w.ajaIo);
		copyInt(vfProp, w.ajaVideoFormat);
		copyInt(pfProp, w.ajaPixelFormat);
		copyInt(sdiProp, w.ajaSdiTransport);
		copyInt(sdi4kProp, w.ajaSdi4kTransport);
	}

	obs_properties_destroy(props);
}

/* Mirror OBS's own SDITransport / SDITransport4K visibility onto the Qt rows. We
 * fire aja_output_device_changed with the current device + io + videoFormat; it
 * ends by calling update_sdi_transport_and_sdi_transport_4k, which sets each SDI
 * property's visible flag (SDITransport <=> io is SDI; SDITransport4K <=> io is SDI
 * AND format is 4K). We read those flags back with obs_property_visible and never
 * compute them ourselves (that would need the SDK). */
void ExternalOutputSettingsDialog::refresh_aja_sdi_visibility(const BackendWidgets &w)
{
	if (!w.ajaForm || !w.ajaSdiTransport || !w.ajaSdi4kTransport)
		return;

	bool sdiVisible = false;
	bool sdi4kVisible = false;

	const QString cardID = w.ajaDevice ? w.ajaDevice->currentData().toString() : QString();
	if (!cardID.isEmpty()) {
		obs_properties_t *props = obs_get_output_properties("aja_output");
		if (props) {
			obs_property_t *deviceProp = obs_properties_get(props, kAjaPropDevice);
			obs_property_t *sdiProp = obs_properties_get(props, kAjaPropSDITransport);
			obs_property_t *sdi4kProp = obs_properties_get(props, kAjaPropSDITransport4K);
			if (deviceProp && sdiProp && sdi4kProp) {
				const long long io = w.ajaIo->currentData().isValid()
							     ? w.ajaIo->currentData().toLongLong()
							     : kAjaIoSelectionInvalid;
				const long long vf = w.ajaVideoFormat->currentData().isValid()
							     ? w.ajaVideoFormat->currentData().toLongLong()
							     : kAjaVideoFormatUnknown;
				OBSDataAutoRelease settings = obs_data_create();
				obs_data_set_string(settings, kAjaPropDevice, cardID.toUtf8().constData());
				obs_data_set_int(settings, kAjaPropOutput, io);
				obs_data_set_int(settings, kAjaPropVideoFormat, vf);
				obs_property_modified(deviceProp, settings);
				sdiVisible = obs_property_visible(sdiProp);
				sdi4kVisible = obs_property_visible(sdi4kProp);
			}
			obs_properties_destroy(props);
		}
	}

	/* sdi4kVisible already folds in "io is SDI" (OBS computes it as is_sdi && 4K). */
	w.ajaForm->setRowVisible(w.ajaSdiTransport, sdiVisible);
	w.ajaForm->setRowVisible(w.ajaSdi4kTransport, sdi4kVisible);
}

QWidget *ExternalOutputSettingsDialog::build_aja_tab(BackendWidgets &w, bool available,
						     const QString &unavailableReason)
{
	auto *tab = new QWidget(this);
	auto *form = new QFormLayout(tab);
	w.ajaForm = form;

	w.enabled = new QCheckBox(amv::text("AMVPlugin.Output.Enable"), tab);
	form->addRow(w.enabled);

	/* Device list (data = cardID string). Enumerated from the aja_output device
	 * property; empty when no AJA device/plugin is present. */
	w.ajaDevice = new QComboBox(tab);
	if (available) {
		obs_properties_t *props = obs_get_output_properties("aja_output");
		if (props) {
			obs_property_t *deviceProp = obs_properties_get(props, kAjaPropDevice);
			if (deviceProp) {
				const size_t count = obs_property_list_item_count(deviceProp);
				for (size_t i = 0; i < count; i++) {
					const char *name = obs_property_list_item_name(deviceProp, i);
					const char *cardID = obs_property_list_item_string(deviceProp, i);
					w.ajaDevice->addItem(QString::fromUtf8(name ? name : ""),
							     QString::fromUtf8(cardID ? cardID : ""));
				}
			}
			obs_properties_destroy(props);
		}
	}
	form->addRow(amv::text("AMVPlugin.Output.AJA.Device"), w.ajaDevice);

	w.ajaIo = new QComboBox(tab);
	form->addRow(amv::text("AMVPlugin.Output.AJA.IOSelection"), w.ajaIo);
	w.ajaVideoFormat = new QComboBox(tab);
	form->addRow(amv::text("AMVPlugin.Output.AJA.VideoFormat"), w.ajaVideoFormat);
	w.ajaPixelFormat = new QComboBox(tab);
	form->addRow(amv::text("AMVPlugin.Output.AJA.PixelFormat"), w.ajaPixelFormat);
	w.ajaSdiTransport = new QComboBox(tab);
	form->addRow(amv::text("AMVPlugin.Output.AJA.SDITransport"), w.ajaSdiTransport);
	w.ajaSdi4kTransport = new QComboBox(tab);
	form->addRow(amv::text("AMVPlugin.Output.AJA.SDITransport4K"), w.ajaSdi4kTransport);

	/* Device change: refill the dependent lists, then recompute SDI visibility. */
	connect(w.ajaDevice, QOverload<int>::of(&QComboBox::currentIndexChanged), tab, [&w](int) {
		populate_aja_lists(w, w.ajaDevice->currentData().toString());
		refresh_aja_sdi_visibility(w);
	});
	/* io / videoFormat changes only affect the two SDI rows' visibility. */
	connect(w.ajaIo, QOverload<int>::of(&QComboBox::currentIndexChanged), tab,
		[&w](int) { refresh_aja_sdi_visibility(w); });
	connect(w.ajaVideoFormat, QOverload<int>::of(&QComboBox::currentIndexChanged), tab,
		[&w](int) { refresh_aja_sdi_visibility(w); });

	/* Populate for the initially selected device (if any). */
	if (available && w.ajaDevice->count() > 0) {
		populate_aja_lists(w, w.ajaDevice->currentData().toString());
		refresh_aja_sdi_visibility(w);
	}

	/* Audio (AJA embeds SDI/HDMI audio, so the full audio path applies). */
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

	/* Note that the video format list is already filtered to the canvas frame rate. */
	auto *fpsNote = new QLabel(amv::text("AMVPlugin.Output.AJA.FpsNote"), tab);
	fpsNote->setWordWrap(true);
	form->addRow(fpsNote);

	if (!available) {
		tab->setEnabled(false);
		if (!unavailableReason.isEmpty())
			tab->setToolTip(unavailableReason);
	}

	return tab;
}

void ExternalOutputSettingsDialog::load_aja(const BackendWidgets &w, const OutputBackendSettings &common,
					    const AjaBackendSettings &hw)
{
	w.enabled->setChecked(common.enabled);

	/* Device: select the saved cardID if still present; else leave on the first. */
	int devIdx = w.ajaDevice->findData(QString::fromStdString(hw.cardID));
	if (devIdx >= 0)
		w.ajaDevice->setCurrentIndex(devIdx);

	/* Make sure the dependent lists match the current device (covers no-change). */
	populate_aja_lists(w, w.ajaDevice->currentData().toString());

	auto selectInt = [](QComboBox *combo, long long v) {
		const int idx = combo->findData(QVariant::fromValue<qlonglong>(v));
		if (idx >= 0)
			combo->setCurrentIndex(idx);
	};
	selectInt(w.ajaIo, hw.ioSelect);
	selectInt(w.ajaVideoFormat, hw.videoFormat);
	selectInt(w.ajaPixelFormat, hw.pixelFormat);
	selectInt(w.ajaSdiTransport, hw.sdiTransport);
	selectInt(w.ajaSdi4kTransport, hw.sdi4kTransport);

	refresh_aja_sdi_visibility(w);

	int audIdx = w.audioMode->findData((int)common.audioMode);
	w.audioMode->setCurrentIndex(audIdx >= 0 ? audIdx : 0);
	w.audioTrack->setValue(common.audioTrackIndex);
}

void ExternalOutputSettingsDialog::read_aja(const BackendWidgets &w, OutputBackendSettings &common,
					    AjaBackendSettings &hw)
{
	common = OutputBackendSettings{};
	hw = AjaBackendSettings{};

	common.enabled = w.enabled->isChecked();
	hw.cardID = w.ajaDevice->currentData().toString().toStdString();
	hw.ioSelect = w.ajaIo->currentData().isValid() ? w.ajaIo->currentData().toLongLong() : kAjaIoSelectionInvalid;
	hw.videoFormat = w.ajaVideoFormat->currentData().isValid() ? w.ajaVideoFormat->currentData().toLongLong()
								   : kAjaVideoFormatUnknown;
	hw.pixelFormat = w.ajaPixelFormat->currentData().isValid() ? w.ajaPixelFormat->currentData().toLongLong() : 0;
	hw.sdiTransport = w.ajaSdiTransport->currentData().isValid() ? w.ajaSdiTransport->currentData().toLongLong()
								     : 0;
	hw.sdi4kTransport =
		w.ajaSdi4kTransport->currentData().isValid() ? w.ajaSdi4kTransport->currentData().toLongLong() : 1;
	common.audioMode = (OutputAudioMode)w.audioMode->currentData().toInt();
	common.audioTrackIndex = w.audioTrack->value();

	/* AJA composes at the canvas size — no mode->raster lock (unlike DeckLink), so
	 * resMode stays CanvasBase and fpsDivisor is fixed to 1 (we always feed at
	 * canvas fps; aja soft-aligns any residual fps mismatch at the frame level). */
	common.resMode = OutputResolutionMode::CanvasBase;
	common.fpsDivisor = 1;

	/* Refuse to persist enabled + an invalid selection (mirrors DeckLink's H2
	 * guard). AJA can't crash on a bad selection, but a persisted enabled+garbage
	 * config would just loop the backend through create-fail + cooldown. No probe-
	 * create: an AJA probe would occupy a channel + spawn a thread and still not
	 * yield the raster (§4.3). pixelFormat validity here = "the combo has a real
	 * selection"; an empty combo (no device) gives an invalid QVariant, and a real
	 * dialog selection is always one of the two offered output pixel formats. */
	const bool pixelValid = w.ajaPixelFormat->currentData().isValid();
	if (common.enabled && (hw.cardID.empty() || hw.ioSelect == kAjaIoSelectionInvalid ||
			       hw.videoFormat == kAjaVideoFormatUnknown || !pixelValid))
		common.enabled = false;
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
	load_backend(spout_, s.at(OutputBackendKind::Spout));
	load_backend(ndi_, s.at(OutputBackendKind::Ndi));
	load_decklink(decklink_, s.at(OutputBackendKind::Decklink), s.decklink);
	load_aja(aja_, s.at(OutputBackendKind::Aja), s.aja);
	/* Carry through any out-of-build backend config the dialog can't edit. */
	preservedUnknownBackends_ = s.unknownBackends;
}

InstanceOutputSettings ExternalOutputSettingsDialog::get_settings() const
{
	InstanceOutputSettings s;
	s.backends[OutputBackendKind::Spout] = read_backend(spout_);
	s.backends[OutputBackendKind::Ndi] = read_backend(ndi_);
	OutputBackendSettings deckCommon;
	read_decklink(decklink_, deckCommon, s.decklink);
	s.backends[OutputBackendKind::Decklink] = deckCommon;
	OutputBackendSettings ajaCommon;
	read_aja(aja_, ajaCommon, s.aja);
	s.backends[OutputBackendKind::Aja] = ajaCommon;
	/* Preserve out-of-build backend config (see the member) so applying the
	 * dialog result doesn't drop it. */
	s.unknownBackends = preservedUnknownBackends_;
	return s;
}
