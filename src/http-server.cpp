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

#include "http-server.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#ifndef _WIN32
#include <netinet/tcp.h>
#include <sys/select.h>
#endif

namespace vcast {

const char *const kStreamPath = "/stream.ts";
const char *const kStreamMimeType = "video/mp2t";
const char *const kDlnaContentFeatures = "DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";

namespace {

/* A client further behind than this is skipped ahead to the next keyframe. */
constexpr size_t kMaxQueuedBytes = 16 * 1024 * 1024;

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string header_value(const std::string &request, const std::string &name)
{
	std::string lowered = lower(request);
	std::string key = "\r\n" + lower(name) + ":";
	size_t pos = lowered.find(key);
	if (pos == std::string::npos)
		return std::string();
	pos += key.size();
	size_t end = request.find("\r\n", pos);
	std::string value = request.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
	size_t first = value.find_first_not_of(" \t");
	size_t last = value.find_last_not_of(" \t");
	return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}

void send_simple(vcast_socket_t sock, const char *status, const char *content_type, const std::string &body,
		 bool head_only)
{
	std::string response = std::string("HTTP/1.1 ") + status +
			       "\r\n"
			       "Server: Vcast\r\n"
			       "Content-Type: " +
			       content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
			       "\r\n"
			       "Cache-Control: no-cache\r\n"
			       "Connection: close\r\n\r\n";
	if (!head_only)
		response += body;
	socket_send_all(sock, response.data(), response.size());
}

std::string html_escape(const std::string &s)
{
	std::string out;
	for (char c : s) {
		switch (c) {
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '&':
			out += "&amp;";
			break;
		case '"':
			out += "&quot;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

} // namespace

HttpStreamServer::~HttpStreamServer()
{
	stop();
}

bool HttpStreamServer::start(uint16_t port, int attempts, std::string *error)
{
	if (running_)
		return true;

	for (int i = 0; i < std::max(attempts, 1); i++) {
		uint16_t candidate = (uint16_t)(port + i);
		vcast_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == VCAST_INVALID_SOCKET)
			break;
		int one = 1;
#ifdef _WIN32
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(candidate);
		if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 && listen(s, 16) == 0) {
			listen_sock_ = s;
			port_ = candidate;
			break;
		}
		socket_close(s);
	}

	if (listen_sock_ == VCAST_INVALID_SOCKET) {
		if (error)
			*error = "Could not open a network port between " + std::to_string(port) + " and " +
				 std::to_string(port + std::max(attempts, 1) - 1);
		return false;
	}

	running_ = true;
	accept_thread_ = std::thread(&HttpStreamServer::accept_loop, this);
	obs_log(LOG_INFO, "HTTP stream server listening on port %u", port_);
	return true;
}

void HttpStreamServer::stop()
{
	if (!running_.exchange(false))
		return;

	socket_shutdown(listen_sock_);
	if (accept_thread_.joinable())
		accept_thread_.join();
	socket_close(listen_sock_);
	listen_sock_ = VCAST_INVALID_SOCKET;

	{
		std::lock_guard<std::mutex> lock(clients_mutex_);
		for (auto &client : clients_) {
			std::lock_guard<std::mutex> client_lock(client->mutex);
			client->closed = true;
			client->cv.notify_all();
		}
		for (vcast_socket_t s : open_sockets_)
			socket_shutdown(s);
	}

	reap_workers(true);

	std::lock_guard<std::mutex> lock(clients_mutex_);
	clients_.clear();
	open_sockets_.clear();
	obs_log(LOG_INFO, "HTTP stream server stopped");
}

size_t HttpStreamServer::client_count()
{
	std::lock_guard<std::mutex> lock(clients_mutex_);
	return clients_.size();
}

void HttpStreamServer::broadcast(const uint8_t *data, size_t size, bool keyframe)
{
	if (!running_ || size == 0)
		return;

	std::shared_ptr<const std::vector<uint8_t>> chunk;
	std::lock_guard<std::mutex> lock(clients_mutex_);
	for (auto &client : clients_) {
		std::lock_guard<std::mutex> client_lock(client->mutex);
		if (client->closed)
			continue;
		if (!client->synced) {
			if (!keyframe)
				continue;
			client->synced = true;
		}
		if (client->queued_bytes > kMaxQueuedBytes) {
			obs_log(LOG_WARNING, "Client %s is too slow, skipping ahead", client->peer.c_str());
			client->queue.clear();
			client->queued_bytes = 0;
			client->synced = false;
			continue;
		}
		if (!chunk)
			chunk = std::make_shared<const std::vector<uint8_t>>(data, data + size);
		client->queue.push_back(chunk);
		client->queued_bytes += size;
		client->cv.notify_one();
	}
}

void HttpStreamServer::reap_workers(bool all)
{
	std::vector<Worker> finished;
	{
		std::lock_guard<std::mutex> lock(workers_mutex_);
		auto it = workers_.begin();
		while (it != workers_.end()) {
			if (all || it->done->load()) {
				finished.push_back(std::move(*it));
				it = workers_.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto &w : finished) {
		if (w.thread.joinable())
			w.thread.join();
	}
}

void HttpStreamServer::accept_loop()
{
	while (running_) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(listen_sock_, &rfds);
		timeval tv{0, 250000};
		int rc = select((int)listen_sock_ + 1, &rfds, nullptr, nullptr, &tv);
		reap_workers(false);
		if (rc <= 0 || !running_)
			continue;

		sockaddr_in addr{};
		socklen_t len = sizeof(addr);
		vcast_socket_t s = accept(listen_sock_, reinterpret_cast<sockaddr *>(&addr), &len);
		if (s == VCAST_INVALID_SOCKET)
			continue;

		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		std::string peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));

		socket_set_nosigpipe(s);
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

		{
			std::lock_guard<std::mutex> lock(clients_mutex_);
			open_sockets_.push_back(s);
		}

		Worker w;
		w.done = std::make_shared<std::atomic<bool>>(false);
		auto done = w.done;
		w.thread = std::thread([this, s, peer, done]() {
			handle_connection(s, peer);
			{
				std::lock_guard<std::mutex> lock(clients_mutex_);
				open_sockets_.erase(std::remove(open_sockets_.begin(), open_sockets_.end(), s),
						    open_sockets_.end());
			}
			socket_close(s);
			done->store(true);
		});
		std::lock_guard<std::mutex> lock(workers_mutex_);
		workers_.push_back(std::move(w));
	}
}

void HttpStreamServer::handle_connection(vcast_socket_t sock, std::string peer)
{
	socket_set_timeouts(sock, 5000, 10000);

	std::string request;
	char buf[2048];
	while (request.find("\r\n\r\n") == std::string::npos) {
		int n = (int)recv(sock, buf, sizeof(buf), 0);
		if (n <= 0 || request.size() > 16384)
			return;
		request.append(buf, (size_t)n);
	}

	size_t line_end = request.find("\r\n");
	std::string line = request.substr(0, line_end);
	size_t sp1 = line.find(' ');
	size_t sp2 = line.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos) {
		send_simple(sock, "400 Bad Request", "text/plain", "Bad request\n", false);
		return;
	}
	std::string method = line.substr(0, sp1);
	std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t query = path.find('?');
	if (query != std::string::npos)
		path.resize(query);

