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

#include "signal-lost-settings-dialog.hpp"
#include "amv-i18n.hpp"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

/* ---- enum <-> combo helpers ---- */

namespace {

constexpr int kInternalBlackIdx = 0;
constexpr int kInternalPlaceholderIdx = 1;
constexpr int kInternalClearIdx = 2;

InternalMissingBehavior internal_from_idx(int idx)
{
	switch (idx) {
	case kInternalPlaceholderIdx:
		return InternalMissingBehavior::PlaceholderImage;
	case kInternalClearIdx:
		return InternalMissingBehavior::ClearCell;
	default:
		return InternalMissingBehavior::Black;
	}
}

int idx_from_internal(InternalMissingBehavior b)
{
	switch (b) {
	case InternalMissingBehavior::PlaceholderImage:
		return kInternalPlaceholderIdx;
	case InternalMissingBehavior::ClearCell:
		return kInternalClearIdx;
	default:
		return kInternalBlackIdx;
	}
}

constexpr int kExternalOverlayIdx = 0;
constexpr int kExternalRetryOnlyIdx = 1;
constexpr int kExternalRetryFallbackIdx = 2;
constexpr int kExternalImageIdx = 3;

ExternalLostBehavior external_from_idx(int idx)
{
	switch (idx) {
	case kExternalRetryOnlyIdx:
		return ExternalLostBehavior::RetryOnly;
	case kExternalRetryFallbackIdx:
		return ExternalLostBehavior::RetryWithFallback;
	case kExternalImageIdx:
		return ExternalLostBehavior::SignalLostImage;
	default:
		return ExternalLostBehavior::SignalLostOverlay;
	}
}

int idx_from_external(ExternalLostBehavior b)
{
	switch (b) {
	case ExternalLostBehavior::RetryOnly:
		return kExternalRetryOnlyIdx;
	case ExternalLostBehavior::RetryWithFallback:
		return kExternalRetryFallbackIdx;
	case ExternalLostBehavior::SignalLostImage:
		return kExternalImageIdx;
	default:
		return kExternalOverlayIdx;
	}
}

/* fallbackType uses the same string namespace as CellAssignment.type so that
 * future M6 work can reuse provider plumbing. The combo just maps display
 * label <-> token.
 *
 * Phase 3 / M5.4 hardening: the OBS-source-based fallback options
 * (pgm / prvw / scene / source) are temporarily disabled in the UI. All
 * four resolve through obs_source_video_render -> scene_video_render ->
 * sceneitem signal callbacks, which has been observed to crash OBS when
 * a third-party plugin (e.g. streamdeck-plugin-obs) holds stale
 * sceneitem state across a source remove + restore. The fallback render
 * code still honors these tokens for previously-persisted configs, but
 * users can no longer pick them in this dialog until we ship a safe
 * render path. "None" and "Static image" stay enabled. */
struct FallbackOption {
	const char *token;
	const char *labelKey;
	bool enabled;
};

constexpr FallbackOption kFallbackOptions[] = {
	{"", "AMVPlugin.SignalLost.Fallback.None", true},
	{"image", "AMVPlugin.SignalLost.Fallback.StaticImage", true},
	{"pgm", "AMVPlugin.SignalLost.Fallback.ProgramComingSoon", false},
	{"prvw", "AMVPlugin.SignalLost.Fallback.PreviewComingSoon", false},
	{"scene", "AMVPlugin.SignalLost.Fallback.SceneComingSoon", false},
	{"source", "AMVPlugin.SignalLost.Fallback.SourceComingSoon", false},
};

constexpr int kFallbackOptionCount = sizeof(kFallbackOptions) / sizeof(kFallbackOptions[0]);

int idx_from_fallback_token(const std::string &token)
{
	for (int i = 0; i < kFallbackOptionCount; i++) {
		if (token == kFallbackOptions[i].token)
			return i;
	}
	return 0; /* unknown -> None */
}

const char *fallback_token_from_idx(int idx)
{
	if (idx >= 0 && idx < kFallbackOptionCount)
		return kFallbackOptions[idx].token;
	return "";
}

} // namespace

