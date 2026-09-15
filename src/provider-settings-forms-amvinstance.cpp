/*
OBS Advanced Multiview - nested AMV-instance provider settings form (issue #20 P3)

Split out of provider-settings-forms.cpp for maintainability. The form's class
declaration lives in provider-settings-forms.hpp; this TU only implements its
members. It configures our OWN hidden amv_instance_source (not a host plugin's
source): which other AMV instance to show, the resolution the target composes at,
full vs grid-only picture, and a disabled audio placeholder. Shared verbatim by
SourcePicker's AMV Instance tab and EditSourceDialog.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "provider-settings-forms.hpp"
#include "amv-i18n.hpp"
#include "config-manager.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <string>

namespace {

/* Qt::UserRole holds the target UUID; UserRole+1 holds the plain instance name
 * (for displayName), so decorations on the row text (current / missing suffix)
 * never leak into the persisted config. */
constexpr int kRoleUuid = Qt::UserRole;
constexpr int kRoleName = Qt::UserRole + 1;

constexpr int kCustomMin = 16;
constexpr int kCustomMax = (int)7680; /* mirrors AmvInstanceCore::kConsumerMaxDim */

/* " (W×H)" suffix appended to a resolution preset's label, mirroring the external
 * output dialog's dims_suffix (external-output-settings-dialog.cpp). Uses the
 * U+00D7 multiplication sign, like that dialog. Empty for an unknown (0) size. */
QString dims_suffix(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0)
		return QString();
	return QStringLiteral(" (%1×%2)").arg(w).arg(h);
}

/* True when the preset combo item at idx exists and is selectable. A rescale
 * preset is greyed out + unselectable when that OBS encoder has no active "Rescale
 * Output"; load_from uses this to fall back to Canvas base for a saved preset that
 * is no longer available (mirrors the external output dialog's bounds check). */
bool preset_selectable(QComboBox *combo, int idx)
{
	if (!combo || idx < 0)
		return false;
	if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
		if (QStandardItem *item = model->item(idx))
			return (item->flags() & Qt::ItemIsSelectable) != 0;
	}
	return true;
}

} // namespace

