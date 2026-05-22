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

#include "cell-display-settings-dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

/* ---- Inheritance combo helper ---- */

static QComboBox *create_inherit_combo(QWidget *parent)
{
	auto *cmb = new QComboBox(parent);
	cmb->addItem(QStringLiteral("Inherit"), (int)InheritanceMode::Inherit);
	cmb->addItem(QStringLiteral("Override"), (int)InheritanceMode::Override);
	return cmb;
}

static void set_inherit_combo(QComboBox *cmb, InheritanceMode mode)
{
	cmb->setCurrentIndex(mode == InheritanceMode::Override ? 1 : 0);
}

static InheritanceMode get_inherit_combo(QComboBox *cmb)
{
	return cmb->currentIndex() == 1 ? InheritanceMode::Override : InheritanceMode::Inherit;
}

/* ---- Color helper: ARGB uint32 <-> "#RRGGBB" hex string ---- */

static QString color_to_hex(uint32_t argb)
{
	return QStringLiteral("#%1%2%3")
		.arg((argb >> 16) & 0xFF, 2, 16, QChar('0'))
		.arg((argb >> 8) & 0xFF, 2, 16, QChar('0'))
		.arg(argb & 0xFF, 2, 16, QChar('0'));
}

static uint32_t hex_to_color(const QString &hex, uint32_t fallback = 0xFFFFFFFF)
{
	QString h = hex.trimmed();
	if (h.startsWith('#'))
		h = h.mid(1);
	if (h.length() != 6)
		return fallback;
	bool ok;
	uint32_t rgb = h.toUInt(&ok, 16);
	if (!ok)
		return fallback;
	return 0xFF000000 | rgb;
}

/* ---- Constructor ---- */

CellDisplaySettingsDialog::CellDisplaySettingsDialog(Mode mode, QWidget *parent) : QDialog(parent), mode_(mode)
{
	setup_ui();
	update_inheritance_visibility();
}

void CellDisplaySettingsDialog::set_cell_position(int row, int col)
{
	cell_row_ = row;
	cell_col_ = col;
	if (mode_ == Mode::Cell) {
		setWindowTitle(QStringLiteral("Cell Display Settings [%1, %2]").arg(row).arg(col));
	}
}

/* ---- UI Setup ---- */

void CellDisplaySettingsDialog::setup_ui()
{
	switch (mode_) {
	case Mode::Global:
		setWindowTitle(QStringLiteral("Global Visual Settings"));
		break;
	case Mode::Instance:
		setWindowTitle(QStringLiteral("Instance Visual Settings"));
		break;
	case Mode::Cell:
		setWindowTitle(QStringLiteral("Cell Display Settings"));
		break;
	}
	setMinimumSize(420, 500);
	resize(480, 640);

	auto *mainLayout = new QVBoxLayout(this);

	/* Scroll area for all settings groups */
	auto *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);
	auto *scrollWidget = new QWidget();
	auto *scrollLayout = new QVBoxLayout(scrollWidget);
	scrollLayout->setContentsMargins(4, 4, 4, 4);

	scrollLayout->addWidget(create_background_group());
	scrollLayout->addWidget(create_label_group());
	scrollLayout->addWidget(create_safe_area_group());
	scrollLayout->addWidget(create_vu_meter_group());
	scrollLayout->addWidget(create_overlay_group());
	scrollLayout->addStretch();

	scrollArea->setWidget(scrollWidget);
	mainLayout->addWidget(scrollArea, 1);

	/* Copy/Paste/Reset + Button box */
	auto *bottomLayout = new QHBoxLayout;
	auto *btnCopy = new QPushButton(tr("Copy"), this);
	auto *btnPaste = new QPushButton(tr("Paste"), this);
	auto *btnReset = new QPushButton(tr("Reset"), this);
	bottomLayout->addWidget(btnCopy);
	bottomLayout->addWidget(btnPaste);
	bottomLayout->addWidget(btnReset);
	bottomLayout->addStretch();
	auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	bottomLayout->addWidget(btnBox);
	mainLayout->addLayout(bottomLayout);

	connect(btnCopy, &QPushButton::clicked, this, &CellDisplaySettingsDialog::on_copy);
	connect(btnPaste, &QPushButton::clicked, this, &CellDisplaySettingsDialog::on_paste);
	connect(btnReset, &QPushButton::clicked, this, &CellDisplaySettingsDialog::on_reset);
	connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

/* ---- Helper: connect dirty tracking to a widget group ---- */

#define HOOK_COMBO(cmb)                                                   \
	connect(cmb, QOverload<int>::of(&QComboBox::currentIndexChanged), \
		this, [this](int) { dirty_ = true; emit settings_changed(); })
#define HOOK_CHECK(chk) \
	connect(chk, &QCheckBox::toggled, this, [this](bool) { dirty_ = true; emit settings_changed(); })
#define HOOK_SPIN(spin)                                                     \
	connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, \
		[this](int) { dirty_ = true; emit settings_changed(); })
#define HOOK_DSPIN(spin)                                                         \
	connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, \
		[this](double) { dirty_ = true; emit settings_changed(); })
#define HOOK_EDIT(edit) \
	connect(edit, &QLineEdit::textChanged, this, [this](const QString &) { dirty_ = true; emit settings_changed(); })

