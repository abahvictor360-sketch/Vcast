/*
Vcast - cast to and from OBS on your local network
Copyright (C) 2026 Vcast contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include "net-util.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vcast {

class Receiver;

constexpr uint16_t kDiscoveryPort = 48689;
constexpr uint16_t kAppIngestPort = 48690;

/*
 * TCP server for the Vcast Android app (see docs/protocol.md). One per
 * receiver; a newly connected phone replaces the current sender.
 */
class AppIngestServer {
public:
	explicit AppIngestServer(Receiver *receiver) : receiver_(receiver) {}
	~AppIngestServer();

	bool start(uint16_t port, int attempts, std::string *error);
	void stop();
	uint16_t port() const { return port_; }

private:
	struct Worker {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	void accept_loop();
	void handle(vcast_socket_t sock, const std::string &peer);
	void reap(bool all);

	Receiver *receiver_;
	std::atomic<bool> running_{false};
	uint16_t port_ = 0;
	vcast_socket_t listen_sock_ = VCAST_INVALID_SOCKET;
	std::thread accept_thread_;
	std::mutex mutex_;
	std::vector<Worker> workers_;
	std::vector<vcast_socket_t> open_sockets_;
};

/* Answers "VCAST?" broadcasts from the Android app with every receiver's name and port. */
class DiscoveryResponder {
public:
	using Provider = std::function<std::vector<std::pair<std::string, uint16_t>>()>;

	~DiscoveryResponder();
	bool start(Provider provider);
	void stop();

private:
	void run();

	Provider provider_;
	std::atomic<bool> running_{false};
	vcast_socket_t sock_ = VCAST_INVALID_SOCKET;
	std::thread thread_;
};

std::string json_escape(const std::string &s);

} // namespace vcast