AmvInstanceForm::AmvInstanceForm(ConfigManager *config, const std::string &self_uuid, QWidget *parent)
	: ProviderSettingsForm(parent),
	  config_(config),
	  self_uuid_(self_uuid)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(8);

	/* ---- Target instance ---- */
	root->addWidget(new QLabel(amv::text("AMVPlugin.Provider.AmvInstance.TargetLabel"), this));
	instance_list_ = new QListWidget(this);
	instance_list_->setSelectionMode(QAbstractItemView::SingleSelection);
	instance_list_->setToolTip(amv::text("AMVPlugin.Provider.AmvInstance.TargetTooltip"));
	root->addWidget(instance_list_, 1);

	/* ---- Resolution ---- */
	auto *form = new QFormLayout();
	form->setContentsMargins(0, 0, 0, 0);

	cmb_res_mode_ = new QComboBox(this);
	cmb_res_mode_->addItem(amv::text("AMVPlugin.Provider.AmvInstance.Res.FollowWindow"),
			       (int)amv_nested::ResMode::FollowWindow);
	cmb_res_mode_->addItem(amv::text("AMVPlugin.Provider.AmvInstance.Res.FollowScreen"),
			       (int)amv_nested::ResMode::FollowScreen);
	cmb_res_mode_->addItem(amv::text("AMVPlugin.Provider.AmvInstance.Res.Manual"),
			       (int)amv_nested::ResMode::Manual);
	cmb_res_mode_->setToolTip(amv::text("AMVPlugin.Provider.AmvInstance.ResTooltip"));
	form->addRow(amv::text("AMVPlugin.Provider.AmvInstance.Resolution"), cmb_res_mode_);
	root->addLayout(form);

	/* Manual preset sub-group (visible only when Manual). */
	manual_group_ = new QGroupBox(amv::text("AMVPlugin.Provider.AmvInstance.ManualGroup"), this);
	manual_form_ = new QFormLayout(manual_group_);

	/* Preset list — mirrors the external output dialog (external-output-settings-
	 * dialog.cpp): each preset's label carries the concrete W×H it composes at, the
	 * two OBS rescale presets are greyed out with a hint when that encoder has no
	 * "Rescale Output" enabled, and Custom carries no size (it owns the editable row
	 * below). The i18n keys are shared with that dialog (AMVPlugin.Output.Res.*) so
	 * preset naming stays identical across both surfaces. Dimensions are read once
	 * here — OBS's video config is stable for the dialog's lifetime, exactly as the
	 * external output dialog does it. UserData stays the OutputResolutionMode string
	 * so load_from's findData is unaffected by the label text. */
	cmb_manual_preset_ = new QComboBox(manual_group_);

	struct obs_video_info ovi;
	const bool have_ovi = obs_get_video_info(&ovi);

	auto add_preset = [&](const char *labelKey, OutputResolutionMode mode, const QString &suffix) {
		cmb_manual_preset_->addItem(amv::text(labelKey) + suffix,
					    QString::fromUtf8(output_res_mode_to_str(mode)));
	};
	auto add_rescale = [&](const char *labelKey, const char *disabledTipKey, bool active, uint32_t rw, uint32_t rh,
			       OutputResolutionMode mode) {
		add_preset(labelKey, mode, active ? dims_suffix(rw, rh) : QString());
		if (active)
			return;
		const int idx = cmb_manual_preset_->count() - 1;
		if (auto *model = qobject_cast<QStandardItemModel *>(cmb_manual_preset_->model())) {
			if (QStandardItem *item = model->item(idx)) {
				item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
				item->setToolTip(amv::text(disabledTipKey));
			}
		}
	};

	add_preset("AMVPlugin.Output.Res.CanvasBase", OutputResolutionMode::CanvasBase,
		   have_ovi ? dims_suffix(ovi.base_width, ovi.base_height) : QString());
	add_preset("AMVPlugin.Output.Res.ObsOutput", OutputResolutionMode::ObsOutput,
		   have_ovi ? dims_suffix(ovi.output_width, ovi.output_height) : QString());

	uint32_t sw = 0, sh = 0;
	const bool stream_active = obs_stream_rescale_dimensions(sw, sh);
	add_rescale("AMVPlugin.Output.Res.StreamRescale", "AMVPlugin.Output.Res.StreamRescaleDisabled", stream_active,
		    sw, sh, OutputResolutionMode::ObsStreamRescale);

	uint32_t recw = 0, rech = 0;
	const bool rec_active = obs_record_rescale_dimensions(recw, rech);
	add_rescale("AMVPlugin.Output.Res.RecordRescale", "AMVPlugin.Output.Res.RecordRescaleDisabled", rec_active,
		    recw, rech, OutputResolutionMode::ObsRecordRescale);

	add_preset("AMVPlugin.Output.Res.Custom", OutputResolutionMode::Custom, QString());
	manual_form_->addRow(amv::text("AMVPlugin.Provider.AmvInstance.ManualPreset"), cmb_manual_preset_);

	/* Custom-size row: only the Custom preset needs editable W × H (every other
	 * preset shows its size in the combo text). The whole row is toggled via
	 * setRowVisible in apply_manual_preset_visibility(). A wide enough minimum keeps
	 * the last digit of a 4-digit dimension (e.g. 7680) from being clipped. */
	custom_row_ = new QWidget(manual_group_);
	auto *custom_layout = new QHBoxLayout(custom_row_);
	custom_layout->setContentsMargins(0, 0, 0, 0);
	spin_custom_w_ = new QSpinBox(custom_row_);
	spin_custom_w_->setRange(kCustomMin, kCustomMax);
	spin_custom_w_->setValue(1920);
	spin_custom_w_->setMinimumWidth(80);
	spin_custom_h_ = new QSpinBox(custom_row_);
	spin_custom_h_->setRange(kCustomMin, kCustomMax);
	spin_custom_h_->setValue(1080);
	spin_custom_h_->setMinimumWidth(80);
	custom_layout->addWidget(spin_custom_w_);
	custom_layout->addWidget(new QLabel(QStringLiteral("×"), custom_row_));
	custom_layout->addWidget(spin_custom_h_);
	custom_layout->addStretch(1);
	manual_form_->addRow(amv::text("AMVPlugin.Provider.AmvInstance.CustomSize"), custom_row_);
	root->addWidget(manual_group_);

	/* ---- Picture mode ---- */
	auto *form2 = new QFormLayout();
	form2->setContentsMargins(0, 0, 0, 0);
	cmb_picture_mode_ = new QComboBox(this);
	cmb_picture_mode_->addItem(amv::text("AMVPlugin.Provider.AmvInstance.Picture.Full"), QStringLiteral("full"));
	cmb_picture_mode_->addItem(amv::text("AMVPlugin.Provider.AmvInstance.Picture.Grid"), QStringLiteral("grid"));
	cmb_picture_mode_->setToolTip(amv::text("AMVPlugin.Provider.AmvInstance.PictureTooltip"));
	form2->addRow(amv::text("AMVPlugin.Provider.AmvInstance.Picture"), cmb_picture_mode_);
	root->addLayout(form2);

	/* ---- Audio placeholder (disabled; v1 does no cross-instance metering) ---- */
	chk_audio_ = new QCheckBox(amv::text("AMVPlugin.Provider.AmvInstance.Audio"), this);
	chk_audio_->setChecked(false);
	chk_audio_->setEnabled(false);
	chk_audio_->setToolTip(amv::text("AMVPlugin.Provider.AmvInstance.AudioTooltip"));
	root->addWidget(chk_audio_);

	connect(cmb_res_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { apply_res_mode_visibility(); });
	connect(cmb_manual_preset_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { apply_manual_preset_visibility(); });

	populate_instances();
	apply_res_mode_visibility();
	apply_manual_preset_visibility();
}