/* ---- Background Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_background_group()
{
	grp_background_ = new QGroupBox(QStringLiteral("Background"), this);
	auto *layout = new QVBoxLayout(grp_background_);

	/* Inheritance combo (Instance/Cell only) */
	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(QStringLiteral("Inheritance:"), grp_background_));
		cmb_bg_inherit_ = create_inherit_combo(grp_background_);
		inh_row->addWidget(cmb_bg_inherit_);
		inh_row->addStretch();
		layout->addLayout(inh_row);
		connect(cmb_bg_inherit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
			update_inheritance_visibility();
			dirty_ = true;
			emit settings_changed();
		});
	}

	auto *form = new QFormLayout();

	chk_bg_color_enabled_ = new QCheckBox(grp_background_);
	form->addRow(QStringLiteral("Color Enabled:"), chk_bg_color_enabled_);

	edit_bg_color_ = new QLineEdit(QStringLiteral("#000000"), grp_background_);
	edit_bg_color_->setMaximumWidth(100);
	form->addRow(QStringLiteral("Color (#RRGGBB):"), edit_bg_color_);

	cmb_bg_fill_mode_ = new QComboBox(grp_background_);
	cmb_bg_fill_mode_->addItem(QStringLiteral("Fill Signal Only"), (int)BackgroundFillMode::FillSignalOnly);
	cmb_bg_fill_mode_->addItem(QStringLiteral("Fill Entire Cell"), (int)BackgroundFillMode::FillEntireCell);
	form->addRow(QStringLiteral("Fill Mode:"), cmb_bg_fill_mode_);

	chk_bg_image_enabled_ = new QCheckBox(grp_background_);
	form->addRow(QStringLiteral("Image Enabled:"), chk_bg_image_enabled_);

	edit_bg_image_path_ = new QLineEdit(grp_background_);
	form->addRow(QStringLiteral("Image Path:"), edit_bg_image_path_);

	cmb_bg_image_fit_ = new QComboBox(grp_background_);
	cmb_bg_image_fit_->addItem(QStringLiteral("Fit"), (int)ImageFitMode::Fit);
	cmb_bg_image_fit_->addItem(QStringLiteral("Stretch"), (int)ImageFitMode::Stretch);
	form->addRow(QStringLiteral("Image Fit:"), cmb_bg_image_fit_);

	layout->addLayout(form);

	HOOK_CHECK(chk_bg_color_enabled_);
	HOOK_EDIT(edit_bg_color_);
	HOOK_COMBO(cmb_bg_fill_mode_);
	HOOK_CHECK(chk_bg_image_enabled_);
	HOOK_EDIT(edit_bg_image_path_);
	HOOK_COMBO(cmb_bg_image_fit_);

	return grp_background_;
}

