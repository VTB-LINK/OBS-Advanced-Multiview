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

#pragma once

#include <obs-module.h>
#include <plugin-support.h>

#include <QString>

#include <cstring>

namespace amv {
inline QString text_or(const char *key, const char *fallback)
{
	if (!key || !*key)
		return fallback ? QString::fromUtf8(fallback) : QString();

	const char *translated = obs_module_text(key);
	if (!translated || !*translated || std::strcmp(translated, key) == 0) {
		obs_log(LOG_WARNING, "missing locale key: %s", key);
		return QString::fromUtf8(fallback && *fallback ? fallback : key);
	}

	return QString::fromUtf8(translated);
}

inline QString text(const char *key)
{
	return text_or(key, key);
}
} // namespace amv