void AmvInstanceForm::populate_instances()
{
	instance_list_->clear();
	int count = 0;
	if (config_) {
		for (const auto &inst : config_->instances()) {
			const bool is_self = !self_uuid_.empty() && inst.uuid == self_uuid_;
			QString label = QString::fromStdString(inst.name);
			if (is_self)
				label += amv::text("AMVPlugin.Provider.AmvInstance.CurrentSuffix");
			auto *item = new QListWidgetItem(label);
			item->setData(kRoleUuid, QString::fromStdString(inst.uuid));
			item->setData(kRoleName, QString::fromStdString(inst.name));
			if (is_self) {
				/* Self-reference stays selectable (a safe feedback picture),
				 * but is marked bold + italic so the user knows it is the
				 * current instance. */
				QFont f = item->font();
				f.setBold(true);
				f.setItalic(true);
				item->setFont(f);
			}
			instance_list_->addItem(item);
			count++;
		}
	}
	if (count == 0) {
		auto *empty = new QListWidgetItem(amv::text("AMVPlugin.SourcePicker.AmvInstance.Empty"));
		empty->setFlags(Qt::NoItemFlags);
		instance_list_->addItem(empty);
	}
}

void AmvInstanceForm::ensure_selected_uuid(const QString &uuid, const QString &name)
{
	if (uuid.isEmpty())
		return;
	for (int i = 0; i < instance_list_->count(); i++) {
		auto *it = instance_list_->item(i);
		if (!(it->flags() & Qt::ItemIsSelectable))
			continue;
		if (it->data(kRoleUuid).toString() == uuid) {
			instance_list_->setCurrentItem(it);
			return;
		}
	}
	/* Target instance was deleted after the cell was bound: keep the binding
	 * visible + selected as a "missing" row so Edit Source round-trips it. */
	auto *missing = new QListWidgetItem(amv::text("AMVPlugin.Provider.AmvInstance.MissingTarget"));
	missing->setData(kRoleUuid, uuid);
	/* Keep the last known display name so re-saving a deleted-target binding does
	 * not blank the cell label (the uuid still round-trips; issue #20 P3 audit). */
	missing->setData(kRoleName, name);
	QFont f = missing->font();
	f.setItalic(true);
	missing->setFont(f);
	missing->setForeground(QBrush(QColor(160, 160, 160)));
	instance_list_->addItem(missing);
	instance_list_->setCurrentItem(missing);
}

void AmvInstanceForm::apply_res_mode_visibility()
{
	const bool manual = cmb_res_mode_->currentData().toInt() == (int)amv_nested::ResMode::Manual;
	manual_group_->setVisible(manual);
}

void AmvInstanceForm::apply_manual_preset_visibility()
{
	const bool custom = cmb_manual_preset_->currentData().toString() ==
			    QString::fromUtf8(output_res_mode_to_str(OutputResolutionMode::Custom));
	/* Every non-Custom preset shows its resolved W×H in the combo item text, so the
	 * editable size row is needed only for Custom; hide the whole row otherwise. */
	if (manual_form_ && custom_row_)
		manual_form_->setRowVisible(custom_row_, custom);
}

