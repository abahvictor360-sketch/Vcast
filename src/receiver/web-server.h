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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcast {

struct HttpRequest {
	std::string method;
	std::string path;                           // without query
	std::string query;                          // raw query string
	std::map<std::string, std::string> headers; // lower-case names
	std::string peer;

	std::string query_param(const std::string &name) const;
	std::string header(const std::string &name) const;
};

struct HttpReply {
	int status = 200;
	std::string content_type = "text/plain; charset=utf-8";
	std::string body;
	std::vector<std::pair<std::string, std::string>> headers;
};

/* One accepted WebSocket. send_* may be called from any thread. */
class WebSocket {
public:
	virtual ~WebSocket() = default;
	virtual bool send_text(const std::string &text) = 0;
	virtual void close() = 0;
	virtual const std::string &peer() const = 0;
};

struct WebSocketHandler {
	/* Called on the connection's thread. Return false from a callback to close. */
	std::function<bool(WebSocket &ws, const uint8_t *data, size_t size)> on_binary;
	std::function<bool(WebSocket &ws, const std::string &text)> on_text;
	std::function<void(WebSocket &ws)> on_close;
};

/*
 * HTTPS + WebSocket server (mbedTLS, TLS 1.2, self-signed certificate kept in
 * the plugin config folder). Browsers only allow screen capture on secure
 * pages, so the laptop cast page must be served over HTTPS.
 */
class WebServer {
public:
	using HttpRoute = std::function<HttpReply(const HttpRequest &)>;
	/* Return a handler to accept the upgrade, or an empty handler (no on_binary/on_text) to refuse. */
	using WsRoute = std::function<std::shared_ptr<WebSocketHandler>(const HttpRequest &)>;

	WebServer();
	~WebServer();

	WebServer(const WebServer &) = delete;
	WebServer &operator=(const WebServer &) = delete;

	void set_http_handler(HttpRoute route) { http_route_ = std::move(route); }
	void set_ws_handler(WsRoute route) { ws_route_ = std::move(route); }

	/* cert_dir: where vcast-cert.pem / vcast-key.pem are created or loaded. */
	bool start(uint16_t port, int attempts, const std::string &cert_dir, std::string *error);
	void stop();
	bool running() const { return running_; }
	uint16_t port() const { return port_; }

private:
	struct Tls;
	struct Worker {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};

	void accept_loop();
	void handle_connection(vcast_socket_t sock, const std::string &peer);
	void reap(bool all);

	std::unique_ptr<Tls> tls_;
	HttpRoute http_route_;
	WsRoute ws_route_;
	std::atomic<bool> running_{false};
	uint16_t port_ = 0;
	vcast_socket_t listen_sock_ = VCAST_INVALID_SOCKET;
	std::thread accept_thread_;
	std::mutex mutex_;
	std::vector<Worker> workers_;
	std::vector<vcast_socket_t> open_sockets_;
};

} // namespace vcast
