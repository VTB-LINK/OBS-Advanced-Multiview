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
#include "amv-i18n.hpp"

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <functional>

/* ---- Inheritance combo helper ---- */

static QComboBox *create_inherit_combo(QWidget *parent)
{
	auto *cmb = new QComboBox(parent);
	cmb->addItem(amv::text("AMVPlugin.Common.Inherit"), (int)InheritanceMode::Inherit);
	cmb->addItem(amv::text("AMVPlugin.Common.Override"), (int)InheritanceMode::Override);
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

/* ---- Color picker widget: line edit + colored swatch button ----
 * Uses Qt's native QColorDialog (the same picker OBS uses internally
 * for its properties view). */
static QWidget *build_color_picker(QLineEdit *edit, QWidget *parent, const QString &title)
{
	auto *container = new QWidget(parent);
	auto *h = new QHBoxLayout(container);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(4);

	edit->setParent(container);
	edit->setMaximumWidth(100);
	h->addWidget(edit);

	auto *swatch = new QPushButton(container);
	swatch->setFixedSize(28, 22);
	swatch->setCursor(Qt::PointingHandCursor);
	swatch->setToolTip(amv::text("AMVPlugin.Common.PickColor"));

	auto refresh_swatch = [swatch, edit]() {
		QString hex = edit->text().trimmed();
		if (!hex.startsWith('#'))
			hex = '#' + hex;
		QColor c(hex);
		if (!c.isValid())
			c = Qt::black;
		swatch->setStyleSheet(
			QStringLiteral(
				"QPushButton { background-color: %1; border: 1px solid #555; border-radius: 2px; }"
				"QPushButton:hover { border: 1px solid #888; }")
				.arg(c.name(QColor::HexRgb)));
	};
	refresh_swatch();

	QObject::connect(edit, &QLineEdit::textChanged, swatch,
			 [refresh_swatch](const QString &) { refresh_swatch(); });

	QObject::connect(swatch, &QPushButton::clicked, swatch, [edit, parent, title]() {
		QString hex = edit->text().trimmed();
		if (!hex.startsWith('#'))
			hex = '#' + hex;
		QColor initial(hex);
		if (!initial.isValid())
			initial = Qt::white;
		QColor chosen = QColorDialog::getColor(initial, parent, title);
		if (chosen.isValid())
			edit->setText(chosen.name(QColor::HexRgb).toUpper());
	});

	h->addWidget(swatch);
	h->addStretch();
	return container;
}

/* ---- File picker widget: line edit + Browse button ----
 * Allows both direct path input and a native file picker. */
static QWidget *build_file_picker(QLineEdit *edit, QWidget *parent, const QString &title)
{
	auto *container = new QWidget(parent);
	auto *h = new QHBoxLayout(container);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(4);

	edit->setParent(container);
	h->addWidget(edit, 1);

	auto *browse = new QPushButton(amv::text("AMVPlugin.Common.Browse"), container);
	browse->setCursor(Qt::PointingHandCursor);

	QObject::connect(browse, &QPushButton::clicked, browse, [edit, parent, title]() {
		QString current = edit->text().trimmed();
		QString startDir;
		if (!current.isEmpty()) {
			QFileInfo fi(current);
			startDir = fi.exists() ? fi.absolutePath() : current;
		}
		QString fn =
			QFileDialog::getOpenFileName(parent, title, startDir, amv::text("AMVPlugin.Common.AllFiles"));
		if (!fn.isEmpty())
			edit->setText(fn);
	});

	h->addWidget(browse);
	return container;
}

/* ---- Font picker button (mimics OBS's text_gdiplus / obs_properties_add_font widget,
 * which uses QFontDialog::getFont() internally). The button itself is rendered using
 * the selected font as a live preview, exactly like OBS's font property. ----
 *
 * The selected font family name is stored on the button via dynamic property
 * "fontFamily" so set/get_*_settings() can read/write it without an extra member.
 * The on_changed callback is invoked when the user picks a new font.
 */
static void font_picker_refresh(QPushButton *btn)
{
	QString family = btn->property("fontFamily").toString();
	QFont f = btn->font();
	if (family.isEmpty()) {
		btn->setText(amv::text("AMVPlugin.Common.FontDefault"));
		f.setFamily(QApplication::font().family());
	} else {
		btn->setText(family);
		f.setFamily(family);
	}
	btn->setFont(f);
}

static QPushButton *build_font_picker(QWidget *parent, std::function<void()> on_changed)
{
	auto *btn = new QPushButton(parent);
	btn->setCursor(Qt::PointingHandCursor);
	btn->setProperty("fontFamily", QString());
	font_picker_refresh(btn);

	QObject::connect(btn, &QPushButton::clicked, btn, [btn, parent, on_changed]() {
		QString current = btn->property("fontFamily").toString();
		QFont initial;
		if (!current.isEmpty())
			initial.setFamily(current);

		bool ok = false;
		QFont chosen = QFontDialog::getFont(&ok, initial, parent, amv::text("AMVPlugin.Common.PickFont"));
		if (!ok)
			return;
		if (chosen.family() == current)
			return;
		btn->setProperty("fontFamily", chosen.family());
		font_picker_refresh(btn);
		if (on_changed)
			on_changed();
	});

	return btn;
}

static QString font_picker_get(QPushButton *btn)
{
	return btn->property("fontFamily").toString();
}

static void font_picker_set(QPushButton *btn, const QString &family)
{
	btn->setProperty("fontFamily", family);
	font_picker_refresh(btn);
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
		setWindowTitle(amv::text("AMVPlugin.Visual.Title.CellPosition").arg(row).arg(col));
	}
}

void CellDisplaySettingsDialog::set_external_cell(bool external)
{
	is_external_cell_ = external;
	if (cmb_vu_track_mode_ && mode_ == Mode::Cell) {
		int externalIdx = cmb_vu_track_mode_->findData((int)VuMeterTrackMode::ExternalSource);
		if (external && externalIdx < 0) {
			cmb_vu_track_mode_->addItem(amv::text("AMVPlugin.Visual.VUMeter.Track.ExternalSource"),
						    (int)VuMeterTrackMode::ExternalSource);
		} else if (!external && externalIdx >= 0) {
			cmb_vu_track_mode_->removeItem(externalIdx);
		}
	}
	update_vu_meter_control_states();
}

/* ---- UI Setup ---- */

