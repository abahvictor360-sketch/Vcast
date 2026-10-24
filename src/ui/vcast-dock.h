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

#pragma once

#include "dlna.h"

#include <QWidget>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

/* The "Vcast" dock: find TVs on the network and cast the OBS output to them. */
class VcastDock : public QWidget {
	Q_OBJECT

public:
	explicit VcastDock(QWidget *parent = nullptr);
	~VcastDock() override;

private slots:
	void search();
	void castToSelected();
	void stopAll();
	void toggleLink();
	void addByAddress();
	void openSettings();
	void copyUrl();
	void refreshState();

private:
	/* Runs job on a worker thread, then done on the UI thread. One job at a time. */
	void runAsync(std::function<void()> job, std::function<void()> done);
	void setDevices(const std::vector<vcast::DlnaDevice> &devices);
	void setStatus(const QString &text, bool error = false);
	void setBusy(bool busy);
	const vcast::DlnaDevice *selectedDevice() const;

	QListWidget *deviceList_;
	QPushButton *searchButton_;
	QPushButton *addButton_;
	QPushButton *castButton_;
	QPushButton *stopButton_;
	QPushButton *linkButton_;
	QPushButton *settingsButton_;
	QPushButton *copyButton_;
	QLineEdit *urlEdit_;
	QLabel *statusLabel_;
	QLabel *clientsLabel_;
	QTimer *timer_;

	std::vector<vcast::DlnaDevice> devices_;
	std::thread worker_;
	std::atomic<bool> busy_{false};
};
