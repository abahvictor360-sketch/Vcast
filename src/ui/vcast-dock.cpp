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

#include "vcast-dock.h"
#include "settings-dialog.h"
#include "cast-manager.h"
#include "miracast/miracast-source.h"
#include "receiver/receiver-source.h"

#include <obs-frontend-api.h>

#include <obs-module.h>
#include <util/config-file.h>

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>
#include <memory>

using vcast::CastManager;
using vcast::DlnaDevice;

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

VcastDock::VcastDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);

	/* Receive: phones and laptops cast into OBS. */
	auto *receiveHint = new QLabel(T("Vcast.ReceiveHint"), this);
	receiveHint->setWordWrap(true);
	receiveHint->setStyleSheet("font-weight: bold;");
	layout->addWidget(receiveHint);
	auto *addReceiverButton = new QPushButton(T("Vcast.AddReceiver"), this);
	layout->addWidget(addReceiverButton);
	receiveUrlLabel_ = new QLabel(this);
	receiveUrlLabel_->setWordWrap(true);
	receiveUrlLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
	receiveUrlLabel_->setOpenExternalLinks(true);
	layout->addWidget(receiveUrlLabel_);
	connect(addReceiverButton, &QPushButton::clicked, this,
		[this]() { addReceiver(VCAST_RECEIVER_SOURCE_ID, "Vcast.Receiver.Name"); });
#ifdef _WIN32
	/* Win + K on laptops, Cast / Smart View on Android (Miracast). */
	auto *addMiracastButton = new QPushButton(T("Vcast.AddMiracast"), this);
	addMiracastButton->setToolTip(T("Vcast.AddMiracast.Tip"));
	layout->insertWidget(layout->indexOf(addReceiverButton) + 1, addMiracastButton);
	connect(addMiracastButton, &QPushButton::clicked, this,
		[this]() { addReceiver(VCAST_MIRACAST_SOURCE_ID, "Vcast.Miracast.Name"); });
#endif
	auto *fitButton = new QPushButton(T("Vcast.FitToScreen"), this);
	layout->addWidget(fitButton);
	connect(fitButton, &QPushButton::clicked, this, []() { vcast_receiver_fit_to_screen(nullptr, false); });

	/* Program is only 1920 x 1080 when OBS's own canvas is. */
	canvasLabel_ = new QLabel(this);
	canvasLabel_->setWordWrap(true);
	canvasLabel_->setStyleSheet("color: #e0a800;");
	layout->addWidget(canvasLabel_);
	canvasButton_ = new QPushButton(T("Vcast.Canvas1080"), this);
	layout->addWidget(canvasButton_);
	connect(canvasButton_, &QPushButton::clicked, this, &VcastDock::setCanvas1080);
	canvasLabel_->hide();
	canvasButton_->hide();

	auto *line = new QFrame(this);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	layout->addWidget(line);

	/* Send: OBS casts to a TV. */
	auto *sendHint = new QLabel(T("Vcast.SendHint"), this);
	sendHint->setWordWrap(true);
	sendHint->setStyleSheet("font-weight: bold;");
	layout->addWidget(sendHint);

	auto *intro = new QLabel(T("Vcast.Intro"), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	deviceList_ = new QListWidget(this);
	deviceList_->setMinimumHeight(90);
	layout->addWidget(deviceList_, 1);

	auto *findRow = new QHBoxLayout();
	searchButton_ = new QPushButton(T("Vcast.Search"), this);
	addButton_ = new QPushButton(T("Vcast.AddByAddress"), this);
	findRow->addWidget(searchButton_);
	findRow->addWidget(addButton_);
	layout->addLayout(findRow);

	auto *castRow = new QHBoxLayout();
	castButton_ = new QPushButton(T("Vcast.Cast"), this);
	stopButton_ = new QPushButton(T("Vcast.Stop"), this);
	castRow->addWidget(castButton_, 1);
	castRow->addWidget(stopButton_);
	layout->addLayout(castRow);

	statusLabel_ = new QLabel(this);
	statusLabel_->setWordWrap(true);
	statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(statusLabel_);

	auto *linkLabel = new QLabel(T("Vcast.LinkHint"), this);
	linkLabel->setWordWrap(true);
	layout->addWidget(linkLabel);

	auto *urlRow = new QHBoxLayout();
	urlEdit_ = new QLineEdit(this);
	urlEdit_->setReadOnly(true);
	urlEdit_->setPlaceholderText(T("Vcast.LinkOff"));
	copyButton_ = new QPushButton(T("Vcast.Copy"), this);
	urlRow->addWidget(urlEdit_, 1);
	urlRow->addWidget(copyButton_);
	layout->addLayout(urlRow);

	auto *bottomRow = new QHBoxLayout();
	linkButton_ = new QPushButton(T("Vcast.StartLink"), this);
	settingsButton_ = new QPushButton(T("Vcast.Settings"), this);
	clientsLabel_ = new QLabel(this);
	bottomRow->addWidget(linkButton_);
	bottomRow->addWidget(settingsButton_);
	bottomRow->addStretch(1);
	bottomRow->addWidget(clientsLabel_);
	layout->addLayout(bottomRow);

	connect(searchButton_, &QPushButton::clicked, this, &VcastDock::search);
	connect(addButton_, &QPushButton::clicked, this, &VcastDock::addByAddress);
	connect(castButton_, &QPushButton::clicked, this, &VcastDock::castToSelected);
	connect(stopButton_, &QPushButton::clicked, this, &VcastDock::stopAll);
	connect(linkButton_, &QPushButton::clicked, this, &VcastDock::toggleLink);
	connect(settingsButton_, &QPushButton::clicked, this, &VcastDock::openSettings);
	connect(copyButton_, &QPushButton::clicked, this, &VcastDock::copyUrl);
	connect(deviceList_, &QListWidget::itemSelectionChanged, this, &VcastDock::refreshState);
	connect(deviceList_, &QListWidget::itemDoubleClicked, this, &VcastDock::castToSelected);

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, &VcastDock::refreshState);
	timer_->start(1000);

	setStatus(T("Vcast.Status.Idle"));
	refreshState();

	/* Find TVs right away so the list is ready when the user opens the dock. */
	QTimer::singleShot(1500, this, &VcastDock::search);
}