void CellDisplaySettingsDialog::setup_ui()
{
	switch (mode_) {
	case Mode::Global:
		setWindowTitle(amv::text("AMVPlugin.Visual.Title.Global"));
		break;
	case Mode::Instance:
		setWindowTitle(amv::text("AMVPlugin.Visual.Title.Instance"));
		break;
	case Mode::Cell:
		setWindowTitle(amv::text("AMVPlugin.Visual.Title.Cell"));
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
	scrollLayout->addWidget(create_highlight_group());
	scrollLayout->addStretch();

	scrollArea->setWidget(scrollWidget);
	mainLayout->addWidget(scrollArea, 1);

	/* Copy/Paste/Reset + Button box */
	auto *bottomLayout = new QHBoxLayout;
	auto *btnCopy = new QPushButton(amv::text("AMVPlugin.Common.Copy"), this);
	auto *btnPaste = new QPushButton(amv::text("AMVPlugin.Common.Paste"), this);
	auto *btnReset = new QPushButton(amv::text("AMVPlugin.Common.Reset"), this);
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

static QFormLayout *add_subzone(QVBoxLayout *layout, QWidget *parent, const QString &title)
{
	auto *label = new QLabel(title, parent);
	QFont font = label->font();
	font.setBold(true);
	label->setFont(font);
	label->setStyleSheet(QStringLiteral("QLabel { margin-top: 8px; }"));
	layout->addWidget(label);

	auto *form = new QFormLayout();
	form->setContentsMargins(8, 0, 0, 0);
	layout->addLayout(form);
	return form;
}

/* ---- Background Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_background_group()
{
	grp_background_ = new QGroupBox(amv::text("AMVPlugin.Visual.Background.Title"), this);
	auto *layout = new QVBoxLayout(grp_background_);

	/* Inheritance combo (Instance/Cell only) */
	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(amv::text("AMVPlugin.Visual.Inheritance"), grp_background_));
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

	auto *fillForm = add_subzone(layout, grp_background_, amv::text("AMVPlugin.Visual.Background.Fill"));

	chk_bg_color_enabled_ = new QCheckBox(grp_background_);
	fillForm->addRow(amv::text("AMVPlugin.Visual.Background.ColorEnabled"), chk_bg_color_enabled_);

	edit_bg_color_ = new QLineEdit(QStringLiteral("#000000"), grp_background_);
	fillForm->addRow(amv::text("AMVPlugin.Common.Color"),
			 build_color_picker(edit_bg_color_, grp_background_,
					    amv::text("AMVPlugin.Visual.Background.Color")));

	cmb_bg_fill_mode_ = new QComboBox(grp_background_);
	cmb_bg_fill_mode_->addItem(amv::text("AMVPlugin.Visual.Background.FillSignalOnly"),
				   (int)BackgroundFillMode::FillSignalOnly);
	cmb_bg_fill_mode_->addItem(amv::text("AMVPlugin.Visual.Background.FillEntireCell"),
				   (int)BackgroundFillMode::FillEntireCell);
	fillForm->addRow(amv::text("AMVPlugin.Visual.Background.FillMode"), cmb_bg_fill_mode_);

	auto *imageForm = add_subzone(layout, grp_background_, amv::text("AMVPlugin.Visual.Common.Image"));

	chk_bg_image_enabled_ = new QCheckBox(grp_background_);
	imageForm->addRow(amv::text("AMVPlugin.Visual.Common.ImageEnabled"), chk_bg_image_enabled_);

	edit_bg_image_path_ = new QLineEdit(grp_background_);
	imageForm->addRow(amv::text("AMVPlugin.Visual.Common.ImagePath"),
			  build_file_picker(edit_bg_image_path_, grp_background_,
					    amv::text("AMVPlugin.Visual.Background.SelectImage")));

	cmb_bg_image_fit_ = new QComboBox(grp_background_);
	cmb_bg_image_fit_->addItem(amv::text("AMVPlugin.Common.Fit"), (int)ImageFitMode::Fit);
	cmb_bg_image_fit_->addItem(amv::text("AMVPlugin.Common.Stretch"), (int)ImageFitMode::Stretch);
	imageForm->addRow(amv::text("AMVPlugin.Visual.Common.ImageFit"), cmb_bg_image_fit_);

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
	grp_label_ = new QGroupBox(amv::text("AMVPlugin.Visual.Label.Title"), this);
	auto *layout = new QVBoxLayout(grp_label_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(amv::text("AMVPlugin.Visual.Inheritance"), grp_label_));
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

	auto *displayForm = add_subzone(layout, grp_label_, amv::text("AMVPlugin.Visual.Label.Display"));

	cmb_label_display_ = new QComboBox(grp_label_);
	cmb_label_display_->addItem(amv::text("AMVPlugin.Common.None"), (int)LabelDisplayMode::None);
	cmb_label_display_->addItem(amv::text("AMVPlugin.Visual.Overlay.Title"), (int)LabelDisplayMode::Overlay);
	cmb_label_display_->addItem(amv::text("AMVPlugin.Visual.Label.Below"), (int)LabelDisplayMode::Below);
	displayForm->addRow(amv::text("AMVPlugin.Visual.Label.DisplayLabel"), cmb_label_display_);

	cmb_label_position_ = new QComboBox(grp_label_);
	cmb_label_position_->addItem(amv::text("AMVPlugin.Common.Top"), (int)LabelPosition::Top);
	cmb_label_position_->addItem(amv::text("AMVPlugin.Common.Bottom"), (int)LabelPosition::Bottom);
	displayForm->addRow(amv::text("AMVPlugin.Common.Position"), cmb_label_position_);

	auto *typographyForm = add_subzone(layout, grp_label_, amv::text("AMVPlugin.Visual.Label.Typography"));

	btn_label_font_ = build_font_picker(grp_label_, [this]() {
		dirty_ = true;
		emit settings_changed();
	});
	typographyForm->addRow(amv::text("AMVPlugin.Visual.Label.FontFamily"), btn_label_font_);

	spin_label_font_size_ = new QSpinBox(grp_label_);
	spin_label_font_size_->setRange(6, 96);
	typographyForm->addRow(amv::text("AMVPlugin.Visual.Label.FontSize"), spin_label_font_size_);

	cmb_label_scale_mode_ = new QComboBox(grp_label_);
	cmb_label_scale_mode_->addItem(amv::text("AMVPlugin.Visual.Label.ScaleFixed"), (int)FontScaleMode::Fixed);
	cmb_label_scale_mode_->addItem(amv::text("AMVPlugin.Visual.Label.ScaleWithCell"),
				       (int)FontScaleMode::ScaleWithCell);
	typographyForm->addRow(amv::text("AMVPlugin.Visual.Label.ScaleMode"), cmb_label_scale_mode_);

	spin_label_min_font_ = new QSpinBox(grp_label_);
	spin_label_min_font_->setRange(4, 48);
	typographyForm->addRow(amv::text("AMVPlugin.Visual.Label.MinFontSize"), spin_label_min_font_);

	spin_label_max_font_ = new QSpinBox(grp_label_);
	spin_label_max_font_->setRange(12, 144);
	typographyForm->addRow(amv::text("AMVPlugin.Visual.Label.MaxFontSize"), spin_label_max_font_);

	auto *colorsForm = add_subzone(layout, grp_label_, amv::text("AMVPlugin.Visual.Common.Colors"));

	edit_label_text_color_ = new QLineEdit(QStringLiteral("#FFFFFF"), grp_label_);
	colorsForm->addRow(amv::text("AMVPlugin.Visual.Label.TextColor"),
			   build_color_picker(edit_label_text_color_, grp_label_,
					      amv::text("AMVPlugin.Visual.Label.TextColorTitle")));

	spin_label_bg_opacity_ = new QDoubleSpinBox(grp_label_);
	spin_label_bg_opacity_->setRange(0.0, 1.0);
	spin_label_bg_opacity_->setSingleStep(0.05);
	spin_label_bg_opacity_->setDecimals(2);
	colorsForm->addRow(amv::text("AMVPlugin.Visual.Label.BackgroundOpacity"), spin_label_bg_opacity_);

	auto *boxForm = add_subzone(layout, grp_label_, amv::text("AMVPlugin.Visual.Label.Box"));

	spin_label_margin_ = new QSpinBox(grp_label_);
	spin_label_margin_->setRange(0, 32);
	boxForm->addRow(amv::text("AMVPlugin.Visual.Label.Margin"), spin_label_margin_);

	chk_bg_label_fill_ = new QCheckBox(grp_label_);
	boxForm->addRow(amv::text("AMVPlugin.Visual.Label.RegionFill"), chk_bg_label_fill_);

	chk_label_bg_rounded_ = new QCheckBox(grp_label_);
	boxForm->addRow(amv::text("AMVPlugin.Visual.Label.BackgroundRounded"), chk_label_bg_rounded_);

	HOOK_COMBO(cmb_label_display_);
	HOOK_COMBO(cmb_label_position_);
	HOOK_SPIN(spin_label_font_size_);
	HOOK_COMBO(cmb_label_scale_mode_);
	HOOK_SPIN(spin_label_min_font_);
	HOOK_SPIN(spin_label_max_font_);
	HOOK_EDIT(edit_label_text_color_);
	HOOK_DSPIN(spin_label_bg_opacity_);
	HOOK_CHECK(chk_label_bg_rounded_);
	HOOK_SPIN(spin_label_margin_);
	HOOK_CHECK(chk_bg_label_fill_);

	connect(cmb_label_display_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { update_label_control_states(); });
	connect(cmb_label_scale_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { update_label_control_states(); });

	/* Not implemented in the renderer yet. Keep the persisted field for
	 * compatibility, but do not present it as editable behavior. */
	chk_label_bg_rounded_->setEnabled(false);
	chk_label_bg_rounded_->setToolTip(amv::text("AMVPlugin.Visual.Label.BackgroundRoundedTooltip"));
	update_label_control_states();

	return grp_label_;
}