/* ---- Label Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_label_group()
{
	grp_label_ = new QGroupBox(QStringLiteral("Label"), this);
	auto *layout = new QVBoxLayout(grp_label_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(QStringLiteral("Inheritance:"), grp_label_));
		cmb_label_inherit_ = create_inherit_combo(grp_label_);
		inh_row->addWidget(cmb_label_inherit_);
		inh_row->addStretch();
		layout->addLayout(inh_row);
		connect(cmb_label_inherit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
			update_inheritance_visibility();
			dirty_ = true;
			emit settings_changed();
		});
	}

	auto *form = new QFormLayout();

	cmb_label_display_ = new QComboBox(grp_label_);
	cmb_label_display_->addItem(QStringLiteral("None"), (int)LabelDisplayMode::None);
	cmb_label_display_->addItem(QStringLiteral("Overlay"), (int)LabelDisplayMode::Overlay);
	cmb_label_display_->addItem(QStringLiteral("Below"), (int)LabelDisplayMode::Below);
	form->addRow(QStringLiteral("Display:"), cmb_label_display_);

	cmb_label_position_ = new QComboBox(grp_label_);
	cmb_label_position_->addItem(QStringLiteral("Top"), (int)LabelPosition::Top);
	cmb_label_position_->addItem(QStringLiteral("Bottom"), (int)LabelPosition::Bottom);
	form->addRow(QStringLiteral("Position:"), cmb_label_position_);

	spin_label_font_size_ = new QSpinBox(grp_label_);
	spin_label_font_size_->setRange(6, 96);
	form->addRow(QStringLiteral("Font Size:"), spin_label_font_size_);

	cmb_label_scale_mode_ = new QComboBox(grp_label_);
	cmb_label_scale_mode_->addItem(QStringLiteral("Fixed"), (int)FontScaleMode::Fixed);
	cmb_label_scale_mode_->addItem(QStringLiteral("Scale With Cell"), (int)FontScaleMode::ScaleWithCell);
	form->addRow(QStringLiteral("Scale Mode:"), cmb_label_scale_mode_);

	spin_label_min_font_ = new QSpinBox(grp_label_);
	spin_label_min_font_->setRange(4, 48);
	form->addRow(QStringLiteral("Min Font Size:"), spin_label_min_font_);

	spin_label_max_font_ = new QSpinBox(grp_label_);
	spin_label_max_font_->setRange(12, 144);
	form->addRow(QStringLiteral("Max Font Size:"), spin_label_max_font_);

	edit_label_text_color_ = new QLineEdit(QStringLiteral("#FFFFFF"), grp_label_);
	edit_label_text_color_->setMaximumWidth(100);
	form->addRow(QStringLiteral("Text Color:"), edit_label_text_color_);

	spin_label_bg_opacity_ = new QDoubleSpinBox(grp_label_);
	spin_label_bg_opacity_->setRange(0.0, 1.0);
	spin_label_bg_opacity_->setSingleStep(0.05);
	spin_label_bg_opacity_->setDecimals(2);
	form->addRow(QStringLiteral("BG Opacity:"), spin_label_bg_opacity_);

	spin_label_margin_ = new QSpinBox(grp_label_);
	spin_label_margin_->setRange(0, 32);
	form->addRow(QStringLiteral("Margin:"), spin_label_margin_);

	chk_bg_label_fill_ = new QCheckBox(grp_label_);
	form->addRow(QStringLiteral("Label Region Fill:"), chk_bg_label_fill_);

	layout->addLayout(form);

	HOOK_COMBO(cmb_label_display_);
	HOOK_COMBO(cmb_label_position_);
	HOOK_SPIN(spin_label_font_size_);
	HOOK_COMBO(cmb_label_scale_mode_);
	HOOK_SPIN(spin_label_min_font_);
	HOOK_SPIN(spin_label_max_font_);
	HOOK_EDIT(edit_label_text_color_);
	HOOK_DSPIN(spin_label_bg_opacity_);
	HOOK_SPIN(spin_label_margin_);
	HOOK_CHECK(chk_bg_label_fill_);

	return grp_label_;
}

/* ---- Safe Area Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_safe_area_group()
{
	grp_safe_area_ = new QGroupBox(QStringLiteral("Safe Area"), this);
	auto *layout = new QVBoxLayout(grp_safe_area_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(QStringLiteral("Inheritance:"), grp_safe_area_));
		cmb_safe_area_inherit_ = create_inherit_combo(grp_safe_area_);
		inh_row->addWidget(cmb_safe_area_inherit_);
		inh_row->addStretch();
		layout->addLayout(inh_row);
		connect(cmb_safe_area_inherit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
			update_inheritance_visibility();
			dirty_ = true;
			emit settings_changed();
		});
	}

	auto *form = new QFormLayout();

	chk_safe_area_enabled_ = new QCheckBox(grp_safe_area_);
	form->addRow(QStringLiteral("Enabled:"), chk_safe_area_enabled_);

	edit_safe_area_color_ = new QLineEdit(QStringLiteral("#D0D0D0"), grp_safe_area_);
	edit_safe_area_color_->setMaximumWidth(100);
	form->addRow(QStringLiteral("Color:"), edit_safe_area_color_);

	spin_safe_area_opacity_ = new QDoubleSpinBox(grp_safe_area_);
	spin_safe_area_opacity_->setRange(0.0, 1.0);
	spin_safe_area_opacity_->setSingleStep(0.1);
	spin_safe_area_opacity_->setDecimals(2);
	form->addRow(QStringLiteral("Opacity:"), spin_safe_area_opacity_);

	layout->addLayout(form);

	HOOK_CHECK(chk_safe_area_enabled_);
	HOOK_EDIT(edit_safe_area_color_);
	HOOK_DSPIN(spin_safe_area_opacity_);

	return grp_safe_area_;
}

/* ---- VU Meter Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_vu_meter_group()
{
	grp_vu_meter_ = new QGroupBox(QStringLiteral("VU Meter"), this);
	auto *layout = new QVBoxLayout(grp_vu_meter_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(QStringLiteral("Inheritance:"), grp_vu_meter_));
		cmb_vu_meter_inherit_ = create_inherit_combo(grp_vu_meter_);
		inh_row->addWidget(cmb_vu_meter_inherit_);
		inh_row->addStretch();
		layout->addLayout(inh_row);
		connect(cmb_vu_meter_inherit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
			update_inheritance_visibility();
			dirty_ = true;
			emit settings_changed();
		});
	}

	auto *form = new QFormLayout();

	chk_vu_enabled_ = new QCheckBox(grp_vu_meter_);
	form->addRow(QStringLiteral("Enabled:"), chk_vu_enabled_);

	cmb_vu_position_ = new QComboBox(grp_vu_meter_);
	cmb_vu_position_->addItem(QStringLiteral("Left"), (int)VuMeterPosition::Left);
	cmb_vu_position_->addItem(QStringLiteral("Right"), (int)VuMeterPosition::Right);
	cmb_vu_position_->addItem(QStringLiteral("Top"), (int)VuMeterPosition::Top);
	cmb_vu_position_->addItem(QStringLiteral("Bottom"), (int)VuMeterPosition::Bottom);
	form->addRow(QStringLiteral("Position:"), cmb_vu_position_);

	cmb_vu_anchor_ = new QComboBox(grp_vu_meter_);
	cmb_vu_anchor_->addItem(QStringLiteral("Cell"), (int)VuMeterAnchorMode::Cell);
	cmb_vu_anchor_->addItem(QStringLiteral("Signal"), (int)VuMeterAnchorMode::Signal);
	form->addRow(QStringLiteral("Anchor:"), cmb_vu_anchor_);

	spin_vu_width_ = new QSpinBox(grp_vu_meter_);
	spin_vu_width_->setRange(1, 64);
	form->addRow(QStringLiteral("Width (px):"), spin_vu_width_);

	spin_vu_opacity_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_opacity_->setRange(0.0, 1.0);
	spin_vu_opacity_->setSingleStep(0.05);
	spin_vu_opacity_->setDecimals(2);
	form->addRow(QStringLiteral("Opacity:"), spin_vu_opacity_);

	spin_vu_length_ratio_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_length_ratio_->setRange(0.1, 1.0);
	spin_vu_length_ratio_->setSingleStep(0.05);
	spin_vu_length_ratio_->setDecimals(2);
	form->addRow(QStringLiteral("Length Ratio:"), spin_vu_length_ratio_);

	cmb_vu_alignment_ = new QComboBox(grp_vu_meter_);
	cmb_vu_alignment_->addItem(QStringLiteral("Start (-∞ anchor)"), (int)VuMeterAlignment::Start);
	cmb_vu_alignment_->addItem(QStringLiteral("Center"), (int)VuMeterAlignment::Center);
	form->addRow(QStringLiteral("Alignment:"), cmb_vu_alignment_);

	spin_vu_warning_db_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_warning_db_->setRange(-60.0, 0.0);
	spin_vu_warning_db_->setSingleStep(1.0);
	spin_vu_warning_db_->setDecimals(1);
	spin_vu_warning_db_->setSuffix(QStringLiteral(" dB"));
	form->addRow(QStringLiteral("Warning:"), spin_vu_warning_db_);

	spin_vu_error_db_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_error_db_->setRange(-60.0, 0.0);
	spin_vu_error_db_->setSingleStep(1.0);
	spin_vu_error_db_->setDecimals(1);
	spin_vu_error_db_->setSuffix(QStringLiteral(" dB"));
	form->addRow(QStringLiteral("Error:"), spin_vu_error_db_);

	cmb_vu_decay_ = new QComboBox(grp_vu_meter_);
	cmb_vu_decay_->addItem(QStringLiteral("Fast (23.5 dB/s)"), (int)VuMeterDecayRate::Fast);
	cmb_vu_decay_->addItem(QStringLiteral("Medium (11.76 dB/s)"), (int)VuMeterDecayRate::Medium);
	cmb_vu_decay_->addItem(QStringLiteral("Slow (8.57 dB/s)"), (int)VuMeterDecayRate::Slow);
	form->addRow(QStringLiteral("Decay Rate:"), cmb_vu_decay_);

	chk_vu_flip_ = new QCheckBox(grp_vu_meter_);
	form->addRow(QStringLiteral("Flip:"), chk_vu_flip_);

	layout->addLayout(form);

	HOOK_CHECK(chk_vu_enabled_);
	HOOK_COMBO(cmb_vu_position_);
	HOOK_COMBO(cmb_vu_anchor_);
	HOOK_SPIN(spin_vu_width_);
	HOOK_DSPIN(spin_vu_opacity_);
	HOOK_DSPIN(spin_vu_length_ratio_);
	HOOK_COMBO(cmb_vu_alignment_);
	HOOK_DSPIN(spin_vu_warning_db_);
	HOOK_DSPIN(spin_vu_error_db_);
	HOOK_COMBO(cmb_vu_decay_);
	HOOK_CHECK(chk_vu_flip_);

	return grp_vu_meter_;
}

/* ---- Overlay Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_overlay_group()
{
	grp_overlay_ = new QGroupBox(QStringLiteral("Overlay"), this);
	auto *layout = new QVBoxLayout(grp_overlay_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(QStringLiteral("Inheritance:"), grp_overlay_));
		cmb_overlay_inherit_ = create_inherit_combo(grp_overlay_);
		inh_row->addWidget(cmb_overlay_inherit_);
		inh_row->addStretch();
		layout->addLayout(inh_row);
		connect(cmb_overlay_inherit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
			update_inheritance_visibility();
			dirty_ = true;
			emit settings_changed();
		});
	}

	auto *form = new QFormLayout();

	chk_overlay_enabled_ = new QCheckBox(grp_overlay_);
	form->addRow(QStringLiteral("Enabled:"), chk_overlay_enabled_);

	edit_overlay_path_ = new QLineEdit(grp_overlay_);
	form->addRow(QStringLiteral("Image Path:"), edit_overlay_path_);

	spin_overlay_opacity_ = new QDoubleSpinBox(grp_overlay_);
	spin_overlay_opacity_->setRange(0.0, 1.0);
	spin_overlay_opacity_->setSingleStep(0.05);
	spin_overlay_opacity_->setDecimals(2);
	form->addRow(QStringLiteral("Opacity:"), spin_overlay_opacity_);

	cmb_overlay_fit_ = new QComboBox(grp_overlay_);
	cmb_overlay_fit_->addItem(QStringLiteral("Fit"), (int)OverlayFitMode::Fit);
	cmb_overlay_fit_->addItem(QStringLiteral("Stretch"), (int)OverlayFitMode::Stretch);
	form->addRow(QStringLiteral("Fit Mode:"), cmb_overlay_fit_);

	cmb_overlay_anchor_ = new QComboBox(grp_overlay_);
	cmb_overlay_anchor_->addItem(QStringLiteral("Cell"), (int)OverlayAnchorMode::Cell);
	cmb_overlay_anchor_->addItem(QStringLiteral("Signal"), (int)OverlayAnchorMode::Signal);
	form->addRow(QStringLiteral("Anchor:"), cmb_overlay_anchor_);

	layout->addLayout(form);

	HOOK_CHECK(chk_overlay_enabled_);
	HOOK_EDIT(edit_overlay_path_);
	HOOK_DSPIN(spin_overlay_opacity_);
	HOOK_COMBO(cmb_overlay_fit_);
	HOOK_COMBO(cmb_overlay_anchor_);

	return grp_overlay_;
}

/* ---- Inheritance visibility ---- */