	bool head_only = method == "HEAD";
	if (method != "GET" && !head_only) {
		send_simple(sock, "405 Method Not Allowed", "text/plain", "Method not allowed\n", false);
		return;
	}

	obs_log(LOG_DEBUG, "%s %s from %s", method.c_str(), path.c_str(), peer.c_str());

	std::string host = header_value(request, "Host");
	if (host.empty())
		host = primary_local_ip() + ":" + std::to_string(port_);
	std::string stream_url = "http://" + host + kStreamPath;

	if (path == kStreamPath || path == "/live.ts" || path == "/vcast.ts") {
		bool dlna = !header_value(request, "getcontentFeatures.dlna.org").empty();
		serve_stream(sock, peer, head_only, dlna);
	} else if (path == "/vcast.m3u" || path == "/vcast.m3u8") {
		send_simple(sock, "200 OK", "audio/x-mpegurl", "#EXTM3U\n#EXTINF:-1,OBS (Vcast)\n" + stream_url + "\n",
			    head_only);
	} else if (path == "/" || path == "/index.html") {
		std::string url = html_escape(stream_url);
		std::string body =
			"<!doctype html><html><head><meta charset=\"utf-8\">"
			"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
			"<title>Vcast</title><style>body{font-family:sans-serif;max-width:40em;margin:2em auto;"
			"padding:0 1em;line-height:1.5}code{background:#eee;padding:.2em .4em;border-radius:4px;"
			"word-break:break-all}</style></head><body><h1>Vcast</h1>"
			"<p>OBS is casting live on this network.</p>"
			"<p>Open this address in VLC, Kodi, or your TV's media player app:</p>"
			"<p><code>" +
			url + "</code></p><p><a href=\"/vcast.m3u\">Download playlist (.m3u)</a></p></body></html>";
		send_simple(sock, "200 OK", "text/html; charset=utf-8", body, head_only);
	} else {
		send_simple(sock, "404 Not Found", "text/plain", "Not found\n", head_only);
	}
}

void HttpStreamServer::serve_stream(vcast_socket_t sock, const std::string &peer, bool head_only, bool dlna_features)
{
	std::string headers = std::string("HTTP/1.1 200 OK\r\n"
					  "Server: Vcast\r\n"
					  "Content-Type: ") +
			      kStreamMimeType +
			      "\r\n"
			      "Cache-Control: no-cache, no-store\r\n"
			      "Pragma: no-cache\r\n"
			      "Accept-Ranges: none\r\n"
			      "transferMode.dlna.org: Streaming\r\n"
			      "Connection: close\r\n";
	if (dlna_features)
		headers += std::string("contentFeatures.dlna.org: http-get:*:") + kStreamMimeType + ":" +
			   kDlnaContentFeatures + "\r\n";
	headers += "\r\n";

	if (!socket_send_all(sock, headers.data(), headers.size()) || head_only)
		return;

	auto client = std::make_shared<Client>();
	client->sock = sock;
	client->peer = peer;
	{
		std::lock_guard<std::mutex> lock(clients_mutex_);
		if (!running_)
			return;
		clients_.push_back(client);
	}
	obs_log(LOG_INFO, "Client connected: %s", peer.c_str());

	for (;;) {
		std::shared_ptr<const std::vector<uint8_t>> chunk;
		{
			std::unique_lock<std::mutex> lock(client->mutex);
			client->cv.wait(lock, [&]() { return client->closed || !client->queue.empty(); });
			if (client->closed)
				break;
			chunk = client->queue.front();
			client->queue.pop_front();
			client->queued_bytes -= chunk->size();
		}
		if (!socket_send_all(sock, chunk->data(), chunk->size()))
			break;
	}

	{
		std::lock_guard<std::mutex> lock(clients_mutex_);
		clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
	}
	obs_log(LOG_INFO, "Client disconnected: %s", peer.c_str());
}

} // namespace vcast