/* ---- Safe Area Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_safe_area_group()
{
	grp_safe_area_ = new QGroupBox(amv::text("AMVPlugin.Visual.SafeArea.Title"), this);
	auto *layout = new QVBoxLayout(grp_safe_area_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(amv::text("AMVPlugin.Visual.Inheritance"), grp_safe_area_));
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

	auto *visibilityForm = add_subzone(layout, grp_safe_area_, amv::text("AMVPlugin.Visual.Common.Visibility"));

	chk_safe_area_enabled_ = new QCheckBox(grp_safe_area_);
	visibilityForm->addRow(amv::text("AMVPlugin.Common.Enabled"), chk_safe_area_enabled_);

	auto *styleForm = add_subzone(layout, grp_safe_area_, amv::text("AMVPlugin.Visual.Common.Style"));

	edit_safe_area_color_ = new QLineEdit(QStringLiteral("#D0D0D0"), grp_safe_area_);
	styleForm->addRow(amv::text("AMVPlugin.Common.Color"),
			  build_color_picker(edit_safe_area_color_, grp_safe_area_,
					     amv::text("AMVPlugin.Visual.SafeArea.Color")));

	spin_safe_area_opacity_ = new QDoubleSpinBox(grp_safe_area_);
	spin_safe_area_opacity_->setRange(0.0, 1.0);
	spin_safe_area_opacity_->setSingleStep(0.1);
	spin_safe_area_opacity_->setDecimals(2);
	styleForm->addRow(amv::text("AMVPlugin.Common.Opacity"), spin_safe_area_opacity_);

	auto *geometryForm = add_subzone(layout, grp_safe_area_, amv::text("AMVPlugin.Visual.SafeArea.Geometry"));

	cmb_safe_area_anchor_ = new QComboBox(grp_safe_area_);
	cmb_safe_area_anchor_->addItem(amv::text("AMVPlugin.Common.Cell"), (int)SafeAreaAnchorMode::Cell);
	cmb_safe_area_anchor_->addItem(amv::text("AMVPlugin.Common.Signal"), (int)SafeAreaAnchorMode::Signal);
	geometryForm->addRow(amv::text("AMVPlugin.Common.Anchor"), cmb_safe_area_anchor_);

	HOOK_CHECK(chk_safe_area_enabled_);
	HOOK_COMBO(cmb_safe_area_anchor_);
	HOOK_EDIT(edit_safe_area_color_);
	HOOK_DSPIN(spin_safe_area_opacity_);

	return grp_safe_area_;
}

/* ---- VU Meter Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_vu_meter_group()
{
	grp_vu_meter_ = new QGroupBox(amv::text("AMVPlugin.Visual.VUMeter.Title"), this);
	auto *layout = new QVBoxLayout(grp_vu_meter_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(amv::text("AMVPlugin.Visual.Inheritance"), grp_vu_meter_));
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

	auto *visibilityForm = add_subzone(layout, grp_vu_meter_, amv::text("AMVPlugin.Visual.Common.Visibility"));

	chk_vu_enabled_ = new QCheckBox(grp_vu_meter_);
	visibilityForm->addRow(amv::text("AMVPlugin.Common.Enabled"), chk_vu_enabled_);

	auto *sourceForm = add_subzone(layout, grp_vu_meter_, amv::text("AMVPlugin.Visual.VUMeter.Source"));

	/* Track selection for internal OBS cells. External-provider cells meter
	 * their private source directly; the cell-scoped dialog inserts and locks
	 * External Source only when editing an external cell. */
	cmb_vu_track_mode_ = new QComboBox(grp_vu_meter_);
	cmb_vu_track_mode_->addItem(amv::text("AMVPlugin.Visual.VUMeter.Track.AutoFollowStreaming"),
				    (int)VuMeterTrackMode::AutoFollowStreaming);
	cmb_vu_track_mode_->addItem(amv::text("AMVPlugin.Visual.VUMeter.Track.Manual"), (int)VuMeterTrackMode::Manual);
	sourceForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.TrackSource"), cmb_vu_track_mode_);

	spin_vu_manual_track_ = new QSpinBox(grp_vu_meter_);
	spin_vu_manual_track_->setRange(1, 6);
	spin_vu_manual_track_->setPrefix(amv::text("AMVPlugin.Visual.VUMeter.TrackPrefix"));
	sourceForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.ManualTrack"), spin_vu_manual_track_);

	/* Manual track spinbox is only meaningful when mode == Manual.
	 * Gray out otherwise to clarify the inactive field. */
	connect(cmb_vu_track_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int) { update_vu_meter_control_states(); });

	auto *placementForm = add_subzone(layout, grp_vu_meter_, amv::text("AMVPlugin.Visual.Common.Placement"));

	cmb_vu_position_ = new QComboBox(grp_vu_meter_);
	cmb_vu_position_->addItem(amv::text("AMVPlugin.Common.Left"), (int)VuMeterPosition::Left);
	cmb_vu_position_->addItem(amv::text("AMVPlugin.Common.Right"), (int)VuMeterPosition::Right);
	cmb_vu_position_->addItem(amv::text("AMVPlugin.Common.Top"), (int)VuMeterPosition::Top);
	cmb_vu_position_->addItem(amv::text("AMVPlugin.Common.Bottom"), (int)VuMeterPosition::Bottom);
	placementForm->addRow(amv::text("AMVPlugin.Common.Position"), cmb_vu_position_);

	cmb_vu_anchor_ = new QComboBox(grp_vu_meter_);
	cmb_vu_anchor_->addItem(amv::text("AMVPlugin.Common.Cell"), (int)VuMeterAnchorMode::Cell);
	cmb_vu_anchor_->addItem(amv::text("AMVPlugin.Common.Signal"), (int)VuMeterAnchorMode::Signal);
	placementForm->addRow(amv::text("AMVPlugin.Common.Anchor"), cmb_vu_anchor_);

	spin_vu_width_ = new QSpinBox(grp_vu_meter_);
	spin_vu_width_->setRange(1, 64);
	placementForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.Width"), spin_vu_width_);

	spin_vu_length_ratio_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_length_ratio_->setRange(0.1, 1.0);
	spin_vu_length_ratio_->setSingleStep(0.05);
	spin_vu_length_ratio_->setDecimals(2);
	placementForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.LengthRatio"), spin_vu_length_ratio_);

	cmb_vu_alignment_ = new QComboBox(grp_vu_meter_);
	cmb_vu_alignment_->addItem(amv::text("AMVPlugin.Visual.VUMeter.Alignment.Start"), (int)VuMeterAlignment::Start);
	cmb_vu_alignment_->addItem(amv::text("AMVPlugin.Common.Center"), (int)VuMeterAlignment::Center);
	placementForm->addRow(amv::text("AMVPlugin.Common.Alignment"), cmb_vu_alignment_);

	chk_vu_flip_ = new QCheckBox(grp_vu_meter_);
	placementForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.Flip"), chk_vu_flip_);

	auto *lookForm = add_subzone(layout, grp_vu_meter_, amv::text("AMVPlugin.Visual.VUMeter.Look"));

	spin_vu_opacity_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_opacity_->setRange(0.0, 1.0);
	spin_vu_opacity_->setSingleStep(0.05);
	spin_vu_opacity_->setDecimals(2);
	lookForm->addRow(amv::text("AMVPlugin.Common.Opacity"), spin_vu_opacity_);

	chk_vu_multi_channel_ = new QCheckBox(grp_vu_meter_);
	chk_vu_multi_channel_->setToolTip(amv::text("AMVPlugin.Visual.VUMeter.MultiChannelTooltip"));
	lookForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.MultiChannel"), chk_vu_multi_channel_);

	auto *levelsForm = add_subzone(layout, grp_vu_meter_, amv::text("AMVPlugin.Visual.VUMeter.Levels"));

	spin_vu_warning_db_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_warning_db_->setRange(-60.0, 0.0);
	spin_vu_warning_db_->setSingleStep(1.0);
	spin_vu_warning_db_->setDecimals(1);
	spin_vu_warning_db_->setSuffix(QStringLiteral(" dB"));
	levelsForm->addRow(amv::text("AMVPlugin.Common.Warning"), spin_vu_warning_db_);

	spin_vu_error_db_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_error_db_->setRange(-60.0, 0.0);
	spin_vu_error_db_->setSingleStep(1.0);
	spin_vu_error_db_->setDecimals(1);
	spin_vu_error_db_->setSuffix(QStringLiteral(" dB"));
	levelsForm->addRow(amv::text("AMVPlugin.Common.Error"), spin_vu_error_db_);

	cmb_vu_decay_ = new QComboBox(grp_vu_meter_);
	cmb_vu_decay_->addItem(amv::text("AMVPlugin.Visual.VUMeter.Decay.Fast"), (int)VuMeterDecayRate::Fast);
	cmb_vu_decay_->addItem(amv::text("AMVPlugin.Visual.VUMeter.Decay.Medium"), (int)VuMeterDecayRate::Medium);
	cmb_vu_decay_->addItem(amv::text("AMVPlugin.Visual.VUMeter.Decay.Slow"), (int)VuMeterDecayRate::Slow);
	levelsForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.DecayRate"), cmb_vu_decay_);

	/* ---- Peak Hold ---- */
	auto *peakHoldForm = add_subzone(layout, grp_vu_meter_, amv::text("AMVPlugin.Visual.VUMeter.PeakHold"));

	chk_vu_peak_hold_ = new QCheckBox(grp_vu_meter_);
	peakHoldForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.PeakHold"), chk_vu_peak_hold_);

	spin_vu_peak_hold_ms_ = new QSpinBox(grp_vu_meter_);
	spin_vu_peak_hold_ms_->setRange(100, 5000);
	spin_vu_peak_hold_ms_->setSingleStep(100);
	spin_vu_peak_hold_ms_->setSuffix(QStringLiteral(" ms"));
	peakHoldForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.HoldTime"), spin_vu_peak_hold_ms_);

	spin_vu_peak_hold_decay_ = new QDoubleSpinBox(grp_vu_meter_);
	spin_vu_peak_hold_decay_->setRange(1.0, 60.0);
	spin_vu_peak_hold_decay_->setSingleStep(1.0);
	spin_vu_peak_hold_decay_->setDecimals(2);
	spin_vu_peak_hold_decay_->setSuffix(QStringLiteral(" dB/s"));
	peakHoldForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.HoldDecay"), spin_vu_peak_hold_decay_);

	spin_vu_peak_hold_width_ = new QSpinBox(grp_vu_meter_);
	spin_vu_peak_hold_width_->setRange(1, 4);
	peakHoldForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.HoldWidth"), spin_vu_peak_hold_width_);

	/* ---- dB Scale ---- */
	auto *scaleForm = add_subzone(layout, grp_vu_meter_, amv::text("AMVPlugin.Visual.VUMeter.Scale"));

	chk_vu_scale_ = new QCheckBox(grp_vu_meter_);
	scaleForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.ShowScale"), chk_vu_scale_);

	edit_vu_scale_ticks_ = new QLineEdit(grp_vu_meter_);
	edit_vu_scale_ticks_->setPlaceholderText(QStringLiteral("-60,-40,-20,-9,0"));
	scaleForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.ScaleTicks"), edit_vu_scale_ticks_);

	chk_vu_scale_labels_ = new QCheckBox(grp_vu_meter_);
	scaleForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.ShowLabels"), chk_vu_scale_labels_);

	cmb_vu_scale_side_ = new QComboBox(grp_vu_meter_);
	cmb_vu_scale_side_->addItem(amv::text("AMVPlugin.Common.Auto"), (int)VuMeterScaleSide::Auto);
	cmb_vu_scale_side_->addItem(amv::text("AMVPlugin.Visual.VUMeter.ScaleSide.Same"), (int)VuMeterScaleSide::Same);
	cmb_vu_scale_side_->addItem(amv::text("AMVPlugin.Visual.VUMeter.ScaleSide.Opposite"),
				    (int)VuMeterScaleSide::Opposite);
	scaleForm->addRow(amv::text("AMVPlugin.Visual.VUMeter.ScaleSide"), cmb_vu_scale_side_);

	HOOK_CHECK(chk_vu_enabled_);
	HOOK_COMBO(cmb_vu_track_mode_);
	HOOK_SPIN(spin_vu_manual_track_);
	HOOK_COMBO(cmb_vu_position_);
	HOOK_COMBO(cmb_vu_anchor_);
	HOOK_SPIN(spin_vu_width_);
	HOOK_DSPIN(spin_vu_opacity_);
	HOOK_DSPIN(spin_vu_length_ratio_);
	HOOK_COMBO(cmb_vu_alignment_);
	HOOK_CHECK(chk_vu_multi_channel_);
	HOOK_DSPIN(spin_vu_warning_db_);
	HOOK_DSPIN(spin_vu_error_db_);
	HOOK_COMBO(cmb_vu_decay_);
	HOOK_CHECK(chk_vu_flip_);
	HOOK_CHECK(chk_vu_peak_hold_);
	HOOK_SPIN(spin_vu_peak_hold_ms_);
	HOOK_DSPIN(spin_vu_peak_hold_decay_);
	HOOK_SPIN(spin_vu_peak_hold_width_);
	HOOK_CHECK(chk_vu_scale_);
	HOOK_EDIT(edit_vu_scale_ticks_);
	HOOK_CHECK(chk_vu_scale_labels_);
	HOOK_COMBO(cmb_vu_scale_side_);

	return grp_vu_meter_;
}