void CellDisplaySettingsDialog::update_inheritance_visibility()
{
	if (mode_ == Mode::Global)
		return;

	/* Helper: enable/disable group contents based on inherit combo */
	auto toggle_group = [](QGroupBox *grp, QComboBox *cmb) {
		if (!cmb)
			return;
		bool isOverride = (cmb->currentIndex() == 1);
		/* Enable all children except the inheritance combo row itself */
		for (auto *child : grp->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
			if (child != cmb && !qobject_cast<QLabel *>(child))
				child->setEnabled(isOverride);
		}
		/* Also handle form children inside nested layouts */
		for (auto *w : grp->findChildren<QComboBox *>()) {
			if (w != cmb)
				w->setEnabled(isOverride);
		}
		for (auto *w : grp->findChildren<QSpinBox *>())
			w->setEnabled(isOverride);
		for (auto *w : grp->findChildren<QDoubleSpinBox *>())
			w->setEnabled(isOverride);
		for (auto *w : grp->findChildren<QCheckBox *>())
			w->setEnabled(isOverride);
		for (auto *w : grp->findChildren<QLineEdit *>())
			w->setEnabled(isOverride);
	};

	toggle_group(grp_background_, cmb_bg_inherit_);
	toggle_group(grp_label_, cmb_label_inherit_);
	toggle_group(grp_safe_area_, cmb_safe_area_inherit_);
	toggle_group(grp_vu_meter_, cmb_vu_meter_inherit_);
	toggle_group(grp_overlay_, cmb_overlay_inherit_);
}

/* ---- Get/Set: Global ---- */

