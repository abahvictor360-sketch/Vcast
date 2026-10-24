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

#include "app-ingest.h"
#include "receiver.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>

#ifndef _WIN32
#include <netinet/tcp.h>
#include <sys/select.h>
#endif

namespace vcast {

namespace {

constexpr uint8_t kHello = 0x01;
constexpr uint8_t kVideoConfig = 0x10;
constexpr uint8_t kVideoFrame = 0x11;
constexpr uint8_t kVideoKeyframe = 0x12;
constexpr uint8_t kAudioConfig = 0x20;
constexpr uint8_t kAudioFrame = 0x21;
constexpr uint8_t kBye = 0x31;
constexpr uint32_t kMaxPayload = 16 * 1024 * 1024;
constexpr int kIdleTimeoutMs = 10000;

int wait_readable(vcast_socket_t s, int timeout_ms)
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(s, &rfds);
	timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	return select((int)s + 1, &rfds, nullptr, nullptr, &tv);
}

/* Reads exactly size bytes, giving up after the idle timeout or when stopped. */
bool read_exact(vcast_socket_t s, uint8_t *buf, size_t size, const std::atomic<bool> &running)
{
	size_t got = 0;
	uint64_t deadline = os_gettime_ns() + (uint64_t)kIdleTimeoutMs * 1000000ULL;
	while (got < size) {
		if (!running || os_gettime_ns() > deadline)
			return false;
		int ready = wait_readable(s, 250);
		if (ready < 0)
			return false;
		if (ready == 0)
			continue;
		int n = (int)recv(s, (char *)buf + got, (int)std::min(size - got, (size_t)0x100000), 0);
		if (n <= 0)
			return false;
		got += (size_t)n;
		deadline = os_gettime_ns() + (uint64_t)kIdleTimeoutMs * 1000000ULL;
	}
	return true;
}

uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int64_t be64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return (int64_t)v;
}

std::string json_string_field(const std::string &json, const std::string &key)
{
	std::string needle = "\"" + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos)
		return std::string();
	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos)
		return std::string();
	pos = json.find('"', pos);
	if (pos == std::string::npos)
		return std::string();
	std::string out;
	for (size_t i = pos + 1; i < json.size() && json[i] != '"'; i++) {
		if (json[i] == '\\' && i + 1 < json.size())
			i++;
		out += json[i];
	}
	return out.substr(0, 64);
}

} // namespace

std::string json_escape(const std::string &s)
{
	std::string out;
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if ((unsigned char)c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out;
}

/* ------------------------------------------------------------------------- */

AppIngestServer::~AppIngestServer()
{
	stop();
}

bool AppIngestServer::start(uint16_t port, int attempts, std::string *error)
{
	if (running_)
		return true;
	for (int i = 0; i < std::max(attempts, 1); i++) {
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
		addr.sin_port = htons((uint16_t)(port + i));
		if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 && listen(s, 4) == 0) {
			listen_sock_ = s;
			port_ = (uint16_t)(port + i);
			break;
		}
		socket_close(s);
	}
	if (listen_sock_ == VCAST_INVALID_SOCKET) {
		if (error)
			*error = "Could not open a network port for the Android app";
		return false;
	}
	running_ = true;
	accept_thread_ = std::thread(&AppIngestServer::accept_loop, this);
	return true;
}

void AppIngestServer::stop()
{
	if (!running_.exchange(false))
		return;
	socket_shutdown(listen_sock_);
	if (accept_thread_.joinable())
		accept_thread_.join();
	socket_close(listen_sock_);
	listen_sock_ = VCAST_INVALID_SOCKET;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto s : open_sockets_)
			socket_shutdown(s);
	}
	reap(true);
}

void AppIngestServer::reap(bool all)
{
	std::vector<Worker> finished;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto it = workers_.begin(); it != workers_.end();) {
			if (all || it->done->load()) {
				finished.push_back(std::move(*it));
				it = workers_.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto &w : finished)
		if (w.thread.joinable())
			w.thread.join();
}

void AppIngestServer::accept_loop()
{
	while (running_) {
		int rc = wait_readable(listen_sock_, 250);
		reap(false);
		if (rc <= 0 || !running_)
			continue;
		sockaddr_in addr{};
		socklen_t len = sizeof(addr);
		vcast_socket_t s = accept(listen_sock_, reinterpret_cast<sockaddr *>(&addr), &len);
		if (s == VCAST_INVALID_SOCKET)
			continue;
		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		socket_set_nosigpipe(s);
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

		Worker w;
		w.done = std::make_shared<std::atomic<bool>>(false);
		auto done = w.done;
		std::string peer = ip;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			open_sockets_.push_back(s);
		}
		w.thread = std::thread([this, s, peer, done]() {
			handle(s, peer);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				open_sockets_.erase(std::remove(open_sockets_.begin(), open_sockets_.end(), s),
						    open_sockets_.end());
			}
			socket_close(s);
			done->store(true);
		});
		std::lock_guard<std::mutex> lock(mutex_);
		workers_.push_back(std::move(w));
	}
}