/* ---- Overlay Group ---- */

QGroupBox *CellDisplaySettingsDialog::create_overlay_group()
{
	grp_overlay_ = new QGroupBox(amv::text("AMVPlugin.Visual.Overlay.Title"), this);
	auto *layout = new QVBoxLayout(grp_overlay_);

	if (mode_ != Mode::Global) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(amv::text("AMVPlugin.Visual.Inheritance"), grp_overlay_));
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

	auto *visibilityForm = add_subzone(layout, grp_overlay_, amv::text("AMVPlugin.Visual.Common.Visibility"));

	chk_overlay_enabled_ = new QCheckBox(grp_overlay_);
	visibilityForm->addRow(amv::text("AMVPlugin.Common.Enabled"), chk_overlay_enabled_);

	auto *imageForm = add_subzone(layout, grp_overlay_, amv::text("AMVPlugin.Visual.Common.Image"));

	edit_overlay_path_ = new QLineEdit(grp_overlay_);
	imageForm->addRow(amv::text("AMVPlugin.Visual.Common.ImagePath"),
			  build_file_picker(edit_overlay_path_, grp_overlay_,
					    amv::text("AMVPlugin.Visual.Overlay.SelectImage")));

	auto *placementForm = add_subzone(layout, grp_overlay_, amv::text("AMVPlugin.Visual.Common.Placement"));

	cmb_overlay_fit_ = new QComboBox(grp_overlay_);
	cmb_overlay_fit_->addItem(amv::text("AMVPlugin.Common.Fit"), (int)OverlayFitMode::Fit);
	cmb_overlay_fit_->addItem(amv::text("AMVPlugin.Common.Stretch"), (int)OverlayFitMode::Stretch);
	placementForm->addRow(amv::text("AMVPlugin.Visual.Common.FitMode"), cmb_overlay_fit_);

	cmb_overlay_anchor_ = new QComboBox(grp_overlay_);
	cmb_overlay_anchor_->addItem(amv::text("AMVPlugin.Common.Cell"), (int)OverlayAnchorMode::Cell);
	cmb_overlay_anchor_->addItem(amv::text("AMVPlugin.Common.Signal"), (int)OverlayAnchorMode::Signal);
	placementForm->addRow(amv::text("AMVPlugin.Common.Anchor"), cmb_overlay_anchor_);

	auto *styleForm = add_subzone(layout, grp_overlay_, amv::text("AMVPlugin.Visual.Common.Style"));

	spin_overlay_opacity_ = new QDoubleSpinBox(grp_overlay_);
	spin_overlay_opacity_->setRange(0.0, 1.0);
	spin_overlay_opacity_->setSingleStep(0.05);
	spin_overlay_opacity_->setDecimals(2);
	styleForm->addRow(amv::text("AMVPlugin.Common.Opacity"), spin_overlay_opacity_);

	HOOK_CHECK(chk_overlay_enabled_);
	HOOK_EDIT(edit_overlay_path_);
	HOOK_DSPIN(spin_overlay_opacity_);
	HOOK_COMBO(cmb_overlay_fit_);
	HOOK_COMBO(cmb_overlay_anchor_);

	return grp_overlay_;
}

/* ---- Highlight Group (PGM / PRVW cell borders) ----
 *
 * UI mirrors the other 5 groups: optional inheritance combo for non-Global
 * scopes, followed by per-property editors in a form layout. The Cell scope
 * is treated specially: instead of an inheritance combo it shows a static
 * notice ("Highlight is instance-level") and disables every editor, because
 * highlight is intrinsically a window-wide concept driven by the OBS scene
 * tree — per-cell override has no useful semantics. */
