/*
OBS Advanced Multiview - obs-spout2 Spout provider settings form (Phase 3 / M6.3)

Split out of provider-settings-forms.cpp for maintainability. The form's
class declaration lives in provider-settings-forms.hpp; this TU only
implements its members. Discovery is provided by
signal-provider-spout.cpp through signal_provider_spout_discover_senders().

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include "provider-settings-forms.hpp"
#include "amv-i18n.hpp"
#include "provider-settings-forms-common.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QVBoxLayout>

using mv_forms::item_bare_name;
using mv_forms::make_lost_item;
using mv_forms::make_normal_item;
using mv_forms::set_or_default_int;
/* ---------- SpoutSenderForm (Phase 3 / M6.3) ---------- */

namespace {

constexpr const char *kSpoutKeySenderList = "spoutsenders";
constexpr const char *kSpoutKeyCompositeMode = "compositemode";
constexpr const char *kSpoutKeyTickSpeedLimit = "tickspeedlimit";
constexpr const char *kSpoutFirstAvailableToken = "usefirstavailablesender";

constexpr int kSpoutCompositeOpaque = 1;
constexpr int kSpoutCompositeAlpha = 2;
constexpr int kSpoutCompositeDefault = 3;
constexpr int kSpoutCompositePremultiplied = 4;

constexpr int kSpoutTickCrazy = 1;
constexpr int kSpoutTickFast = 100;
constexpr int kSpoutTickNormal = 500;
constexpr int kSpoutTickSlow = 1000;

} // namespace

SpoutSenderForm::SpoutSenderForm(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(8);

	chk_first_available_ = new QCheckBox(amv::text("AMVPlugin.Provider.Spout.FirstAvailable"), this);
	chk_first_available_->setChecked(true);
	chk_first_available_->setToolTip(amv::text("AMVPlugin.Provider.Spout.FirstAvailableTooltip"));
	root->addWidget(chk_first_available_);

	auto *list_label = new QLabel(amv::text("AMVPlugin.Provider.Spout.Discovered"), this);
	root->addWidget(list_label);

	discovered_list_ = new QListWidget(this);
	discovered_list_->setSelectionMode(QAbstractItemView::SingleSelection);
	discovered_list_->setToolTip(amv::text("AMVPlugin.Provider.Spout.DiscoveredTooltip"));
	root->addWidget(discovered_list_, 1);

	auto *refresh_row = new QHBoxLayout();
	refresh_btn_ = new QPushButton(amv::text("AMVPlugin.Common.Refresh"), this);
	refresh_btn_->setToolTip(amv::text("AMVPlugin.Provider.Spout.RefreshTooltip"));
	refresh_row->addWidget(refresh_btn_);
	refresh_row->addStretch(1);
	root->addLayout(refresh_row);

	auto *form = new QFormLayout();

	cmb_composite_mode_ = new QComboBox(this);
	cmb_composite_mode_->addItem(amv::text("AMVPlugin.Provider.Spout.Composite.Default"), kSpoutCompositeDefault);
	cmb_composite_mode_->addItem(amv::text("AMVPlugin.Provider.Spout.Composite.Opaque"), kSpoutCompositeOpaque);
	cmb_composite_mode_->addItem(amv::text("AMVPlugin.Provider.Spout.Composite.Alpha"), kSpoutCompositeAlpha);
	cmb_composite_mode_->addItem(amv::text("AMVPlugin.Provider.Spout.Composite.Premultiplied"),
				     kSpoutCompositePremultiplied);
	cmb_composite_mode_->setToolTip(amv::text("AMVPlugin.Provider.Spout.CompositeTooltip"));
	form->addRow(amv::text("AMVPlugin.Provider.Spout.CompositeMode"), cmb_composite_mode_);

	cmb_tick_speed_ = new QComboBox(this);
	cmb_tick_speed_->addItem(amv::text("AMVPlugin.Provider.Spout.Tick.Crazy"), kSpoutTickCrazy);
	cmb_tick_speed_->addItem(amv::text("AMVPlugin.Provider.Spout.Tick.Fast"), kSpoutTickFast);
	cmb_tick_speed_->addItem(amv::text("AMVPlugin.Provider.Spout.Tick.Normal"), kSpoutTickNormal);
	cmb_tick_speed_->addItem(amv::text("AMVPlugin.Provider.Spout.Tick.Slow"), kSpoutTickSlow);
	cmb_tick_speed_->setToolTip(amv::text("AMVPlugin.Provider.Spout.TickTooltip"));
	/* Default to Normal (500 ms): obs-spout2's own default is Fast
	 * (100 ms) which is wasteful for a Multiview cell where stale
	 * frames cost more than they're worth. Each multiview cell
	 * polling at 100 ms compounds quickly with many cells. */
	{
		int normal_idx = cmb_tick_speed_->findData(kSpoutTickNormal);
		if (normal_idx >= 0)
			cmb_tick_speed_->setCurrentIndex(normal_idx);
	}
	form->addRow(amv::text("AMVPlugin.Provider.Spout.TickSpeed"), cmb_tick_speed_);

	root->addLayout(form);

	connect(refresh_btn_, &QPushButton::clicked, this, &SpoutSenderForm::refresh_discovery);
	connect(chk_first_available_, &QCheckBox::toggled, this, [this](bool) { apply_first_available_visibility(); });
	connect(discovered_list_, &QListWidget::currentItemChanged, this,
		[this](QListWidgetItem *current, QListWidgetItem *) {
			if (current && (current->flags() & Qt::ItemIsSelectable))
				remembered_selection_ = item_bare_name(current);
		});

	apply_first_available_visibility();
	refresh_discovery();
}