void CellDisplaySettingsDialog::set_global_settings(const GlobalVisualSettings &gs)
{
	/* Background */
	chk_bg_color_enabled_->setChecked(gs.background.colorEnabled);
	edit_bg_color_->setText(color_to_hex(gs.background.color));
	cmb_bg_fill_mode_->setCurrentIndex(gs.background.fillMode == BackgroundFillMode::FillEntireCell ? 1 : 0);
	chk_bg_label_fill_->setChecked(gs.background.labelRegionFill);
	chk_bg_image_enabled_->setChecked(gs.background.imageEnabled);
	edit_bg_image_path_->setText(QString::fromStdString(gs.background.imagePath));
	cmb_bg_image_fit_->setCurrentIndex(gs.background.imageFitMode == ImageFitMode::Stretch ? 1 : 0);

	/* Label */
	cmb_label_display_->setCurrentIndex((int)gs.label.displayMode);
	cmb_label_position_->setCurrentIndex((int)gs.label.position);
	spin_label_font_size_->setValue(gs.label.fontSize);
	cmb_label_scale_mode_->setCurrentIndex((int)gs.label.fontScaleMode);
	spin_label_min_font_->setValue(gs.label.minFontSize);
	spin_label_max_font_->setValue(gs.label.maxFontSize);
	edit_label_text_color_->setText(color_to_hex(gs.label.textColor));
	spin_label_bg_opacity_->setValue(gs.label.backgroundOpacity);
	spin_label_margin_->setValue(gs.label.margin);

	/* Safe Area */
	chk_safe_area_enabled_->setChecked(gs.safeArea.enabled);
	edit_safe_area_color_->setText(color_to_hex(gs.safeArea.color));
	spin_safe_area_opacity_->setValue(gs.safeArea.opacity);

	/* VU Meter */
	chk_vu_enabled_->setChecked(gs.vuMeter.enabled);
	cmb_vu_position_->setCurrentIndex(cmb_vu_position_->findData((int)gs.vuMeter.position));
	cmb_vu_anchor_->setCurrentIndex((int)gs.vuMeter.anchor);
	spin_vu_width_->setValue(gs.vuMeter.width);
	spin_vu_opacity_->setValue(gs.vuMeter.opacity);
	spin_vu_length_ratio_->setValue(gs.vuMeter.lengthRatio);
	cmb_vu_alignment_->setCurrentIndex((int)gs.vuMeter.alignment);
	spin_vu_warning_db_->setValue(gs.vuMeter.warningDB);
	spin_vu_error_db_->setValue(gs.vuMeter.errorDB);
	cmb_vu_decay_->setCurrentIndex((int)gs.vuMeter.decayRate);
	chk_vu_flip_->setChecked(gs.vuMeter.flip);

	/* Overlay */
	chk_overlay_enabled_->setChecked(gs.overlay.enabled);
	edit_overlay_path_->setText(QString::fromStdString(gs.overlay.imagePath));
	spin_overlay_opacity_->setValue(gs.overlay.opacity);
	cmb_overlay_fit_->setCurrentIndex((int)gs.overlay.fitMode);
	cmb_overlay_anchor_->setCurrentIndex((int)gs.overlay.anchorMode);

	dirty_ = false;
}

GlobalVisualSettings CellDisplaySettingsDialog::get_global_settings() const
{
	GlobalVisualSettings gs;

	/* Background */
	gs.background.colorEnabled = chk_bg_color_enabled_->isChecked();
	gs.background.color = hex_to_color(edit_bg_color_->text(), 0xFF000000);
	gs.background.fillMode = cmb_bg_fill_mode_->currentIndex() == 1 ? BackgroundFillMode::FillEntireCell
									: BackgroundFillMode::FillSignalOnly;
	gs.background.labelRegionFill = chk_bg_label_fill_->isChecked();
	gs.background.imageEnabled = chk_bg_image_enabled_->isChecked();
	gs.background.imagePath = edit_bg_image_path_->text().toStdString();
	gs.background.imageFitMode = cmb_bg_image_fit_->currentIndex() == 1 ? ImageFitMode::Stretch : ImageFitMode::Fit;

	/* Label */
	gs.label.displayMode = (LabelDisplayMode)cmb_label_display_->currentIndex();
	gs.label.position = (LabelPosition)cmb_label_position_->currentIndex();
	gs.label.fontSize = spin_label_font_size_->value();
	gs.label.fontScaleMode = (FontScaleMode)cmb_label_scale_mode_->currentIndex();
	gs.label.minFontSize = spin_label_min_font_->value();
	gs.label.maxFontSize = spin_label_max_font_->value();
	gs.label.textColor = hex_to_color(edit_label_text_color_->text(), 0xFFFFFFFF);
	gs.label.backgroundOpacity = spin_label_bg_opacity_->value();
	gs.label.margin = spin_label_margin_->value();

	/* Safe Area */
	gs.safeArea.enabled = chk_safe_area_enabled_->isChecked();
	gs.safeArea.color = hex_to_color(edit_safe_area_color_->text(), 0xFFD0D0D0);
	gs.safeArea.opacity = spin_safe_area_opacity_->value();

	/* VU Meter */
	gs.vuMeter.enabled = chk_vu_enabled_->isChecked();
	gs.vuMeter.position = (VuMeterPosition)cmb_vu_position_->currentData().toInt();
	gs.vuMeter.anchor = (VuMeterAnchorMode)cmb_vu_anchor_->currentIndex();
	gs.vuMeter.width = spin_vu_width_->value();
	gs.vuMeter.opacity = spin_vu_opacity_->value();
	gs.vuMeter.lengthRatio = spin_vu_length_ratio_->value();
	gs.vuMeter.alignment = (VuMeterAlignment)cmb_vu_alignment_->currentIndex();
	gs.vuMeter.warningDB = spin_vu_warning_db_->value();
	gs.vuMeter.errorDB = spin_vu_error_db_->value();
	gs.vuMeter.decayRate = (VuMeterDecayRate)cmb_vu_decay_->currentIndex();
	gs.vuMeter.flip = chk_vu_flip_->isChecked();

	/* Overlay */
	gs.overlay.enabled = chk_overlay_enabled_->isChecked();
	gs.overlay.imagePath = edit_overlay_path_->text().toStdString();
	gs.overlay.opacity = spin_overlay_opacity_->value();
	gs.overlay.fitMode = (OverlayFitMode)cmb_overlay_fit_->currentIndex();
	gs.overlay.anchorMode = (OverlayAnchorMode)cmb_overlay_anchor_->currentIndex();

	return gs;
}