QGroupBox *CellDisplaySettingsDialog::create_highlight_group()
{
	grp_highlight_ = new QGroupBox(amv::text("AMVPlugin.Visual.Highlight.Title"), this);
	auto *layout = new QVBoxLayout(grp_highlight_);

	if (mode_ == Mode::Cell) {
		/* Cell scope: replace inheritance combo with informational label.
		 * Wording is deliberate — "instance-level" tells the user where
		 * to go to actually edit it (Instance Settings dialog), not just
		 * that it's disabled here. */
		lbl_highlight_cell_note_ = new QLabel(amv::text("AMVPlugin.Visual.Highlight.CellNote"), grp_highlight_);
		lbl_highlight_cell_note_->setStyleSheet(QStringLiteral("QLabel { color: #888; font-style: italic; }"));
		layout->addWidget(lbl_highlight_cell_note_);
	} else if (mode_ == Mode::Instance) {
		auto *inh_row = new QHBoxLayout();
		inh_row->addWidget(new QLabel(amv::text("AMVPlugin.Visual.Inheritance"), grp_highlight_));
		cmb_highlight_inherit_ = create_inherit_combo(grp_highlight_);
		inh_row->addWidget(cmb_highlight_inherit_);
		inh_row->addStretch();
		layout->addLayout(inh_row);
		connect(cmb_highlight_inherit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
			update_inheritance_visibility();
			dirty_ = true;
			emit settings_changed();
		});
	}

	auto *visibilityForm = add_subzone(layout, grp_highlight_, amv::text("AMVPlugin.Visual.Common.Visibility"));

	chk_highlight_enabled_ = new QCheckBox(grp_highlight_);
	visibilityForm->addRow(amv::text("AMVPlugin.Common.Enabled"), chk_highlight_enabled_);

	auto *colorsForm = add_subzone(layout, grp_highlight_, amv::text("AMVPlugin.Visual.Common.Colors"));

	edit_highlight_pgm_color_ = new QLineEdit(QStringLiteral("#D00000"), grp_highlight_);
	colorsForm->addRow(amv::text("AMVPlugin.Visual.Highlight.PGMColor"),
			   build_color_picker(edit_highlight_pgm_color_, grp_highlight_,
					      amv::text("AMVPlugin.Visual.Highlight.PGMBorderColor")));

	edit_highlight_prvw_color_ = new QLineEdit(QStringLiteral("#00D000"), grp_highlight_);
	colorsForm->addRow(amv::text("AMVPlugin.Visual.Highlight.PRVWColor"),
			   build_color_picker(edit_highlight_prvw_color_, grp_highlight_,
					      amv::text("AMVPlugin.Visual.Highlight.PRVWBorderColor")));

	auto *nestedStyleForm =
		add_subzone(layout, grp_highlight_, amv::text("AMVPlugin.Visual.Highlight.NestedSceneStyle"));

	chk_highlight_nested_dashed_ = new QCheckBox(grp_highlight_);
	nestedStyleForm->addRow(amv::text("AMVPlugin.Visual.Highlight.NestedDashed"), chk_highlight_nested_dashed_);

	spin_highlight_dash_length_ = new QSpinBox(grp_highlight_);
	spin_highlight_dash_length_->setRange(4, 32);
	spin_highlight_dash_length_->setSuffix(QStringLiteral(" px"));
	nestedStyleForm->addRow(amv::text("AMVPlugin.Visual.Highlight.DashLength"), spin_highlight_dash_length_);

	spin_highlight_dash_gap_ = new QSpinBox(grp_highlight_);
	spin_highlight_dash_gap_->setRange(2, 16);
	spin_highlight_dash_gap_->setSuffix(QStringLiteral(" px"));
	nestedStyleForm->addRow(amv::text("AMVPlugin.Visual.Highlight.DashGap"), spin_highlight_dash_gap_);

	auto *borderForm = add_subzone(layout, grp_highlight_, amv::text("AMVPlugin.Visual.Highlight.Border"));

	spin_highlight_min_thickness_ = new QSpinBox(grp_highlight_);
	spin_highlight_min_thickness_->setRange(2, 16);
	spin_highlight_min_thickness_->setSuffix(QStringLiteral(" px"));
	spin_highlight_min_thickness_->setToolTip(amv::text("AMVPlugin.Visual.Highlight.MinThicknessTooltip"));
	borderForm->addRow(amv::text("AMVPlugin.Visual.Highlight.MinThickness"), spin_highlight_min_thickness_);

	HOOK_CHECK(chk_highlight_enabled_);
	HOOK_EDIT(edit_highlight_pgm_color_);
	HOOK_EDIT(edit_highlight_prvw_color_);
	HOOK_CHECK(chk_highlight_nested_dashed_);
	HOOK_SPIN(spin_highlight_dash_length_);
	HOOK_SPIN(spin_highlight_dash_gap_);
	HOOK_SPIN(spin_highlight_min_thickness_);

	return grp_highlight_;
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
	toggle_group(grp_highlight_, cmb_highlight_inherit_);

	/* Cell scope: highlight has no per-cell override at all, so force-disable
	 * the entire group regardless of any inheritance combo state. The note
	 * label set in create_highlight_group() already explains why. */
	if (mode_ == Mode::Cell && grp_highlight_) {
		for (auto *w : grp_highlight_->findChildren<QWidget *>())
			w->setEnabled(false);
		if (lbl_highlight_cell_note_)
			lbl_highlight_cell_note_->setEnabled(true); /* keep note readable */
	}

	/* Cross-control rule: manual track spinbox is only meaningful when
	 * trackMode == Manual. toggle_group above unconditionally enables all
	 * children of the override group, so re-apply this narrower rule. */
	update_vu_meter_control_states();
	update_label_control_states();
}

void CellDisplaySettingsDialog::update_vu_meter_control_states()
{
	if (!cmb_vu_track_mode_ || !spin_vu_manual_track_)
		return;

	if (mode_ == Mode::Cell) {
		if (is_external_cell_) {
			int externalIdx = cmb_vu_track_mode_->findData((int)VuMeterTrackMode::ExternalSource);
			if (externalIdx >= 0)
				cmb_vu_track_mode_->setCurrentIndex(externalIdx);
		}
		cmb_vu_track_mode_->setEnabled(false);
		spin_vu_manual_track_->setEnabled(false);
		return;
	}

	cmb_vu_track_mode_->setEnabled(true);
	const bool manual = cmb_vu_track_mode_->currentData().toInt() == (int)VuMeterTrackMode::Manual;
	spin_vu_manual_track_->setEnabled(manual);
}

void CellDisplaySettingsDialog::update_label_control_states()
{
	if (!cmb_label_display_ || !cmb_label_position_ || !cmb_label_scale_mode_ || !spin_label_font_size_ ||
	    !spin_label_min_font_ || !spin_label_max_font_ || !chk_bg_label_fill_)
		return;

	const auto display = (LabelDisplayMode)cmb_label_display_->currentData().toInt();
	const auto scaleMode = (FontScaleMode)cmb_label_scale_mode_->currentData().toInt();
	const bool labelVisible = display != LabelDisplayMode::None;
	const bool overlayMode = display == LabelDisplayMode::Overlay;
	const bool belowMode = display == LabelDisplayMode::Below;
	const bool fixedMode = scaleMode == FontScaleMode::Fixed;
	const bool scaleWithCell = scaleMode == FontScaleMode::ScaleWithCell;

	cmb_label_position_->setEnabled(labelVisible && overlayMode);
	spin_label_font_size_->setEnabled(labelVisible && fixedMode);
	spin_label_min_font_->setEnabled(labelVisible && scaleWithCell);
	spin_label_max_font_->setEnabled(labelVisible && scaleWithCell);
	chk_bg_label_fill_->setEnabled(labelVisible && belowMode);

	if (chk_label_bg_rounded_)
		chk_label_bg_rounded_->setEnabled(false);
}

/* ---- Get/Set: Global ---- */