VcastDock::~VcastDock()
{
	if (worker_.joinable())
		worker_.join();
}

void VcastDock::runAsync(std::function<void()> job, std::function<void()> done)
{
	if (busy_.exchange(true))
		return;
	if (worker_.joinable())
		worker_.join();
	setBusy(true);
	worker_ = std::thread([this, job, done]() {
		job();
		QMetaObject::invokeMethod(
			this,
			[this, done]() {
				busy_ = false;
				setBusy(false);
				done();
				refreshState();
			},
			Qt::QueuedConnection);
	});
}

void VcastDock::setBusy(bool busy)
{
	searchButton_->setEnabled(!busy);
	addButton_->setEnabled(!busy);
	settingsButton_->setEnabled(!busy);
	refreshState();
}

void VcastDock::setStatus(const QString &text, bool error)
{
	statusLabel_->setText(text);
	statusLabel_->setStyleSheet(error ? "color: #e5533d;" : "");
}

const DlnaDevice *VcastDock::selectedDevice() const
{
	int row = deviceList_->currentRow();
	if (row < 0 || row >= (int)devices_.size() || deviceList_->selectedItems().isEmpty())
		return nullptr;
	return &devices_[(size_t)row];
}

void VcastDock::setDevices(const std::vector<DlnaDevice> &devices)
{
	std::string selected_udn;
	if (const DlnaDevice *d = selectedDevice())
		selected_udn = d->udn;
	const std::string &last = CastManager::instance().settings().last_device_location;

	devices_ = devices;
	deviceList_->clear();
	int select = -1;
	for (size_t i = 0; i < devices_.size(); i++) {
		const DlnaDevice &d = devices_[i];
		QString label = QString::fromStdString(d.name);
		QString detail = QString::fromStdString(d.manufacturer.empty() ? d.model : d.manufacturer);
		if (!detail.isEmpty() && !label.contains(detail))
			label += QString(" (%1)").arg(detail);
		label += QString(" - %1").arg(QString::fromStdString(d.ip));
		deviceList_->addItem(label);
		if (d.udn == selected_udn || (select < 0 && d.location == last))
			select = (int)i;
	}
	if (select < 0 && devices_.size() == 1)
		select = 0;
	if (select >= 0)
		deviceList_->setCurrentRow(select);
}

