/*
OBS Advanced Multiview - External Output (Spout/NDI) settings dialog (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "external-output-settings-dialog.hpp"
#include "amv-i18n.hpp"
#include "multiview-output.hpp"

#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
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
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.FollowStreaming"), (int)OutputAudioMode::FollowStreaming);
	w.audioMode->addItem(amv::text("AMVPlugin.Output.Audio.ManualTrack"), (int)OutputAudioMode::ManualTrack);
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
}

InstanceOutputSettings ExternalOutputSettingsDialog::get_settings() const
{
	InstanceOutputSettings s;
	s.spout = read_backend(spout_);
	s.ndi = read_backend(ndi_);
	return s;
}