void CellDisplaySettingsDialog::set_global_settings(const GlobalVisualSettings &gs)
{
	/* Background */
	chk_bg_color_enabled_->setChecked(gs.background.colorEnabled);
	edit_bg_color_->setText(color_to_hex(gs.background.color));
	cmb_bg_fill_mode_->setCurrentIndex(gs.background.fillMode == BackgroundFillMode::FillEntireCell ? 1 : 0);
	chk_bg_label_fill_->setChecked(gs.label.labelRegionFill);
	chk_bg_image_enabled_->setChecked(gs.background.imageEnabled);
	edit_bg_image_path_->setText(QString::fromStdString(gs.background.imagePath));
	cmb_bg_image_fit_->setCurrentIndex(gs.background.imageFitMode == ImageFitMode::Stretch ? 1 : 0);

	/* Label */
	cmb_label_display_->setCurrentIndex((int)gs.label.displayMode);
	cmb_label_position_->setCurrentIndex((int)gs.label.position);
	spin_label_font_size_->setValue(gs.label.fontSize);
	font_picker_set(btn_label_font_, QString::fromStdString(gs.label.fontFamily));
	cmb_label_scale_mode_->setCurrentIndex((int)gs.label.fontScaleMode);
	spin_label_min_font_->setValue(gs.label.minFontSize);
	spin_label_max_font_->setValue(gs.label.maxFontSize);
	edit_label_text_color_->setText(color_to_hex(gs.label.textColor));
	spin_label_bg_opacity_->setValue(gs.label.backgroundOpacity);
	chk_label_bg_rounded_->setChecked(gs.label.backgroundRounded);
	spin_label_margin_->setValue(gs.label.margin);
	update_label_control_states();

	/* Safe Area */
	chk_safe_area_enabled_->setChecked(gs.safeArea.enabled);
	cmb_safe_area_anchor_->setCurrentIndex(cmb_safe_area_anchor_->findData((int)gs.safeArea.anchorMode));
	edit_safe_area_color_->setText(color_to_hex(gs.safeArea.color));
	spin_safe_area_opacity_->setValue(gs.safeArea.opacity);

	/* VU Meter */
	chk_vu_enabled_->setChecked(gs.vuMeter.enabled);
	{
		int idx = cmb_vu_track_mode_->findData((int)gs.vuMeter.trackMode);
		if (idx < 0)
			idx = cmb_vu_track_mode_->findData((int)VuMeterTrackMode::AutoFollowStreaming);
		if (idx >= 0)
			cmb_vu_track_mode_->setCurrentIndex(idx);
	}
	spin_vu_manual_track_->setValue(gs.vuMeter.manualTrackIndex);
	update_vu_meter_control_states();
	cmb_vu_position_->setCurrentIndex(cmb_vu_position_->findData((int)gs.vuMeter.position));
	cmb_vu_anchor_->setCurrentIndex((int)gs.vuMeter.anchor);
	spin_vu_width_->setValue(gs.vuMeter.width);
	spin_vu_opacity_->setValue(gs.vuMeter.opacity);
	spin_vu_length_ratio_->setValue(gs.vuMeter.lengthRatio);
	cmb_vu_alignment_->setCurrentIndex((int)gs.vuMeter.alignment);
	chk_vu_multi_channel_->setChecked(gs.vuMeter.multiChannelEnabled);
	spin_vu_warning_db_->setValue(gs.vuMeter.warningDB);
	spin_vu_error_db_->setValue(gs.vuMeter.errorDB);
	cmb_vu_decay_->setCurrentIndex((int)gs.vuMeter.decayRate);
	chk_vu_flip_->setChecked(gs.vuMeter.flip);
	chk_vu_peak_hold_->setChecked(gs.vuMeter.peakHoldEnabled);
	spin_vu_peak_hold_ms_->setValue(gs.vuMeter.peakHoldMs);
	spin_vu_peak_hold_decay_->setValue(gs.vuMeter.peakHoldDecayDbPerSec);
	spin_vu_peak_hold_width_->setValue(gs.vuMeter.peakHoldWidthPx);
	chk_vu_scale_->setChecked(gs.vuMeter.scaleEnabled);
	edit_vu_scale_ticks_->setText(QString::fromStdString(gs.vuMeter.scaleTicks));
	chk_vu_scale_labels_->setChecked(gs.vuMeter.scaleShowLabels);
	cmb_vu_scale_side_->setCurrentIndex((int)gs.vuMeter.scaleSide);

	/* Overlay */
	chk_overlay_enabled_->setChecked(gs.overlay.enabled);
	edit_overlay_path_->setText(QString::fromStdString(gs.overlay.imagePath));
	spin_overlay_opacity_->setValue(gs.overlay.opacity);
	cmb_overlay_fit_->setCurrentIndex((int)gs.overlay.fitMode);
	cmb_overlay_anchor_->setCurrentIndex((int)gs.overlay.anchorMode);

	/* Highlight */
	chk_highlight_enabled_->setChecked(gs.highlight.enabled);
	edit_highlight_pgm_color_->setText(color_to_hex(gs.highlight.pgmColor));
	edit_highlight_prvw_color_->setText(color_to_hex(gs.highlight.prvwColor));
	chk_highlight_nested_dashed_->setChecked(gs.highlight.nestedDashed);
	spin_highlight_dash_length_->setValue(gs.highlight.dashLengthPx);
	spin_highlight_dash_gap_->setValue(gs.highlight.dashGapPx);
	spin_highlight_min_thickness_->setValue(gs.highlight.minThicknessPx);

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
	gs.label.labelRegionFill = chk_bg_label_fill_->isChecked();
	gs.background.imageEnabled = chk_bg_image_enabled_->isChecked();
	gs.background.imagePath = edit_bg_image_path_->text().toStdString();
	gs.background.imageFitMode = cmb_bg_image_fit_->currentIndex() == 1 ? ImageFitMode::Stretch : ImageFitMode::Fit;

	/* Label */
	gs.label.displayMode = (LabelDisplayMode)cmb_label_display_->currentIndex();
	gs.label.position = (LabelPosition)cmb_label_position_->currentIndex();
	gs.label.fontSize = spin_label_font_size_->value();
	gs.label.fontFamily = font_picker_get(btn_label_font_).toStdString();
	gs.label.fontScaleMode = (FontScaleMode)cmb_label_scale_mode_->currentIndex();
	gs.label.minFontSize = spin_label_min_font_->value();
	gs.label.maxFontSize = spin_label_max_font_->value();
	gs.label.textColor = hex_to_color(edit_label_text_color_->text(), 0xFFFFFFFF);
	gs.label.backgroundOpacity = spin_label_bg_opacity_->value();
	gs.label.backgroundRounded = chk_label_bg_rounded_->isChecked();
	gs.label.margin = spin_label_margin_->value();

	/* Safe Area */
	gs.safeArea.enabled = chk_safe_area_enabled_->isChecked();
	gs.safeArea.anchorMode = (SafeAreaAnchorMode)cmb_safe_area_anchor_->currentData().toInt();
	gs.safeArea.color = hex_to_color(edit_safe_area_color_->text(), 0xFFD0D0D0);
	gs.safeArea.opacity = spin_safe_area_opacity_->value();

	/* VU Meter */
	gs.vuMeter.enabled = chk_vu_enabled_->isChecked();
	gs.vuMeter.trackMode = (VuMeterTrackMode)cmb_vu_track_mode_->currentData().toInt();
	gs.vuMeter.manualTrackIndex = spin_vu_manual_track_->value();
	gs.vuMeter.position = (VuMeterPosition)cmb_vu_position_->currentData().toInt();
	gs.vuMeter.anchor = (VuMeterAnchorMode)cmb_vu_anchor_->currentIndex();
	gs.vuMeter.width = spin_vu_width_->value();
	gs.vuMeter.opacity = spin_vu_opacity_->value();
	gs.vuMeter.lengthRatio = spin_vu_length_ratio_->value();
	gs.vuMeter.alignment = (VuMeterAlignment)cmb_vu_alignment_->currentIndex();
	gs.vuMeter.multiChannelEnabled = chk_vu_multi_channel_->isChecked();
	gs.vuMeter.warningDB = spin_vu_warning_db_->value();
	gs.vuMeter.errorDB = spin_vu_error_db_->value();
	gs.vuMeter.decayRate = (VuMeterDecayRate)cmb_vu_decay_->currentIndex();
	gs.vuMeter.flip = chk_vu_flip_->isChecked();
	gs.vuMeter.peakHoldEnabled = chk_vu_peak_hold_->isChecked();
	gs.vuMeter.peakHoldMs = spin_vu_peak_hold_ms_->value();
	gs.vuMeter.peakHoldDecayDbPerSec = spin_vu_peak_hold_decay_->value();
	gs.vuMeter.peakHoldWidthPx = spin_vu_peak_hold_width_->value();
	gs.vuMeter.scaleEnabled = chk_vu_scale_->isChecked();
	gs.vuMeter.scaleTicks = edit_vu_scale_ticks_->text().toStdString();
	gs.vuMeter.scaleShowLabels = chk_vu_scale_labels_->isChecked();
	gs.vuMeter.scaleSide = (VuMeterScaleSide)cmb_vu_scale_side_->currentIndex();

	/* Overlay */
	gs.overlay.enabled = chk_overlay_enabled_->isChecked();
	gs.overlay.imagePath = edit_overlay_path_->text().toStdString();
	gs.overlay.opacity = spin_overlay_opacity_->value();
	gs.overlay.fitMode = (OverlayFitMode)cmb_overlay_fit_->currentIndex();
	gs.overlay.anchorMode = (OverlayAnchorMode)cmb_overlay_anchor_->currentIndex();

	/* Highlight (window-wide; Global owns the canonical defaults) */
	gs.highlight.enabled = chk_highlight_enabled_->isChecked();
	gs.highlight.pgmColor = hex_to_color(edit_highlight_pgm_color_->text(), 0xFFD00000);
	gs.highlight.prvwColor = hex_to_color(edit_highlight_prvw_color_->text(), 0xFF00D000);
	gs.highlight.nestedDashed = chk_highlight_nested_dashed_->isChecked();
	gs.highlight.dashLengthPx = spin_highlight_dash_length_->value();
	gs.highlight.dashGapPx = spin_highlight_dash_gap_->value();
	gs.highlight.minThicknessPx = spin_highlight_min_thickness_->value();

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
	chk_bg_label_fill_->setChecked(is.label.labelRegionFill);
	chk_bg_image_enabled_->setChecked(is.background.imageEnabled);
	edit_bg_image_path_->setText(QString::fromStdString(is.background.imagePath));
	cmb_bg_image_fit_->setCurrentIndex(is.background.imageFitMode == ImageFitMode::Stretch ? 1 : 0);

	cmb_label_display_->setCurrentIndex((int)is.label.displayMode);
	cmb_label_position_->setCurrentIndex((int)is.label.position);
	spin_label_font_size_->setValue(is.label.fontSize);
	font_picker_set(btn_label_font_, QString::fromStdString(is.label.fontFamily));
	cmb_label_scale_mode_->setCurrentIndex((int)is.label.fontScaleMode);
	spin_label_min_font_->setValue(is.label.minFontSize);
	spin_label_max_font_->setValue(is.label.maxFontSize);
	edit_label_text_color_->setText(color_to_hex(is.label.textColor));
	spin_label_bg_opacity_->setValue(is.label.backgroundOpacity);
	chk_label_bg_rounded_->setChecked(is.label.backgroundRounded);
	spin_label_margin_->setValue(is.label.margin);
	update_label_control_states();

	chk_safe_area_enabled_->setChecked(is.safeArea.enabled);
	cmb_safe_area_anchor_->setCurrentIndex(cmb_safe_area_anchor_->findData((int)is.safeArea.anchorMode));
	edit_safe_area_color_->setText(color_to_hex(is.safeArea.color));
	spin_safe_area_opacity_->setValue(is.safeArea.opacity);

	chk_vu_enabled_->setChecked(is.vuMeter.enabled);
	{
		int idx = cmb_vu_track_mode_->findData((int)is.vuMeter.trackMode);
		if (idx < 0)
			idx = cmb_vu_track_mode_->findData((int)VuMeterTrackMode::AutoFollowStreaming);
		if (idx >= 0)
			cmb_vu_track_mode_->setCurrentIndex(idx);
	}
	spin_vu_manual_track_->setValue(is.vuMeter.manualTrackIndex);
	update_vu_meter_control_states();
	cmb_vu_position_->setCurrentIndex(cmb_vu_position_->findData((int)is.vuMeter.position));
	cmb_vu_anchor_->setCurrentIndex((int)is.vuMeter.anchor);
	spin_vu_width_->setValue(is.vuMeter.width);
	spin_vu_opacity_->setValue(is.vuMeter.opacity);
	spin_vu_length_ratio_->setValue(is.vuMeter.lengthRatio);
	cmb_vu_alignment_->setCurrentIndex((int)is.vuMeter.alignment);
	chk_vu_multi_channel_->setChecked(is.vuMeter.multiChannelEnabled);
	spin_vu_warning_db_->setValue(is.vuMeter.warningDB);
	spin_vu_error_db_->setValue(is.vuMeter.errorDB);
	cmb_vu_decay_->setCurrentIndex((int)is.vuMeter.decayRate);
	chk_vu_flip_->setChecked(is.vuMeter.flip);
	chk_vu_peak_hold_->setChecked(is.vuMeter.peakHoldEnabled);
	spin_vu_peak_hold_ms_->setValue(is.vuMeter.peakHoldMs);
	spin_vu_peak_hold_decay_->setValue(is.vuMeter.peakHoldDecayDbPerSec);
	spin_vu_peak_hold_width_->setValue(is.vuMeter.peakHoldWidthPx);
	chk_vu_scale_->setChecked(is.vuMeter.scaleEnabled);
	edit_vu_scale_ticks_->setText(QString::fromStdString(is.vuMeter.scaleTicks));
	chk_vu_scale_labels_->setChecked(is.vuMeter.scaleShowLabels);
	cmb_vu_scale_side_->setCurrentIndex((int)is.vuMeter.scaleSide);

	chk_overlay_enabled_->setChecked(is.overlay.enabled);
	edit_overlay_path_->setText(QString::fromStdString(is.overlay.imagePath));
	spin_overlay_opacity_->setValue(is.overlay.opacity);
	cmb_overlay_fit_->setCurrentIndex((int)is.overlay.fitMode);
	cmb_overlay_anchor_->setCurrentIndex((int)is.overlay.anchorMode);

	/* Highlight */
	if (cmb_highlight_inherit_)
		set_inherit_combo(cmb_highlight_inherit_, is.highlightMode);
	chk_highlight_enabled_->setChecked(is.highlight.enabled);
	edit_highlight_pgm_color_->setText(color_to_hex(is.highlight.pgmColor));
	edit_highlight_prvw_color_->setText(color_to_hex(is.highlight.prvwColor));
	chk_highlight_nested_dashed_->setChecked(is.highlight.nestedDashed);
	spin_highlight_dash_length_->setValue(is.highlight.dashLengthPx);
	spin_highlight_dash_gap_->setValue(is.highlight.dashGapPx);
	spin_highlight_min_thickness_->setValue(is.highlight.minThicknessPx);

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
	is.label.labelRegionFill = chk_bg_label_fill_->isChecked();
	is.background.imageEnabled = chk_bg_image_enabled_->isChecked();
	is.background.imagePath = edit_bg_image_path_->text().toStdString();
	is.background.imageFitMode = cmb_bg_image_fit_->currentIndex() == 1 ? ImageFitMode::Stretch : ImageFitMode::Fit;

	/* Label */
	is.label.displayMode = (LabelDisplayMode)cmb_label_display_->currentIndex();
	is.label.position = (LabelPosition)cmb_label_position_->currentIndex();
	is.label.fontSize = spin_label_font_size_->value();
	is.label.fontFamily = font_picker_get(btn_label_font_).toStdString();
	is.label.fontScaleMode = (FontScaleMode)cmb_label_scale_mode_->currentIndex();
	is.label.minFontSize = spin_label_min_font_->value();
	is.label.maxFontSize = spin_label_max_font_->value();
	is.label.textColor = hex_to_color(edit_label_text_color_->text(), 0xFFFFFFFF);
	is.label.backgroundOpacity = spin_label_bg_opacity_->value();
	is.label.backgroundRounded = chk_label_bg_rounded_->isChecked();
	is.label.margin = spin_label_margin_->value();

	/* Safe Area */
	is.safeArea.enabled = chk_safe_area_enabled_->isChecked();
	is.safeArea.anchorMode = (SafeAreaAnchorMode)cmb_safe_area_anchor_->currentData().toInt();
	is.safeArea.color = hex_to_color(edit_safe_area_color_->text(), 0xFFD0D0D0);
	is.safeArea.opacity = spin_safe_area_opacity_->value();

	/* VU Meter */
	is.vuMeter.enabled = chk_vu_enabled_->isChecked();
	is.vuMeter.trackMode = (VuMeterTrackMode)cmb_vu_track_mode_->currentData().toInt();
	is.vuMeter.manualTrackIndex = spin_vu_manual_track_->value();
	is.vuMeter.position = (VuMeterPosition)cmb_vu_position_->currentData().toInt();
	is.vuMeter.anchor = (VuMeterAnchorMode)cmb_vu_anchor_->currentIndex();
	is.vuMeter.width = spin_vu_width_->value();
	is.vuMeter.opacity = spin_vu_opacity_->value();
	is.vuMeter.lengthRatio = spin_vu_length_ratio_->value();
	is.vuMeter.alignment = (VuMeterAlignment)cmb_vu_alignment_->currentIndex();
	is.vuMeter.multiChannelEnabled = chk_vu_multi_channel_->isChecked();
	is.vuMeter.warningDB = spin_vu_warning_db_->value();
	is.vuMeter.errorDB = spin_vu_error_db_->value();
	is.vuMeter.decayRate = (VuMeterDecayRate)cmb_vu_decay_->currentIndex();
	is.vuMeter.flip = chk_vu_flip_->isChecked();
	is.vuMeter.peakHoldEnabled = chk_vu_peak_hold_->isChecked();
	is.vuMeter.peakHoldMs = spin_vu_peak_hold_ms_->value();
	is.vuMeter.peakHoldDecayDbPerSec = spin_vu_peak_hold_decay_->value();
	is.vuMeter.peakHoldWidthPx = spin_vu_peak_hold_width_->value();
	is.vuMeter.scaleEnabled = chk_vu_scale_->isChecked();
	is.vuMeter.scaleTicks = edit_vu_scale_ticks_->text().toStdString();
	is.vuMeter.scaleShowLabels = chk_vu_scale_labels_->isChecked();
	is.vuMeter.scaleSide = (VuMeterScaleSide)cmb_vu_scale_side_->currentIndex();

	/* Overlay */
	is.overlay.enabled = chk_overlay_enabled_->isChecked();
	is.overlay.imagePath = edit_overlay_path_->text().toStdString();
	is.overlay.opacity = spin_overlay_opacity_->value();
	is.overlay.fitMode = (OverlayFitMode)cmb_overlay_fit_->currentIndex();
	is.overlay.anchorMode = (OverlayAnchorMode)cmb_overlay_anchor_->currentIndex();

	/* Highlight */
	if (cmb_highlight_inherit_)
		is.highlightMode = get_inherit_combo(cmb_highlight_inherit_);
	is.highlight.enabled = chk_highlight_enabled_->isChecked();
	is.highlight.pgmColor = hex_to_color(edit_highlight_pgm_color_->text(), 0xFFD00000);
	is.highlight.prvwColor = hex_to_color(edit_highlight_prvw_color_->text(), 0xFF00D000);
	is.highlight.nestedDashed = chk_highlight_nested_dashed_->isChecked();
	is.highlight.dashLengthPx = spin_highlight_dash_length_->value();
	is.highlight.dashGapPx = spin_highlight_dash_gap_->value();
	is.highlight.minThicknessPx = spin_highlight_min_thickness_->value();

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
	chk_bg_label_fill_->setChecked(cs.label.labelRegionFill);
	chk_bg_image_enabled_->setChecked(cs.background.imageEnabled);
	edit_bg_image_path_->setText(QString::fromStdString(cs.background.imagePath));
	cmb_bg_image_fit_->setCurrentIndex(cs.background.imageFitMode == ImageFitMode::Stretch ? 1 : 0);

	cmb_label_display_->setCurrentIndex((int)cs.label.displayMode);
	cmb_label_position_->setCurrentIndex((int)cs.label.position);
	spin_label_font_size_->setValue(cs.label.fontSize);
	font_picker_set(btn_label_font_, QString::fromStdString(cs.label.fontFamily));
	cmb_label_scale_mode_->setCurrentIndex((int)cs.label.fontScaleMode);
	spin_label_min_font_->setValue(cs.label.minFontSize);
	spin_label_max_font_->setValue(cs.label.maxFontSize);
	edit_label_text_color_->setText(color_to_hex(cs.label.textColor));
	spin_label_bg_opacity_->setValue(cs.label.backgroundOpacity);
	chk_label_bg_rounded_->setChecked(cs.label.backgroundRounded);
	spin_label_margin_->setValue(cs.label.margin);
	update_label_control_states();

	chk_safe_area_enabled_->setChecked(cs.safeArea.enabled);
	cmb_safe_area_anchor_->setCurrentIndex(cmb_safe_area_anchor_->findData((int)cs.safeArea.anchorMode));
	edit_safe_area_color_->setText(color_to_hex(cs.safeArea.color));
	spin_safe_area_opacity_->setValue(cs.safeArea.opacity);

	chk_vu_enabled_->setChecked(cs.vuMeter.enabled);
	{
		int idx = cmb_vu_track_mode_->findData((int)cs.vuMeter.trackMode);
		if (idx < 0)
			idx = cmb_vu_track_mode_->findData((int)VuMeterTrackMode::AutoFollowStreaming);
		if (idx >= 0)
			cmb_vu_track_mode_->setCurrentIndex(idx);
	}
	spin_vu_manual_track_->setValue(cs.vuMeter.manualTrackIndex);
	update_vu_meter_control_states();
	cmb_vu_position_->setCurrentIndex(cmb_vu_position_->findData((int)cs.vuMeter.position));
	cmb_vu_anchor_->setCurrentIndex((int)cs.vuMeter.anchor);
	spin_vu_width_->setValue(cs.vuMeter.width);
	spin_vu_opacity_->setValue(cs.vuMeter.opacity);
	spin_vu_length_ratio_->setValue(cs.vuMeter.lengthRatio);
	cmb_vu_alignment_->setCurrentIndex((int)cs.vuMeter.alignment);
	chk_vu_multi_channel_->setChecked(cs.vuMeter.multiChannelEnabled);
	spin_vu_warning_db_->setValue(cs.vuMeter.warningDB);
	spin_vu_error_db_->setValue(cs.vuMeter.errorDB);
	cmb_vu_decay_->setCurrentIndex((int)cs.vuMeter.decayRate);
	chk_vu_flip_->setChecked(cs.vuMeter.flip);
	chk_vu_peak_hold_->setChecked(cs.vuMeter.peakHoldEnabled);
	spin_vu_peak_hold_ms_->setValue(cs.vuMeter.peakHoldMs);
	spin_vu_peak_hold_decay_->setValue(cs.vuMeter.peakHoldDecayDbPerSec);
	spin_vu_peak_hold_width_->setValue(cs.vuMeter.peakHoldWidthPx);
	chk_vu_scale_->setChecked(cs.vuMeter.scaleEnabled);
	edit_vu_scale_ticks_->setText(QString::fromStdString(cs.vuMeter.scaleTicks));
	chk_vu_scale_labels_->setChecked(cs.vuMeter.scaleShowLabels);
	cmb_vu_scale_side_->setCurrentIndex((int)cs.vuMeter.scaleSide);

	chk_overlay_enabled_->setChecked(cs.overlay.enabled);
	edit_overlay_path_->setText(QString::fromStdString(cs.overlay.imagePath));
	spin_overlay_opacity_->setValue(cs.overlay.opacity);
	cmb_overlay_fit_->setCurrentIndex((int)cs.overlay.fitMode);
	cmb_overlay_anchor_->setCurrentIndex((int)cs.overlay.anchorMode);

	/* Highlight — no per-cell value exists. Show the default struct so the
	 * disabled controls look sane rather than empty/zero. update_inheritance_
	 * visibility() unconditionally disables this group in Cell mode. */
	{
		HighlightSettings hd;
		chk_highlight_enabled_->setChecked(hd.enabled);
		edit_highlight_pgm_color_->setText(color_to_hex(hd.pgmColor));
		edit_highlight_prvw_color_->setText(color_to_hex(hd.prvwColor));
		chk_highlight_nested_dashed_->setChecked(hd.nestedDashed);
		spin_highlight_dash_length_->setValue(hd.dashLengthPx);
		spin_highlight_dash_gap_->setValue(hd.dashGapPx);
		spin_highlight_min_thickness_->setValue(hd.minThicknessPx);
	}

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
	cs.label.labelRegionFill = chk_bg_label_fill_->isChecked();
	cs.background.imageEnabled = chk_bg_image_enabled_->isChecked();
	cs.background.imagePath = edit_bg_image_path_->text().toStdString();
	cs.background.imageFitMode = cmb_bg_image_fit_->currentIndex() == 1 ? ImageFitMode::Stretch : ImageFitMode::Fit;

	/* Label */
	cs.label.displayMode = (LabelDisplayMode)cmb_label_display_->currentIndex();
	cs.label.position = (LabelPosition)cmb_label_position_->currentIndex();
	cs.label.fontSize = spin_label_font_size_->value();
	cs.label.fontFamily = font_picker_get(btn_label_font_).toStdString();
	cs.label.fontScaleMode = (FontScaleMode)cmb_label_scale_mode_->currentIndex();
	cs.label.minFontSize = spin_label_min_font_->value();
	cs.label.maxFontSize = spin_label_max_font_->value();
	cs.label.textColor = hex_to_color(edit_label_text_color_->text(), 0xFFFFFFFF);
	cs.label.backgroundOpacity = spin_label_bg_opacity_->value();
	cs.label.backgroundRounded = chk_label_bg_rounded_->isChecked();
	cs.label.margin = spin_label_margin_->value();

	/* Safe Area */
	cs.safeArea.enabled = chk_safe_area_enabled_->isChecked();
	cs.safeArea.anchorMode = (SafeAreaAnchorMode)cmb_safe_area_anchor_->currentData().toInt();
	cs.safeArea.color = hex_to_color(edit_safe_area_color_->text(), 0xFFD0D0D0);
	cs.safeArea.opacity = spin_safe_area_opacity_->value();

	/* VU Meter */
	cs.vuMeter.enabled = chk_vu_enabled_->isChecked();
	cs.vuMeter.trackMode = (VuMeterTrackMode)cmb_vu_track_mode_->currentData().toInt();
	cs.vuMeter.manualTrackIndex = spin_vu_manual_track_->value();
	cs.vuMeter.position = (VuMeterPosition)cmb_vu_position_->currentData().toInt();
	cs.vuMeter.anchor = (VuMeterAnchorMode)cmb_vu_anchor_->currentIndex();
	cs.vuMeter.width = spin_vu_width_->value();
	cs.vuMeter.opacity = spin_vu_opacity_->value();
	cs.vuMeter.lengthRatio = spin_vu_length_ratio_->value();
	cs.vuMeter.alignment = (VuMeterAlignment)cmb_vu_alignment_->currentIndex();
	cs.vuMeter.multiChannelEnabled = chk_vu_multi_channel_->isChecked();
	cs.vuMeter.warningDB = spin_vu_warning_db_->value();
	cs.vuMeter.errorDB = spin_vu_error_db_->value();
	cs.vuMeter.decayRate = (VuMeterDecayRate)cmb_vu_decay_->currentIndex();
	cs.vuMeter.flip = chk_vu_flip_->isChecked();
	cs.vuMeter.peakHoldEnabled = chk_vu_peak_hold_->isChecked();
	cs.vuMeter.peakHoldMs = spin_vu_peak_hold_ms_->value();
	cs.vuMeter.peakHoldDecayDbPerSec = spin_vu_peak_hold_decay_->value();
	cs.vuMeter.peakHoldWidthPx = spin_vu_peak_hold_width_->value();
	cs.vuMeter.scaleEnabled = chk_vu_scale_->isChecked();
	cs.vuMeter.scaleTicks = edit_vu_scale_ticks_->text().toStdString();
	cs.vuMeter.scaleShowLabels = chk_vu_scale_labels_->isChecked();
	cs.vuMeter.scaleSide = (VuMeterScaleSide)cmb_vu_scale_side_->currentIndex();

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
		if (auto *clipboard = QApplication::clipboard())
			clipboard->setText(QString::fromUtf8(s_clipboard_));
		obs_data_release(data);
	}
}

void CellDisplaySettingsDialog::on_paste()
{
	QByteArray payload = s_clipboard_;
	if (auto *clipboard = QApplication::clipboard()) {
		const QByteArray systemText = clipboard->text().toUtf8();
		if (!systemText.trimmed().isEmpty())
			payload = systemText;
	}
	if (payload.isEmpty())
		return;
	obs_data_t *data = obs_data_create_from_json(payload.constData());
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