/* ---- Get/Set: Instance ---- */

void CellDisplaySettingsDialog::set_instance_settings(const InstanceVisualSettings &is)
{
	if (cmb_bg_inherit_)
		set_inherit_combo(cmb_bg_inherit_, is.backgroundMode);
	if (cmb_label_inherit_)
		set_inherit_combo(cmb_label_inherit_, is.labelMode);
	if (cmb_safe_area_inherit_)
		set_inherit_combo(cmb_safe_area_inherit_, is.safeAreaMode);
	if (cmb_vu_meter_inherit_)
		set_inherit_combo(cmb_vu_meter_inherit_, is.vuMeterMode);
	if (cmb_overlay_inherit_)
		set_inherit_combo(cmb_overlay_inherit_, is.overlayMode);

	/* Populate editors with instance values */
	chk_bg_color_enabled_->setChecked(is.background.colorEnabled);
	edit_bg_color_->setText(color_to_hex(is.background.color));
	cmb_bg_fill_mode_->setCurrentIndex(is.background.fillMode == BackgroundFillMode::FillEntireCell ? 1 : 0);
	chk_bg_label_fill_->setChecked(is.background.labelRegionFill);
	chk_bg_image_enabled_->setChecked(is.background.imageEnabled);
	edit_bg_image_path_->setText(QString::fromStdString(is.background.imagePath));
	cmb_bg_image_fit_->setCurrentIndex(is.background.imageFitMode == ImageFitMode::Stretch ? 1 : 0);

	cmb_label_display_->setCurrentIndex((int)is.label.displayMode);
	cmb_label_position_->setCurrentIndex((int)is.label.position);
	spin_label_font_size_->setValue(is.label.fontSize);
	cmb_label_scale_mode_->setCurrentIndex((int)is.label.fontScaleMode);
	spin_label_min_font_->setValue(is.label.minFontSize);
	spin_label_max_font_->setValue(is.label.maxFontSize);
	edit_label_text_color_->setText(color_to_hex(is.label.textColor));
	spin_label_bg_opacity_->setValue(is.label.backgroundOpacity);
	spin_label_margin_->setValue(is.label.margin);

	chk_safe_area_enabled_->setChecked(is.safeArea.enabled);
	edit_safe_area_color_->setText(color_to_hex(is.safeArea.color));
	spin_safe_area_opacity_->setValue(is.safeArea.opacity);

	chk_vu_enabled_->setChecked(is.vuMeter.enabled);
	cmb_vu_position_->setCurrentIndex(cmb_vu_position_->findData((int)is.vuMeter.position));
	cmb_vu_anchor_->setCurrentIndex((int)is.vuMeter.anchor);
	spin_vu_width_->setValue(is.vuMeter.width);
	spin_vu_opacity_->setValue(is.vuMeter.opacity);
	spin_vu_length_ratio_->setValue(is.vuMeter.lengthRatio);
	cmb_vu_alignment_->setCurrentIndex((int)is.vuMeter.alignment);
	spin_vu_warning_db_->setValue(is.vuMeter.warningDB);
	spin_vu_error_db_->setValue(is.vuMeter.errorDB);
	cmb_vu_decay_->setCurrentIndex((int)is.vuMeter.decayRate);
	chk_vu_flip_->setChecked(is.vuMeter.flip);

	chk_overlay_enabled_->setChecked(is.overlay.enabled);
	edit_overlay_path_->setText(QString::fromStdString(is.overlay.imagePath));
	spin_overlay_opacity_->setValue(is.overlay.opacity);
	cmb_overlay_fit_->setCurrentIndex((int)is.overlay.fitMode);
	cmb_overlay_anchor_->setCurrentIndex((int)is.overlay.anchorMode);

	update_inheritance_visibility();
	dirty_ = false;
}

