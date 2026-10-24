/*
Vcast - cast OBS to TVs and players on your local network
Copyright (C) 2026 Vcast contributors

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

#include "settings-dialog.h"
#include "cast-manager.h"

#include <obs-module.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(T("Vcast.Settings.Title"));
	const vcast::Settings &s = vcast::CastManager::instance().settings();

	auto *layout = new QVBoxLayout(this);
	auto *form = new QFormLayout();

	encoder_ = new QComboBox(this);
	for (auto &enc : vcast::available_h264_encoders())
		encoder_->addItem(QString::fromStdString(enc.second), QString::fromStdString(enc.first));
	int index = encoder_->findData(QString::fromStdString(s.video_encoder));
	encoder_->setCurrentIndex(index >= 0 ? index : 0);
	form->addRow(T("Vcast.Settings.Encoder"), encoder_);

	videoBitrate_ = new QSpinBox(this);
	videoBitrate_->setRange(500, 50000);
	videoBitrate_->setSingleStep(500);
	videoBitrate_->setSuffix(" Kbps");
	videoBitrate_->setValue(s.video_bitrate);
	form->addRow(T("Vcast.Settings.VideoBitrate"), videoBitrate_);

	audioBitrate_ = new QComboBox(this);
	for (int rate : {96, 128, 160, 192, 256, 320})
		audioBitrate_->addItem(QString("%1 Kbps").arg(rate), rate);
	index = audioBitrate_->findData(s.audio_bitrate);
	audioBitrate_->setCurrentIndex(index >= 0 ? index : 2);
	form->addRow(T("Vcast.Settings.AudioBitrate"), audioBitrate_);

	resolution_ = new QComboBox(this);
	resolution_->addItem(T("Vcast.Settings.Resolution.Canvas"), (int)vcast::Resolution::Canvas);
	resolution_->addItem("1080p", (int)vcast::Resolution::P1080);
	resolution_->addItem("720p", (int)vcast::Resolution::P720);
	resolution_->addItem("480p", (int)vcast::Resolution::P480);
	index = resolution_->findData((int)s.resolution);
	resolution_->setCurrentIndex(index >= 0 ? index : 0);
	form->addRow(T("Vcast.Settings.Resolution"), resolution_);

	port_ = new QSpinBox(this);
	port_->setRange(1024, 65000);
	port_->setValue(s.port);
	form->addRow(T("Vcast.Settings.Port"), port_);

	title_ = new QLineEdit(QString::fromStdString(s.title), this);
	form->addRow(T("Vcast.Settings.CastTitle"), title_);

	layout->addLayout(form);

	auto *note = new QLabel(T("Vcast.Settings.Note"), this);
	note->setWordWrap(true);
	layout->addWidget(note);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
	layout->addWidget(buttons);
}

void SettingsDialog::accept()
{
	vcast::Settings &s = vcast::CastManager::instance().settings();
	if (encoder_->currentIndex() >= 0)
		s.video_encoder = encoder_->currentData().toString().toStdString();
	s.video_bitrate = videoBitrate_->value();
	s.audio_bitrate = audioBitrate_->currentData().toInt();
	s.resolution = (vcast::Resolution)resolution_->currentData().toInt();
	s.port = port_->value();
	s.title = title_->text().trimmed().toStdString();
	if (s.title.empty())
		s.title = "OBS Studio";
	s.save();
	QDialog::accept();
}