void VcastDock::search()
{
	setStatus(T("Vcast.Status.Searching"));
	auto found = std::make_shared<std::vector<DlnaDevice>>();
	std::string last = CastManager::instance().settings().last_device_location;
	runAsync(
		[found, last]() {
			*found = vcast::dlna_discover(4000);
			/* Remember the last TV even if it ignored the multicast search. */
			bool have_last = last.empty();
			for (auto &d : *found)
				have_last = have_last || d.location == last;
			DlnaDevice device;
			if (!have_last && vcast::dlna_describe(last, device, nullptr))
				found->push_back(device);
		},
		[this, found]() {
			setDevices(*found);
			if (found->empty())
				setStatus(T("Vcast.Status.NoDevices"), true);
			else
				setStatus(QString(T("Vcast.Status.Found")).arg(found->size()));
		});
}

void VcastDock::addByAddress()
{
	bool ok = false;
	QString text = QInputDialog::getText(this, T("Vcast.AddByAddress"), T("Vcast.AddByAddress.Prompt"),
					     QLineEdit::Normal, QString(), &ok)
			       .trimmed();
	if (!ok || text.isEmpty())
		return;

	setStatus(T("Vcast.Status.Searching"));
	auto found = std::make_shared<std::vector<DlnaDevice>>();
	auto error = std::make_shared<std::string>();
	std::string input = text.toStdString();
	runAsync(
		[found, error, input]() {
			if (input.rfind("http://", 0) == 0) {
				DlnaDevice device;
				if (vcast::dlna_describe(input, device, error.get()))
					found->push_back(device);
			} else {
				*found = vcast::dlna_discover(3000, input);
				if (found->empty())
					*error = "no media renderer answered at " + input;
			}
		},
		[this, found, error]() {
			if (found->empty()) {
				setStatus(QString::fromStdString(*error), true);
				return;
			}
			std::vector<DlnaDevice> merged = devices_;
			for (auto &d : *found) {
				bool exists = false;
				for (auto &m : merged)
					exists = exists || m.udn == d.udn;
				if (!exists)
					merged.push_back(d);
			}
			setDevices(merged);
			for (size_t i = 0; i < devices_.size(); i++) {
				if (devices_[i].udn == found->front().udn)
					deviceList_->setCurrentRow((int)i);
			}
			setStatus(QString(T("Vcast.Status.Found")).arg(devices_.size()));
		});
}

void VcastDock::castToSelected()
{
	const DlnaDevice *selected = selectedDevice();
	if (!selected) {
		setStatus(T("Vcast.Status.SelectDevice"), true);
		return;
	}
	DlnaDevice device = *selected;
	auto &manager = CastManager::instance();

	std::string error;
	if (!manager.start_sharing(&error)) {
		setStatus(QString::fromStdString(error), true);
		return;
	}

	manager.settings().last_device_location = device.location;
	manager.settings().save();

	setStatus(QString(T("Vcast.Status.Connecting")).arg(QString::fromStdString(device.name)));
	auto result = std::make_shared<std::string>();
	auto ok = std::make_shared<bool>(false);
	runAsync([device, result, ok]() { *ok = CastManager::instance().cast_to(device, result.get()); },
		 [this, device, result, ok]() {
			 if (*ok)
				 setStatus(QString(T("Vcast.Status.Casting")).arg(QString::fromStdString(device.name)));
			 else
				 setStatus(QString(T("Vcast.Status.CastFailed"))
						   .arg(QString::fromStdString(device.name),
							QString::fromStdString(*result)),
					   true);
		 });
}

void VcastDock::stopAll()
{
	runAsync([]() { CastManager::instance().stop_cast(); },
		 [this]() {
			 CastManager::instance().stop_sharing();
			 setStatus(T("Vcast.Status.Stopped"));
		 });
}

void VcastDock::toggleLink()
{
	auto &manager = CastManager::instance();
	if (manager.sharing()) {
		stopAll();
		return;
	}
	std::string error;
	if (manager.start_sharing(&error))
		setStatus(T("Vcast.Status.LinkOn"));
	else
		setStatus(QString::fromStdString(error), true);
	refreshState();
}