InstanceVisualSettings CellDisplaySettingsDialog::get_instance_settings() const
{
	InstanceVisualSettings is;

	if (cmb_bg_inherit_)
		is.backgroundMode = get_inherit_combo(cmb_bg_inherit_);
	if (cmb_label_inherit_)
		is.labelMode = get_inherit_combo(cmb_label_inherit_);
	if (cmb_safe_area_inherit_)
		is.safeAreaMode = get_inherit_combo(cmb_safe_area_inherit_);
	if (cmb_vu_meter_inherit_)
		is.vuMeterMode = get_inherit_combo(cmb_vu_meter_inherit_);
	if (cmb_overlay_inherit_)
		is.overlayMode = get_inherit_combo(cmb_overlay_inherit_);

	/* Background */
	is.background.colorEnabled = chk_bg_color_enabled_->isChecked();
	is.background.color = hex_to_color(edit_bg_color_->text(), 0xFF000000);
	is.background.fillMode = cmb_bg_fill_mode_->currentIndex() == 1 ? BackgroundFillMode::FillEntireCell
									: BackgroundFillMode::FillSignalOnly;
	is.background.labelRegionFill = chk_bg_label_fill_->isChecked();
	is.background.imageEnabled = chk_bg_image_enabled_->isChecked();
	is.background.imagePath = edit_bg_image_path_->text().toStdString();
	is.background.imageFitMode = cmb_bg_image_fit_->currentIndex() == 1 ? ImageFitMode::Stretch : ImageFitMode::Fit;

	/* Label */
	is.label.displayMode = (LabelDisplayMode)cmb_label_display_->currentIndex();
	is.label.position = (LabelPosition)cmb_label_position_->currentIndex();
	is.label.fontSize = spin_label_font_size_->value();
	is.label.fontScaleMode = (FontScaleMode)cmb_label_scale_mode_->currentIndex();
	is.label.minFontSize = spin_label_min_font_->value();
	is.label.maxFontSize = spin_label_max_font_->value();
	is.label.textColor = hex_to_color(edit_label_text_color_->text(), 0xFFFFFFFF);
	is.label.backgroundOpacity = spin_label_bg_opacity_->value();
	is.label.margin = spin_label_margin_->value();

	/* Safe Area */
	is.safeArea.enabled = chk_safe_area_enabled_->isChecked();
	is.safeArea.color = hex_to_color(edit_safe_area_color_->text(), 0xFFD0D0D0);
	is.safeArea.opacity = spin_safe_area_opacity_->value();

	/* VU Meter */
	is.vuMeter.enabled = chk_vu_enabled_->isChecked();
	is.vuMeter.position = (VuMeterPosition)cmb_vu_position_->currentData().toInt();
	is.vuMeter.anchor = (VuMeterAnchorMode)cmb_vu_anchor_->currentIndex();
	is.vuMeter.width = spin_vu_width_->value();
	is.vuMeter.opacity = spin_vu_opacity_->value();
	is.vuMeter.lengthRatio = spin_vu_length_ratio_->value();
	is.vuMeter.alignment = (VuMeterAlignment)cmb_vu_alignment_->currentIndex();
	is.vuMeter.warningDB = spin_vu_warning_db_->value();
	is.vuMeter.errorDB = spin_vu_error_db_->value();
	is.vuMeter.decayRate = (VuMeterDecayRate)cmb_vu_decay_->currentIndex();
	is.vuMeter.flip = chk_vu_flip_->isChecked();

	/* Overlay */
	is.overlay.enabled = chk_overlay_enabled_->isChecked();
	is.overlay.imagePath = edit_overlay_path_->text().toStdString();
	is.overlay.opacity = spin_overlay_opacity_->value();
	is.overlay.fitMode = (OverlayFitMode)cmb_overlay_fit_->currentIndex();
	is.overlay.anchorMode = (OverlayAnchorMode)cmb_overlay_anchor_->currentIndex();

	return is;
}

/* ---- Get/Set: Cell ---- */

void CellDisplaySettingsDialog::set_cell_settings(const CellVisualSettings &cs)
{
	if (cmb_bg_inherit_)
		set_inherit_combo(cmb_bg_inherit_, cs.backgroundMode);
	if (cmb_label_inherit_)
		set_inherit_combo(cmb_label_inherit_, cs.labelMode);
	if (cmb_safe_area_inherit_)
		set_inherit_combo(cmb_safe_area_inherit_, cs.safeAreaMode);
	if (cmb_vu_meter_inherit_)
		set_inherit_combo(cmb_vu_meter_inherit_, cs.vuMeterMode);
	if (cmb_overlay_inherit_)
		set_inherit_combo(cmb_overlay_inherit_, cs.overlayMode);

	/* Populate editors with cell values */
	chk_bg_color_enabled_->setChecked(cs.background.colorEnabled);
	edit_bg_color_->setText(color_to_hex(cs.background.color));
	cmb_bg_fill_mode_->setCurrentIndex(cs.background.fillMode == BackgroundFillMode::FillEntireCell ? 1 : 0);
	chk_bg_label_fill_->setChecked(cs.background.labelRegionFill);
	chk_bg_image_enabled_->setChecked(cs.background.imageEnabled);
	edit_bg_image_path_->setText(QString::fromStdString(cs.background.imagePath));
	cmb_bg_image_fit_->setCurrentIndex(cs.background.imageFitMode == ImageFitMode::Stretch ? 1 : 0);

	cmb_label_display_->setCurrentIndex((int)cs.label.displayMode);
	cmb_label_position_->setCurrentIndex((int)cs.label.position);
	spin_label_font_size_->setValue(cs.label.fontSize);
	cmb_label_scale_mode_->setCurrentIndex((int)cs.label.fontScaleMode);
	spin_label_min_font_->setValue(cs.label.minFontSize);
	spin_label_max_font_->setValue(cs.label.maxFontSize);
	edit_label_text_color_->setText(color_to_hex(cs.label.textColor));
	spin_label_bg_opacity_->setValue(cs.label.backgroundOpacity);
	spin_label_margin_->setValue(cs.label.margin);

	chk_safe_area_enabled_->setChecked(cs.safeArea.enabled);
	edit_safe_area_color_->setText(color_to_hex(cs.safeArea.color));
	spin_safe_area_opacity_->setValue(cs.safeArea.opacity);

	chk_vu_enabled_->setChecked(cs.vuMeter.enabled);
	cmb_vu_position_->setCurrentIndex(cmb_vu_position_->findData((int)cs.vuMeter.position));
	cmb_vu_anchor_->setCurrentIndex((int)cs.vuMeter.anchor);
	spin_vu_width_->setValue(cs.vuMeter.width);
	spin_vu_opacity_->setValue(cs.vuMeter.opacity);
	spin_vu_length_ratio_->setValue(cs.vuMeter.lengthRatio);
	cmb_vu_alignment_->setCurrentIndex((int)cs.vuMeter.alignment);
	spin_vu_warning_db_->setValue(cs.vuMeter.warningDB);
	spin_vu_error_db_->setValue(cs.vuMeter.errorDB);
	cmb_vu_decay_->setCurrentIndex((int)cs.vuMeter.decayRate);
	chk_vu_flip_->setChecked(cs.vuMeter.flip);

	chk_overlay_enabled_->setChecked(cs.overlay.enabled);
	edit_overlay_path_->setText(QString::fromStdString(cs.overlay.imagePath));
	spin_overlay_opacity_->setValue(cs.overlay.opacity);
	cmb_overlay_fit_->setCurrentIndex((int)cs.overlay.fitMode);
	cmb_overlay_anchor_->setCurrentIndex((int)cs.overlay.anchorMode);

	update_inheritance_visibility();
	dirty_ = false;
}