void SpoutSenderForm::apply_first_available_visibility()
{
	const bool first_avail = chk_first_available_->isChecked();
	/* Disable the discovered list so the user can't accidentally
	 * select a specific sender while first-available is on, but keep
	 * the Refresh button enabled \u2014 the user may still want to re-scan
	 * the list to confirm which senders are currently present before
	 * deciding to switch off first-available. */
	discovered_list_->setEnabled(!first_avail);
}

void SpoutSenderForm::refresh_discovery()
{
	discovered_list_->clear();

	const auto names = signal_provider_spout_discover_senders();
	QListWidgetItem *to_select = nullptr;
	for (const auto &name : names) {
		const QString q = QString::fromStdString(name);
		auto *item = make_normal_item(q);
		discovered_list_->addItem(item);
		if (!remembered_selection_.isEmpty() && q == remembered_selection_)
			to_select = item;
	}

	/* Re-inject a previously-selected sender that has temporarily
	 * disappeared from the local Spout registry as a lost-state row
	 * so the user keeps visibility of the cell's binding. The row
	 * reverts to a normal entry on the next Refresh once the sender
	 * comes back, with selection preserved. */
	if (!remembered_selection_.isEmpty() && !to_select) {
		auto *lost = make_lost_item(remembered_selection_);
		discovered_list_->addItem(lost);
		to_select = lost;
	}

	if (to_select)
		discovered_list_->setCurrentItem(to_select);

	if (names.empty() && remembered_selection_.isEmpty()) {
		auto *item = new QListWidgetItem(amv::text("AMVPlugin.Provider.Spout.NoSenders"));
		item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
		QFont f = item->font();
		f.setItalic(true);
		item->setFont(f);
		discovered_list_->addItem(item);
	}
}

void SpoutSenderForm::load_from(const SignalConfig &cfg)
{
	obs_data_t *src = cfg.providerSettings;

	const QString persisted_sender = src ? QString::fromUtf8(obs_data_get_string(src, kSpoutKeySenderList))
					     : QString::fromUtf8(kSpoutFirstAvailableToken);
	const bool is_first_avail = persisted_sender.isEmpty() ||
				    persisted_sender == QString::fromUtf8(kSpoutFirstAvailableToken);
	chk_first_available_->setChecked(is_first_avail);

	/* Track the persisted sender across Refresh cycles so a lost
	 * sender stays visible (italic/grey + " | signal lost") and
	 * selected, then reverts to a normal entry when it returns. */
	remembered_selection_ = is_first_avail ? QString() : persisted_sender;
	refresh_discovery();

	if (src && obs_data_has_user_value(src, kSpoutKeyCompositeMode)) {
		int idx = cmb_composite_mode_->findData((int)obs_data_get_int(src, kSpoutKeyCompositeMode));
		if (idx >= 0)
			cmb_composite_mode_->setCurrentIndex(idx);
	}
	if (src && obs_data_has_user_value(src, kSpoutKeyTickSpeedLimit)) {
		int idx = cmb_tick_speed_->findData((int)obs_data_get_int(src, kSpoutKeyTickSpeedLimit));
		if (idx >= 0)
			cmb_tick_speed_->setCurrentIndex(idx);
	}

	apply_first_available_visibility();
}

bool SpoutSenderForm::is_valid() const
{
	if (chk_first_available_->isChecked())
		return true;
	auto *cur = discovered_list_->currentItem();
	return cur && (cur->flags() & Qt::ItemIsSelectable);
}

QString SpoutSenderForm::invalid_reason() const
{
	if (is_valid())
		return QString();
	return amv::text("AMVPlugin.Provider.Spout.ErrorNoSender");
}

SignalConfig SpoutSenderForm::to_signal_config() const
{
	SignalConfig cfg;
	if (!is_valid())
		return cfg;

	cfg.provider = SignalProviderType::Spout;

	QString sender_name;
	if (chk_first_available_->isChecked()) {
		sender_name = QString::fromUtf8(kSpoutFirstAvailableToken);
		cfg.displayName = amv::text("AMVPlugin.Provider.Spout.DisplayFirstAvailable").toStdString();
	} else {
		auto *cur = discovered_list_->currentItem();
		sender_name = cur ? item_bare_name(cur) : QString();
		cfg.displayName = sender_name.toStdString();
	}

	cfg.providerSettings = obs_data_create();
	obs_data_t *d = cfg.providerSettings;
	obs_data_set_string(d, kSpoutKeySenderList, sender_name.toUtf8().constData());

	set_or_default_int(d, kSpoutKeyCompositeMode, cmb_composite_mode_->currentData().toInt(),
			   kSpoutCompositeDefault);
	set_or_default_int(d, kSpoutKeyTickSpeedLimit, cmb_tick_speed_->currentData().toInt(), kSpoutTickNormal);

	return cfg;
}
