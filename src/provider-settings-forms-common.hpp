/*
OBS Advanced Multiview - Provider settings form internal helpers

Internal header shared by the four provider settings form impls
(provider-settings-forms-{ffmpeg,ndi,spout,vlc}.cpp). Not part of the
public API \u2014 intentionally not re-exported through
provider-settings-forms.hpp.

All helpers are inline so each translation unit gets its own copy with
no link-time dependency. Runtime state (none here) would belong in
provider-settings-forms-common.cpp instead.

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#pragma once

#include "amv-i18n.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QListWidget>
#include <QListWidgetItem>
#include <QString>
#include <QVariant>

#include <obs.h>

namespace mv_forms {

/* set_or_default_*: write to obs_data_t only when the value differs
 * from the supplied default. Matches one of the persistence rules in
 * the M6 design: persisted JSON should not bloat with default values
 * \u2014 the provider re-applies defaults on create so missing keys
 * resolve to the same runtime value. */
inline void set_or_default_int(obs_data_t *data, const char *key, int val, int def)
{
	if (val == def)
		obs_data_unset_user_value(data, key);
	else
		obs_data_set_int(data, key, (long long)val);
}

inline void set_or_default_bool(obs_data_t *data, const char *key, bool val, bool def)
{
	if (val == def)
		obs_data_unset_user_value(data, key);
	else
		obs_data_set_bool(data, key, val);
}

inline void set_or_default_string(obs_data_t *data, const char *key, const QString &val, const char *def)
{
	const std::string utf8 = val.toStdString();
	if (utf8 == (def ? def : ""))
		obs_data_unset_user_value(data, key);
	else
		obs_data_set_string(data, key, utf8.c_str());
}

/* Lost-state list-row helpers shared by NdiSourceForm and
 * SpoutSenderForm.
 *
 * Both forms render a discovered-sender list and need to keep the
 * user's previous selection visible across Refresh cycles, even when
 * the named sender is currently absent. The bare sender / source name
 * is stored in Qt::UserRole so the rendered text can carry decoration
 * like " | signal lost" without breaking the lookup. Selection restore
 * compares against UserRole; to_signal_config() also reads UserRole.
 *
 * SourcePicker / EditSource on a Refresh just builds normal items;
 * the form notices when the remembered selection is missing and
 * appends a single lost-style item. */
inline QString item_bare_name(const QListWidgetItem *item)
{
	if (!item)
		return QString();
	const QVariant v = item->data(Qt::UserRole);
	if (v.isValid() && v.canConvert<QString>())
		return v.toString();
	return item->text();
}

inline QListWidgetItem *make_normal_item(const QString &bare_name)
{
	auto *item = new QListWidgetItem(bare_name);
	item->setData(Qt::UserRole, bare_name);
	return item;
}

inline QListWidgetItem *make_lost_item(const QString &bare_name)
{
	auto *item = new QListWidgetItem(bare_name + amv::text("AMVPlugin.Provider.Common.SignalLostSuffix"));
	item->setData(Qt::UserRole, bare_name);
	QFont f = item->font();
	f.setItalic(true);
	item->setFont(f);
	/* Slightly desaturated foreground so the row visually recedes but
	 * remains readable when selected (Qt's selection highlight paints
	 * over the text color in most styles). */
	item->setForeground(QBrush(QColor(160, 160, 160)));
	return item;
}

inline bool list_contains_bare(const QListWidget *list, const QString &bare_name)
{
	for (int i = 0; i < list->count(); i++) {
		const auto *it = list->item(i);
		if (!(it->flags() & Qt::ItemIsSelectable))
			continue;
		if (item_bare_name(it) == bare_name)
			return true;
	}
	return false;
}

} // namespace mv_forms
