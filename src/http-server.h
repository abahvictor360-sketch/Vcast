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

#include "net-util.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcast {

/* Path of the live MPEG-TS stream served to TVs and players. */
extern const char *const kStreamPath;
/* DLNA protocolInfo / contentFeatures for the live stream. */
extern const char *const kStreamMimeType;
extern const char *const kDlnaContentFeatures;

/*
 * Small HTTP server that fans one live MPEG-TS stream out to any number of
 * clients (TVs, VLC, phones...). New clients start at the next keyframe so
 * they can decode immediately; clients that fall too far behind are skipped
 * ahead to the next keyframe instead of stalling everyone else.
 */
class HttpStreamServer {
public:
	HttpStreamServer() = default;
	~HttpStreamServer();

	HttpStreamServer(const HttpStreamServer &) = delete;
	HttpStreamServer &operator=(const HttpStreamServer &) = delete;

	/* Binds to the first free port in [port, port + attempts). */
	bool start(uint16_t port, int attempts, std::string *error);
	void stop();

	bool running() const { return running_; }
	uint16_t port() const { return port_; }
	size_t client_count();

	/* chunk must consist of whole TS packets; keyframe marks a clean join point. */
	void broadcast(const uint8_t *data, size_t size, bool keyframe);

private:
	struct Client {
		vcast_socket_t sock = VCAST_INVALID_SOCKET;
		std::string peer;
		std::mutex mutex;
		std::condition_variable cv;
		std::deque<std::shared_ptr<const std::vector<uint8_t>>> queue;
		size_t queued_bytes = 0;
		bool synced = false;
		bool closed = false;
	};

	struct Worker {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};

	void accept_loop();
	void handle_connection(vcast_socket_t sock, std::string peer);
	void serve_stream(vcast_socket_t sock, const std::string &peer, bool head_only, bool dlna_features);
	void reap_workers(bool all);

	std::atomic<bool> running_{false};
	uint16_t port_ = 0;
	vcast_socket_t listen_sock_ = VCAST_INVALID_SOCKET;
	std::thread accept_thread_;

	std::mutex clients_mutex_;
	std::vector<std::shared_ptr<Client>> clients_;
	std::vector<vcast_socket_t> open_sockets_;

	std::mutex workers_mutex_;
	std::vector<Worker> workers_;
};

} // namespace vcast