CellVisualSettings CellDisplaySettingsDialog::get_cell_settings() const
{
	CellVisualSettings cs;
	cs.row = cell_row_;
	cs.col = cell_col_;

	if (cmb_bg_inherit_)
		cs.backgroundMode = get_inherit_combo(cmb_bg_inherit_);
	if (cmb_label_inherit_)
		cs.labelMode = get_inherit_combo(cmb_label_inherit_);
	if (cmb_safe_area_inherit_)
		cs.safeAreaMode = get_inherit_combo(cmb_safe_area_inherit_);
	if (cmb_vu_meter_inherit_)
		cs.vuMeterMode = get_inherit_combo(cmb_vu_meter_inherit_);
	if (cmb_overlay_inherit_)
		cs.overlayMode = get_inherit_combo(cmb_overlay_inherit_);

	/* Background */
	cs.background.colorEnabled = chk_bg_color_enabled_->isChecked();
	cs.background.color = hex_to_color(edit_bg_color_->text(), 0xFF000000);
	cs.background.fillMode = cmb_bg_fill_mode_->currentIndex() == 1 ? BackgroundFillMode::FillEntireCell
									: BackgroundFillMode::FillSignalOnly;
	cs.background.labelRegionFill = chk_bg_label_fill_->isChecked();
	cs.background.imageEnabled = chk_bg_image_enabled_->isChecked();
	cs.background.imagePath = edit_bg_image_path_->text().toStdString();
	cs.background.imageFitMode = cmb_bg_image_fit_->currentIndex() == 1 ? ImageFitMode::Stretch : ImageFitMode::Fit;

	/* Label */
	cs.label.displayMode = (LabelDisplayMode)cmb_label_display_->currentIndex();
	cs.label.position = (LabelPosition)cmb_label_position_->currentIndex();
	cs.label.fontSize = spin_label_font_size_->value();
	cs.label.fontScaleMode = (FontScaleMode)cmb_label_scale_mode_->currentIndex();
	cs.label.minFontSize = spin_label_min_font_->value();
	cs.label.maxFontSize = spin_label_max_font_->value();
	cs.label.textColor = hex_to_color(edit_label_text_color_->text(), 0xFFFFFFFF);
	cs.label.backgroundOpacity = spin_label_bg_opacity_->value();
	cs.label.margin = spin_label_margin_->value();

	/* Safe Area */
	cs.safeArea.enabled = chk_safe_area_enabled_->isChecked();
	cs.safeArea.color = hex_to_color(edit_safe_area_color_->text(), 0xFFD0D0D0);
	cs.safeArea.opacity = spin_safe_area_opacity_->value();

	/* VU Meter */
	cs.vuMeter.enabled = chk_vu_enabled_->isChecked();
	cs.vuMeter.position = (VuMeterPosition)cmb_vu_position_->currentData().toInt();
	cs.vuMeter.anchor = (VuMeterAnchorMode)cmb_vu_anchor_->currentIndex();
	cs.vuMeter.width = spin_vu_width_->value();
	cs.vuMeter.opacity = spin_vu_opacity_->value();
	cs.vuMeter.lengthRatio = spin_vu_length_ratio_->value();
	cs.vuMeter.alignment = (VuMeterAlignment)cmb_vu_alignment_->currentIndex();
	cs.vuMeter.warningDB = spin_vu_warning_db_->value();
	cs.vuMeter.errorDB = spin_vu_error_db_->value();
	cs.vuMeter.decayRate = (VuMeterDecayRate)cmb_vu_decay_->currentIndex();
	cs.vuMeter.flip = chk_vu_flip_->isChecked();

	/* Overlay */
	cs.overlay.enabled = chk_overlay_enabled_->isChecked();
	cs.overlay.imagePath = edit_overlay_path_->text().toStdString();
	cs.overlay.opacity = spin_overlay_opacity_->value();
	cs.overlay.fitMode = (OverlayFitMode)cmb_overlay_fit_->currentIndex();
	cs.overlay.anchorMode = (OverlayAnchorMode)cmb_overlay_anchor_->currentIndex();

	return cs;
}

/* ================================================================
 * Copy / Paste / Reset
 * ================================================================ */

QByteArray CellDisplaySettingsDialog::s_clipboard_;

void CellDisplaySettingsDialog::on_copy()
{
	obs_data_t *data = nullptr;
	switch (mode_) {
	case Mode::Global:
		data = get_global_settings().to_obs_data();
		break;
	case Mode::Instance:
		data = get_instance_settings().to_obs_data();
		break;
	case Mode::Cell:
		data = get_cell_settings().to_obs_data();
		break;
	}
	if (data) {
		const char *json = obs_data_get_json(data);
		s_clipboard_ = QByteArray(json);
		obs_data_release(data);
	}
}

void CellDisplaySettingsDialog::on_paste()
{
	if (s_clipboard_.isEmpty())
		return;
	obs_data_t *data = obs_data_create_from_json(s_clipboard_.constData());
	if (!data)
		return;
	switch (mode_) {
	case Mode::Global:
		set_global_settings(GlobalVisualSettings::from_obs_data(data));
		break;
	case Mode::Instance:
		set_instance_settings(InstanceVisualSettings::from_obs_data(data));
		break;
	case Mode::Cell:
		set_cell_settings(CellVisualSettings::from_obs_data(data));
		break;
	}
	obs_data_release(data);
}

void CellDisplaySettingsDialog::on_reset()
{
	switch (mode_) {
	case Mode::Global:
		set_global_settings(GlobalVisualSettings{});
		break;
	case Mode::Instance:
		set_instance_settings(InstanceVisualSettings{});
		break;
	case Mode::Cell: {
		CellVisualSettings cs{};
		cs.row = cell_row_;
		cs.col = cell_col_;
		set_cell_settings(cs);
		break;
	}
	}
}