/* ---- ctor / UI ---- */

SignalLostSettingsDialog::SignalLostSettingsDialog(Mode mode, QWidget *parent) : QDialog(parent), mode_(mode)
{
	setWindowTitle(mode_ == Mode::Global ? amv::text("AMVPlugin.SignalLost.Title.Global")
					     : amv::text("AMVPlugin.SignalLost.Title.Cell"));
	setMinimumWidth(440);
	build_ui();
	apply_settings(LostSignalSettings{}); /* initialize controls with defaults */
	update_enabled_state();
}

void SignalLostSettingsDialog::build_ui()
{
	auto *root = new QVBoxLayout(this);

	/* Inheritance row \u2014 visible only in Cell mode. We still construct it in
	 * Global mode and hide so widget pointers stay valid. */
	auto *inherit_row = new QHBoxLayout();
	inherit_row->addWidget(new QLabel(amv::text("AMVPlugin.Visual.Inheritance")));
	cmb_inherit_ = new QComboBox(this);
	cmb_inherit_->addItem(amv::text("AMVPlugin.SignalLost.Inherit.UseGlobal"));
	cmb_inherit_->addItem(amv::text("AMVPlugin.SignalLost.Inherit.OverrideCell"));
	inherit_row->addWidget(cmb_inherit_, 1);
	root->addLayout(inherit_row);
	if (mode_ == Mode::Global) {
		for (int i = 0; i < inherit_row->count(); i++) {
			QWidget *w = inherit_row->itemAt(i)->widget();
			if (w)
				w->hide();
		}
	}
	connect(cmb_inherit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&SignalLostSettingsDialog::on_inheritance_changed);

	/* Internal source missing group. */
	auto *grp_internal = new QGroupBox(amv::text("AMVPlugin.SignalLost.Internal.Title"), this);
	auto *form_internal = new QFormLayout(grp_internal);
	cmb_internal_behavior_ = new QComboBox(grp_internal);
	cmb_internal_behavior_->addItem(amv::text("AMVPlugin.SignalLost.Internal.BlackOverlay"));
	cmb_internal_behavior_->addItem(amv::text("AMVPlugin.SignalLost.Internal.PlaceholderImage"));
	cmb_internal_behavior_->addItem(amv::text("AMVPlugin.SignalLost.Internal.ClearCell"));
	form_internal->addRow(amv::text("AMVPlugin.SignalLost.Internal.OnMissing"), cmb_internal_behavior_);

	auto *placeholder_row = new QHBoxLayout();
	edit_placeholder_path_ = new QLineEdit(grp_internal);
	edit_placeholder_path_->setPlaceholderText(amv::text("AMVPlugin.SignalLost.ImagePathPlaceholder"));
	placeholder_row->addWidget(edit_placeholder_path_, 1);
	auto *btn_placeholder = new QToolButton(grp_internal);
	btn_placeholder->setText(QStringLiteral("..."));
	connect(btn_placeholder, &QToolButton::clicked, this, &SignalLostSettingsDialog::on_browse_placeholder_image);
	placeholder_row->addWidget(btn_placeholder);
	form_internal->addRow(amv::text("AMVPlugin.SignalLost.Internal.PlaceholderImageLabel"), placeholder_row);

	/* Phase 3 / M5.4: image fit mode for placeholder. Stretch fills the
	 * cell exactly (default — most placeholder/banner art is authored at
	 * a known target ratio); Fit preserves aspect with letterbox bars. */
	cmb_placeholder_fit_ = new QComboBox(grp_internal);
	cmb_placeholder_fit_->addItem(amv::text("AMVPlugin.SignalLost.ImageFit.Stretch"));
	cmb_placeholder_fit_->addItem(amv::text("AMVPlugin.SignalLost.ImageFit.Fit"));
	form_internal->addRow(amv::text("AMVPlugin.SignalLost.Internal.PlaceholderFit"), cmb_placeholder_fit_);
	root->addWidget(grp_internal);

	connect(cmb_internal_behavior_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { update_enabled_state(); });

	/* External source lost group. Provider-backed cells (FFmpeg / NDI / Spout
	 * / VLC) use this branch when the private source is alive but unhealthy. */
	auto *grp_external = new QGroupBox(amv::text("AMVPlugin.SignalLost.External.Title"), this);
	auto *form_external = new QFormLayout(grp_external);
	cmb_external_behavior_ = new QComboBox(grp_external);
	cmb_external_behavior_->addItem(amv::text("AMVPlugin.SignalLost.External.SignalLostOverlay"));
	cmb_external_behavior_->addItem(amv::text("AMVPlugin.SignalLost.External.RetryOnly"));
	cmb_external_behavior_->addItem(amv::text("AMVPlugin.SignalLost.External.RetryFallback"));
	cmb_external_behavior_->addItem(amv::text("AMVPlugin.SignalLost.External.SignalLostImage"));
	form_external->addRow(amv::text("AMVPlugin.SignalLost.External.OnLost"), cmb_external_behavior_);

	auto *signal_lost_row = new QHBoxLayout();
	edit_signal_lost_path_ = new QLineEdit(grp_external);
	edit_signal_lost_path_->setPlaceholderText(amv::text("AMVPlugin.SignalLost.ImagePathPlaceholder"));
	signal_lost_row->addWidget(edit_signal_lost_path_, 1);
	auto *btn_signal_lost = new QToolButton(grp_external);
	btn_signal_lost->setText(QStringLiteral("..."));
	connect(btn_signal_lost, &QToolButton::clicked, this, &SignalLostSettingsDialog::on_browse_signal_lost_image);
	signal_lost_row->addWidget(btn_signal_lost);
	form_external->addRow(amv::text("AMVPlugin.SignalLost.External.SignalLostImageLabel"), signal_lost_row);

	/* Phase 3 / M6 (preview): fit mode for the Signal Lost image. */
	cmb_signal_lost_fit_ = new QComboBox(grp_external);
	cmb_signal_lost_fit_->addItem(amv::text("AMVPlugin.SignalLost.ImageFit.Stretch"));
	cmb_signal_lost_fit_->addItem(amv::text("AMVPlugin.SignalLost.ImageFit.Fit"));
	form_external->addRow(amv::text("AMVPlugin.SignalLost.External.SignalLostFit"), cmb_signal_lost_fit_);
	root->addWidget(grp_external);

	connect(cmb_external_behavior_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { update_enabled_state(); });

	/* Fallback group. */
	auto *grp_fallback = new QGroupBox(amv::text("AMVPlugin.SignalLost.Fallback.Title"), this);
	auto *form_fallback = new QFormLayout(grp_fallback);
	cmb_fallback_type_ = new QComboBox(grp_fallback);
	for (int i = 0; i < kFallbackOptionCount; i++)
		cmb_fallback_type_->addItem(amv::text(kFallbackOptions[i].labelKey));
	/* Phase 3 / M5.4 hardening: grey out the OBS-source-based fallback
	 * options (pgm/prvw/scene/source). They still render correctly when
	 * present in a previously-persisted config, but the user can no longer
	 * pick them here — they trigger a third-party-plugin crash on source
	 * restore. Reach the underlying QStandardItemModel to clear
	 * ItemIsEnabled / ItemIsSelectable on each disabled row. */
	if (auto *model = qobject_cast<QStandardItemModel *>(cmb_fallback_type_->model())) {
		for (int i = 0; i < kFallbackOptionCount; i++) {
			if (kFallbackOptions[i].enabled)
				continue;
			if (auto *item = model->item(i)) {
				Qt::ItemFlags flags = item->flags();
				flags &= ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
				item->setFlags(flags);
			}
		}
	}
	form_fallback->addRow(amv::text("AMVPlugin.SignalLost.Fallback.Type"), cmb_fallback_type_);

	auto *fallback_row = new QHBoxLayout();
	edit_fallback_name_ = new QLineEdit(grp_fallback);
	edit_fallback_name_->setPlaceholderText(amv::text("AMVPlugin.SignalLost.Fallback.NamePlaceholder"));
	fallback_row->addWidget(edit_fallback_name_, 1);
	auto *btn_fallback = new QToolButton(grp_fallback);
	btn_fallback->setText(QStringLiteral("..."));
	connect(btn_fallback, &QToolButton::clicked, this, &SignalLostSettingsDialog::on_browse_fallback_image);
	fallback_row->addWidget(btn_fallback);
	form_fallback->addRow(amv::text("AMVPlugin.SignalLost.Fallback.NamePath"), fallback_row);

	/* Phase 3 / M5.4: fit mode applies only when fallbackType == "image".
	 * For OBS source / scene / pgm / prvw fallbacks the renderer always
	 * uses native obs_source_video_render letterbox to preserve canvas
	 * pixels, so the fit choice would be ignored — we still keep the row
	 * always-visible for clarity (greys out automatically when not
	 * applicable via update_enabled_state). */
	cmb_fallback_image_fit_ = new QComboBox(grp_fallback);
	cmb_fallback_image_fit_->addItem(amv::text("AMVPlugin.SignalLost.ImageFit.Stretch"));
	cmb_fallback_image_fit_->addItem(amv::text("AMVPlugin.SignalLost.ImageFit.Fit"));
	form_fallback->addRow(amv::text("AMVPlugin.SignalLost.Fallback.ImageFit"), cmb_fallback_image_fit_);
	root->addWidget(grp_fallback);

	connect(cmb_fallback_type_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&SignalLostSettingsDialog::on_fallback_type_changed);

	/* Backoff timing group. */
	auto *grp_backoff = new QGroupBox(amv::text("AMVPlugin.SignalLost.Reconnect.Title"), this);
	auto *form_backoff = new QFormLayout(grp_backoff);
	spin_retry_initial_ = new QSpinBox(grp_backoff);
	spin_retry_initial_->setRange(100, 60000);
	spin_retry_initial_->setSuffix(QStringLiteral(" ms"));
	form_backoff->addRow(amv::text("AMVPlugin.SignalLost.Reconnect.Initial"), spin_retry_initial_);

	spin_retry_max_ = new QSpinBox(grp_backoff);
	spin_retry_max_->setRange(100, 600000);
	spin_retry_max_->setSuffix(QStringLiteral(" ms"));
	form_backoff->addRow(amv::text("AMVPlugin.SignalLost.Reconnect.Max"), spin_retry_max_);

	spin_manual_cooldown_ = new QSpinBox(grp_backoff);
	spin_manual_cooldown_->setRange(0, 60000);
	spin_manual_cooldown_->setSuffix(QStringLiteral(" ms"));
	/* Phase 3 hardening tail: the legacy label "Manual reconnect cooldown"
	 * suggested this only throttled Reconnect Now clicks, but the
	 * supervisor uses the same value to pace its automatic retry ladder
	 * (cheap media_restart attempts and full source recreate alike).
	 * Tooltip makes the dual scope explicit; persisted JSON key stays
	 * `manualReconnectCooldownMs` for backwards compatibility. */
	spin_manual_cooldown_->setToolTip(amv::text("AMVPlugin.SignalLost.Reconnect.CooldownTooltip"));
	form_backoff->addRow(amv::text("AMVPlugin.SignalLost.Reconnect.Cooldown"), spin_manual_cooldown_);
	root->addWidget(grp_backoff);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

/* ---- public setters / getters ---- */

void SignalLostSettingsDialog::set_cell_position(int row, int col)
{
	cell_row_ = row;
	cell_col_ = col;
	if (mode_ == Mode::Cell && row >= 0 && col >= 0)
		setWindowTitle(amv::text("AMVPlugin.SignalLost.Title.CellPosition").arg(row).arg(col));
}

void SignalLostSettingsDialog::set_global_settings(const LostSignalSettings &s)
{
	apply_settings(s);
	update_enabled_state();
}

LostSignalSettings SignalLostSettingsDialog::get_global_settings() const
{
	return collect_settings();
}

void SignalLostSettingsDialog::set_cell_settings(const CellLostSignalSettings &c)
{
	cell_row_ = c.row;
	cell_col_ = c.col;
	cmb_inherit_->setCurrentIndex(c.mode == InheritanceMode::Override ? 1 : 0);
	apply_settings(c.settings);
	update_enabled_state();
}

CellLostSignalSettings SignalLostSettingsDialog::get_cell_settings() const
{
	CellLostSignalSettings c;
	c.row = cell_row_;
	c.col = cell_col_;
	c.mode = cmb_inherit_->currentIndex() == 1 ? InheritanceMode::Override : InheritanceMode::Inherit;
	c.settings = collect_settings();
	return c;
}

/* ---- private helpers ---- */

void SignalLostSettingsDialog::apply_settings(const LostSignalSettings &s)
{
	cmb_internal_behavior_->setCurrentIndex(idx_from_internal(s.internalMissingBehavior));
	edit_placeholder_path_->setText(QString::fromStdString(s.placeholderImagePath));
	cmb_placeholder_fit_->setCurrentIndex(s.placeholderImageFitMode == ImageFitMode::Fit ? 1 : 0);

	cmb_external_behavior_->setCurrentIndex(idx_from_external(s.externalLostBehavior));
	edit_signal_lost_path_->setText(QString::fromStdString(s.signalLostImagePath));
	cmb_signal_lost_fit_->setCurrentIndex(s.signalLostImageFitMode == ImageFitMode::Fit ? 1 : 0);

	cmb_fallback_type_->setCurrentIndex(idx_from_fallback_token(s.fallbackType));
	edit_fallback_name_->setText(QString::fromStdString(s.fallbackName));
	cmb_fallback_image_fit_->setCurrentIndex(s.fallbackImageFitMode == ImageFitMode::Fit ? 1 : 0);

	spin_retry_initial_->setValue(s.retryInitialMs);
	spin_retry_max_->setValue(s.retryMaxMs);
	spin_manual_cooldown_->setValue(s.manualReconnectCooldownMs);
}

LostSignalSettings SignalLostSettingsDialog::collect_settings() const
{
	LostSignalSettings s;
	s.internalMissingBehavior = internal_from_idx(cmb_internal_behavior_->currentIndex());
	s.placeholderImagePath = edit_placeholder_path_->text().toStdString();
	s.placeholderImageFitMode = cmb_placeholder_fit_->currentIndex() == 1 ? ImageFitMode::Fit
									      : ImageFitMode::Stretch;

	s.externalLostBehavior = external_from_idx(cmb_external_behavior_->currentIndex());
	s.signalLostImagePath = edit_signal_lost_path_->text().toStdString();
	s.signalLostImageFitMode = cmb_signal_lost_fit_->currentIndex() == 1 ? ImageFitMode::Fit
									     : ImageFitMode::Stretch;

	s.fallbackType = fallback_token_from_idx(cmb_fallback_type_->currentIndex());
	s.fallbackName = edit_fallback_name_->text().toStdString();
	s.fallbackImageFitMode = cmb_fallback_image_fit_->currentIndex() == 1 ? ImageFitMode::Fit
									      : ImageFitMode::Stretch;

	s.retryInitialMs = spin_retry_initial_->value();
	s.retryMaxMs = spin_retry_max_->value();
	if (s.retryMaxMs < s.retryInitialMs)
		s.retryMaxMs = s.retryInitialMs;
	s.manualReconnectCooldownMs = spin_manual_cooldown_->value();
	return s;
}

void SignalLostSettingsDialog::update_enabled_state()
{
	const bool inherit_locks_form = mode_ == Mode::Cell && cmb_inherit_->currentIndex() == 0;

	const auto setRowEnabled = [&](QWidget *w, bool enabled) {
		if (w)
			w->setEnabled(enabled && !inherit_locks_form);
	};

	setRowEnabled(cmb_internal_behavior_, true);
	setRowEnabled(edit_placeholder_path_, internal_from_idx(cmb_internal_behavior_->currentIndex()) ==
						      InternalMissingBehavior::PlaceholderImage);
	setRowEnabled(cmb_placeholder_fit_, internal_from_idx(cmb_internal_behavior_->currentIndex()) ==
						    InternalMissingBehavior::PlaceholderImage);

	setRowEnabled(cmb_external_behavior_, true);
	setRowEnabled(edit_signal_lost_path_, external_from_idx(cmb_external_behavior_->currentIndex()) ==
						      ExternalLostBehavior::SignalLostImage);
	setRowEnabled(cmb_signal_lost_fit_, external_from_idx(cmb_external_behavior_->currentIndex()) ==
						    ExternalLostBehavior::SignalLostImage);

	setRowEnabled(cmb_fallback_type_, true);
	const std::string fb_token = fallback_token_from_idx(cmb_fallback_type_->currentIndex());
	setRowEnabled(edit_fallback_name_, !fb_token.empty());
	/* Image fit only meaningful for fallbackType == "image"; greyed out
	 * for OBS-source variants (renderer uses native letterbox there). */
	setRowEnabled(cmb_fallback_image_fit_, fb_token == "image");

	setRowEnabled(spin_retry_initial_, true);
	setRowEnabled(spin_retry_max_, true);
	setRowEnabled(spin_manual_cooldown_, true);
}

/* ---- slots ---- */

void SignalLostSettingsDialog::on_inheritance_changed(int)
{
	update_enabled_state();
}

void SignalLostSettingsDialog::on_browse_placeholder_image()
{
	QString file = QFileDialog::getOpenFileName(this, amv::text("AMVPlugin.SignalLost.FileDialog.Placeholder"),
						    QString(),
						    amv::text("AMVPlugin.SignalLost.FileDialog.ImageFilter"));
	if (!file.isEmpty())
		edit_placeholder_path_->setText(file);
}

void SignalLostSettingsDialog::on_browse_signal_lost_image()
{
	QString file = QFileDialog::getOpenFileName(this, amv::text("AMVPlugin.SignalLost.FileDialog.SignalLost"),
						    QString(),
						    amv::text("AMVPlugin.SignalLost.FileDialog.ImageFilter"));
	if (!file.isEmpty())
		edit_signal_lost_path_->setText(file);
}

void SignalLostSettingsDialog::on_fallback_type_changed(int)
{
	update_enabled_state();
}

void SignalLostSettingsDialog::on_browse_fallback_image()
{
	/* Only meaningful when fallbackType == "image". The browse button
	 * itself is enabled in the row layout regardless of type to keep the
	 * widget tree static; if the user browses while another type is
	 * selected we still apply the path \u2014 it'll just be ignored until
	 * they switch fallback type back to Image. */
	QString file = QFileDialog::getOpenFileName(this, amv::text("AMVPlugin.SignalLost.FileDialog.Fallback"),
						    QString(),
						    amv::text("AMVPlugin.SignalLost.FileDialog.ImageFilter"));
	if (!file.isEmpty())
		edit_fallback_name_->setText(file);
}