void VcastDock::openSettings()
{
	SettingsDialog dialog(this);
	if (dialog.exec() == QDialog::Accepted && CastManager::instance().sharing())
		setStatus(T("Vcast.Status.RestartToApply"));
}

void VcastDock::copyUrl()
{
	if (!urlEdit_->text().isEmpty())
		QApplication::clipboard()->setText(urlEdit_->text());
}

void VcastDock::addReceiver(const char *id, const char *name_key)
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	if (!scene) {
		obs_source_release(scene_source);
		return;
	}

	/* Reuse an existing receiver if there is one, so the AirPlay name stays the same. */
	struct Search {
		const char *id;
		obs_source_t *found;
	} search = {id, nullptr};
	obs_enum_sources(
		[](void *param, obs_source_t *candidate) {
			auto *search = static_cast<Search *>(param);
			if (strcmp(obs_source_get_id(candidate), search->id) != 0)
				return true;
			search->found = obs_source_get_ref(candidate);
			return false;
		},
		&search);
	obs_source_t *source = search.found;
	if (!source) {
		std::string name = T(name_key).toStdString();
		source = obs_source_create(id, name.c_str(), nullptr, nullptr);
	}
	if (source && !obs_scene_find_source(scene, obs_source_get_name(source))) {
		obs_scene_add(scene, source);
		vcast_receiver_fit_to_screen(source, true);
	}
	obs_source_release(source);
	obs_source_release(scene_source);
	refreshState();
}

void VcastDock::setCanvas1080()
{
	if (obs_video_active()) {
		QMessageBox::information(this, T("Vcast.Canvas1080"), T("Vcast.Canvas1080.Busy"));
		return;
	}
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;
	config_set_uint(config, "Video", "BaseCX", 1920);
	config_set_uint(config, "Video", "BaseCY", 1080);
	config_set_uint(config, "Video", "OutputCX", 1920);
	config_set_uint(config, "Video", "OutputCY", 1080);
	config_save_safe(config, "tmp", nullptr);
	obs_frontend_reset_video();
	/* The canvas changed size, so every receiver is fitted again. */
	vcast_receiver_fit_to_screen(nullptr, false);
	refreshState();
}

void VcastDock::refreshState()
{
	/* Receivers added by hand from the Sources list also fill the screen. */
	vcast_receiver_fit_to_screen(nullptr, true);

	obs_video_info ovi = {};
	bool full_hd = !obs_get_video_info(&ovi) || (ovi.base_width == 1920 && ovi.base_height == 1080 &&
						     ovi.output_width == 1920 && ovi.output_height == 1080);
	if (!full_hd) {
		QString text = QString(T("Vcast.Canvas1080.Warning"))
				       .arg(ovi.base_width)
				       .arg(ovi.base_height)
				       .arg(ovi.output_width)
				       .arg(ovi.output_height);
		if (canvasLabel_->text() != text)
			canvasLabel_->setText(text);
	}
	canvasLabel_->setVisible(!full_hd);
	canvasButton_->setVisible(!full_hd);

	std::string web = vcast_receiver_web_url();
	QString receiveText = web.empty() ? QString()
					  : QString(T("Vcast.ReceiveUrl"))
						    .arg(QString::fromStdString(web),
							 QString::fromStdString(vcast_receiver_default_name()));
	if (receiveUrlLabel_->text() != receiveText)
		receiveUrlLabel_->setText(receiveText);

	auto &manager = CastManager::instance();
	bool sharing = manager.sharing();
	bool busy = busy_;

	QString url = sharing ? QString::fromStdString(manager.stream_url()) : QString();
	if (urlEdit_->text() != url)
		urlEdit_->setText(url);
	copyButton_->setEnabled(sharing);

	castButton_->setEnabled(!busy && selectedDevice() != nullptr);
	stopButton_->setEnabled(!busy && (sharing || manager.casting()));
	linkButton_->setEnabled(!busy);
	linkButton_->setText(sharing ? T("Vcast.StopLink") : T("Vcast.StartLink"));

	if (sharing)
		clientsLabel_->setText(QString(T("Vcast.Viewers")).arg(manager.client_count()));
	else
		clientsLabel_->clear();
}