void AmvInstanceForm::load_from(const SignalConfig &cfg)
{
	obs_data_t *src = cfg.providerSettings;

	const QString uuid = src ? QString::fromUtf8(obs_data_get_string(src, amv_nested::kTargetUuidKey)) : QString();
	ensure_selected_uuid(uuid, QString::fromStdString(cfg.displayName));

	/* Resolution mode. */
	const char *rm = src ? obs_data_get_string(src, amv_nested::kResModeKey) : nullptr;
	const int res_idx = cmb_res_mode_->findData((int)amv_nested::res_mode_from_string(rm));
	if (res_idx >= 0)
		cmb_res_mode_->setCurrentIndex(res_idx);

	/* Manual preset + custom size. Fall back to Canvas base (index 0, always
	 * selectable) when the saved preset is an OBS rescale preset that is no longer
	 * enabled — its item is present but greyed + unselectable — mirroring the
	 * external output dialog's bounds check. */
	if (src && obs_data_has_user_value(src, amv_nested::kManualResModeKey)) {
		const QString preset = QString::fromUtf8(obs_data_get_string(src, amv_nested::kManualResModeKey));
		const int pidx = cmb_manual_preset_->findData(preset);
		cmb_manual_preset_->setCurrentIndex(preset_selectable(cmb_manual_preset_, pidx) ? pidx : 0);
	}
	if (src && obs_data_has_user_value(src, amv_nested::kManualCustomWKey))
		spin_custom_w_->setValue((int)obs_data_get_int(src, amv_nested::kManualCustomWKey));
	if (src && obs_data_has_user_value(src, amv_nested::kManualCustomHKey))
		spin_custom_h_->setValue((int)obs_data_get_int(src, amv_nested::kManualCustomHKey));

	/* Picture mode ("full" default / "grid"). */
	const char *pm = src ? obs_data_get_string(src, amv_nested::kPictureModeKey) : nullptr;
	const QString picture = (pm && *pm && std::string(pm) == "grid") ? QStringLiteral("grid")
									 : QStringLiteral("full");
	const int midx = cmb_picture_mode_->findData(picture);
	if (midx >= 0)
		cmb_picture_mode_->setCurrentIndex(midx);

	apply_res_mode_visibility();
	apply_manual_preset_visibility();
}

bool AmvInstanceForm::is_valid() const
{
	auto *cur = instance_list_->currentItem();
	return cur && (cur->flags() & Qt::ItemIsSelectable);
}

QString AmvInstanceForm::invalid_reason() const
{
	if (is_valid())
		return QString();
	return amv::text("AMVPlugin.SourcePicker.AmvInstance.SelectHint");
}

SignalConfig AmvInstanceForm::to_signal_config() const
{
	SignalConfig cfg;
	if (!is_valid())
		return cfg;

	auto *cur = instance_list_->currentItem();
	const QString uuid = cur->data(kRoleUuid).toString();
	const QString name = cur->data(kRoleName).toString();

	cfg.provider = SignalProviderType::AmvInstance;
	cfg.displayName = name.toStdString();
	cfg.providerSettings = obs_data_create();
	obs_data_t *d = cfg.providerSettings;

	obs_data_set_string(d, amv_nested::kTargetUuidKey, uuid.toUtf8().constData());
	obs_data_set_string(d, amv_nested::kPictureModeKey,
			    cmb_picture_mode_->currentData().toString().toUtf8().constData());

	const auto res_mode = (amv_nested::ResMode)cmb_res_mode_->currentData().toInt();
	obs_data_set_string(d, amv_nested::kResModeKey, amv_nested::res_mode_to_string(res_mode));

	/* Persist the manual preset + custom size unconditionally so toggling back to
	 * Manual restores the user's last choice (round-trip stable). */
	obs_data_set_string(d, amv_nested::kManualResModeKey,
			    cmb_manual_preset_->currentData().toString().toUtf8().constData());
	obs_data_set_int(d, amv_nested::kManualCustomWKey, spin_custom_w_->value());
	obs_data_set_int(d, amv_nested::kManualCustomHKey, spin_custom_h_->value());

	return cfg;
}