void AppIngestServer::handle(vcast_socket_t sock, const std::string &peer)
{
	uint8_t magic[8];
	if (!read_exact(sock, magic, sizeof(magic), running_) || memcmp(magic, "VCAST/1\n", 8) != 0) {
		const char *err = "VCAST/1 ERR unsupported protocol\n";
		socket_send_all(sock, err, strlen(err));
		return;
	}
	std::string ok = "VCAST/1 OK " + receiver_->name() + "\n";
	if (!socket_send_all(sock, ok.data(), ok.size()))
		return;

	uint64_t session = receiver_->begin_session("Vcast app", peer, [sock]() { socket_shutdown(sock); });
	std::vector<uint8_t> payload;

	while (running_ && receiver_->is_active(session)) {
		uint8_t header[13];
		if (!read_exact(sock, header, sizeof(header), running_))
			break;
		uint8_t type = header[0];
		uint32_t length = be32(header + 1);
		int64_t pts_us = be64(header + 5);
		if (length > kMaxPayload)
			break;
		payload.resize(length);
		if (length && !read_exact(sock, payload.data(), length, running_))
			break;
		if (!receiver_->is_active(session))
			break;

		if (type == kBye)
			break;
		if (type == kHello) {
			std::string device = json_string_field(std::string(payload.begin(), payload.end()), "device");
			if (!device.empty())
				receiver_->set_device_name(session, device);
			continue;
		}
		bool alive = receiver_->feed(session, [&](StreamDecoder &decoder) {
			switch (type) {
			case kVideoConfig:
				if (length >= 1) {
					AVCodecID codec = payload[0] == 2 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
					/* Parameter sets are passed in-band ahead of the next keyframe. */
					decoder.configure_video(codec, nullptr, 0);
					decoder.decode_video(payload.data() + 1, length - 1, -1);
				}
				break;
			case kVideoFrame:
			case kVideoKeyframe:
				decoder.decode_video(payload.data(), length, pts_us);
				break;
			case kAudioConfig:
				if (length >= 6 && payload[0] == 1) {
					int rate = (int)be32(payload.data() + 1);
					int channels = payload[5];
					decoder.configure_audio(AV_CODEC_ID_AAC, rate, channels, payload.data() + 6,
								length - 6);
				}
				break;
			case kAudioFrame:
				decoder.decode_audio(payload.data(), length, pts_us);
				break;
			default:
				break; // PING (0x30) and unknown frame types (forward compatibility)
			}
		});
		if (!alive)
			break;
	}
	receiver_->end_session(session);
}

/* ------------------------------------------------------------------------- */

DiscoveryResponder::~DiscoveryResponder()
{
	stop();
}

bool DiscoveryResponder::start(Provider provider)
{
	if (running_)
		return true;
	provider_ = std::move(provider);
	sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_ == VCAST_INVALID_SOCKET)
		return false;
	int one = 1;
	/* Several OBS instances on one PC may all answer. */
	setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#ifdef SO_REUSEPORT
	setsockopt(sock_, SOL_SOCKET, SO_REUSEPORT, (const char *)&one, sizeof(one));
#endif
	setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(kDiscoveryPort);
	if (bind(sock_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		obs_log(LOG_WARNING, "Could not listen for Android app discovery on UDP %u", kDiscoveryPort);
		socket_close(sock_);
		sock_ = VCAST_INVALID_SOCKET;
		return false;
	}
	running_ = true;
	thread_ = std::thread(&DiscoveryResponder::run, this);
	return true;
}

void DiscoveryResponder::stop()
{
	if (!running_.exchange(false))
		return;
	if (thread_.joinable())
		thread_.join();
	socket_close(sock_);
	sock_ = VCAST_INVALID_SOCKET;
}

void DiscoveryResponder::run()
{
	while (running_) {
		if (wait_readable(sock_, 250) <= 0)
			continue;
		char buf[512];
		sockaddr_in from{};
		socklen_t from_len = sizeof(from);
		int n = (int)recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
		if (n < 6 || memcmp(buf, "VCAST?", 6) != 0)
			continue;
		for (auto &entry : provider_()) {
			std::string reply = "VCAST!\n{\"name\":\"" + json_escape(entry.first) +
					    "\",\"port\":" + std::to_string(entry.second) + ",\"version\":1}";
			sendto(sock_, reply.data(), (int)reply.size(), 0, reinterpret_cast<sockaddr *>(&from),
			       from_len);
		}
	}
}

} // namespace vcast
