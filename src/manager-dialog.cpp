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

#include "manager-dialog.hpp"

#include <QLabel>
#include <QVBoxLayout>

ManagerDialog::ManagerDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("OBS Advanced Multiview"));
	setMinimumSize(800, 500);
	setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);

	auto *layout = new QVBoxLayout(this);
	auto *placeholder =
		new QLabel(QStringLiteral("OBS Advanced Multiview"), this);
	placeholder->setAlignment(Qt::AlignCenter);
	layout->addWidget(placeholder);
}

ManagerDialog::~ManagerDialog() = default;
