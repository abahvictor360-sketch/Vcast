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

#include "net-util.h"

#include <cstring>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace vcast {

bool net_init()
{
#ifdef _WIN32
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
	return true;
#endif
}

void net_shutdown()
{
#ifdef _WIN32
	WSACleanup();
#endif
}

int last_socket_error()
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

void socket_close(vcast_socket_t s)
{
	if (s == VCAST_INVALID_SOCKET)
		return;
#ifdef _WIN32
	closesocket(s);
#else
	close(s);
#endif
}

void socket_shutdown(vcast_socket_t s)
{
	if (s == VCAST_INVALID_SOCKET)
		return;
#ifdef _WIN32
	shutdown(s, SD_BOTH);
#else
	shutdown(s, SHUT_RDWR);
#endif
}

void socket_set_timeouts(vcast_socket_t s, int recv_ms, int send_ms)
{
#ifdef _WIN32
	DWORD r = (DWORD)recv_ms, w = (DWORD)send_ms;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof(r));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&w, sizeof(w));
#else
	timeval r{recv_ms / 1000, (recv_ms % 1000) * 1000};
	timeval w{send_ms / 1000, (send_ms % 1000) * 1000};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w));
#endif
}

void socket_set_nosigpipe(vcast_socket_t s)
{
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	(void)s;
#endif
}

bool socket_send_all(vcast_socket_t s, const void *data, size_t size)
{
	const char *p = static_cast<const char *>(data);
	while (size > 0) {
#ifdef _WIN32
		int chunk = size > 0x10000000 ? 0x10000000 : (int)size;
		int n = send(s, p, chunk, 0);
#elif defined(MSG_NOSIGNAL)
		ssize_t n = send(s, p, size, MSG_NOSIGNAL);
#else
		ssize_t n = send(s, p, size, 0);
#endif
		if (n <= 0) {
#ifndef _WIN32
			if (n < 0 && errno == EINTR)
				continue;
#endif
			return false;
		}
		p += n;
		size -= (size_t)n;
	}
	return true;
}

static bool set_nonblocking(vcast_socket_t s, bool enable)
{
#ifdef _WIN32
	u_long mode = enable ? 1 : 0;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0)
		return false;
	flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return fcntl(s, F_SETFL, flags) == 0;
#endif
}

vcast_socket_t tcp_connect(const std::string &host, uint16_t port, int timeout_ms, std::string *error)
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *res = nullptr;
	std::string port_str = std::to_string(port);
	if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
		if (error)
			*error = "could not resolve " + host;
		return VCAST_INVALID_SOCKET;
	}

	vcast_socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s == VCAST_INVALID_SOCKET) {
		freeaddrinfo(res);
		if (error)
			*error = "socket() failed";
		return VCAST_INVALID_SOCKET;
	}
	socket_set_nosigpipe(s);
	set_nonblocking(s, true);

	int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
	freeaddrinfo(res);

	bool ok = rc == 0;
	if (!ok) {
#ifdef _WIN32
		bool in_progress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
		bool in_progress = errno == EINPROGRESS;
#endif
		if (in_progress) {
			fd_set wfds, efds;
			FD_ZERO(&wfds);
			FD_ZERO(&efds);
			FD_SET(s, &wfds);
			FD_SET(s, &efds);
			timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
			if (select((int)s + 1, nullptr, &wfds, &efds, &tv) > 0 && FD_ISSET(s, &wfds)) {
				int so_error = 0;
				socklen_t len = sizeof(so_error);
				getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);
				ok = so_error == 0;
			}
		}
	}

	if (!ok) {
		socket_close(s);
		if (error)
			*error = "could not connect to " + host + ":" + port_str;
		return VCAST_INVALID_SOCKET;
	}

	set_nonblocking(s, false);
	return s;
}

std::vector<LocalInterface> local_interfaces()
{
	std::vector<LocalInterface> result;
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buffer;
	ULONG rc;
	do {
		buffer.resize(size);
		rc = GetAdaptersAddresses(AF_INET,
					  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
					  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
	} while (rc == ERROR_BUFFER_OVERFLOW);
	if (rc != NO_ERROR)
		return result;

	for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			if (u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			char ip[INET_ADDRSTRLEN] = {};
			auto *sin = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
			inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
			if (strncmp(ip, "169.254.", 8) == 0)
				continue;
			char name[256] = {};
			WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), nullptr, nullptr);
			result.push_back({name, ip});
		}
	}
#else
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return result;
	for (ifaddrs *i = list; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr, ip, sizeof(ip));
		if (strncmp(ip, "169.254.", 8) == 0)
			continue;
		result.push_back({i->ifa_name, ip});
	}
	freeifaddrs(list);
#endif
	return result;
}

std::string local_ip_for(const std::string &remote_ip)
{
	vcast_socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == VCAST_INVALID_SOCKET)
		return std::string();

	sockaddr_in remote{};
	remote.sin_family = AF_INET;
	remote.sin_port = htons(1900);
	std::string ip;
	if (inet_pton(AF_INET, remote_ip.c_str(), &remote.sin_addr) == 1 &&
	    connect(s, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) == 0) {
		sockaddr_in local{};
		socklen_t len = sizeof(local);
		if (getsockname(s, reinterpret_cast<sockaddr *>(&local), &len) == 0) {
			char buf[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
			ip = buf;
		}
	}
	socket_close(s);
	if (ip == "0.0.0.0")
		ip.clear();
	return ip;
}

std::string primary_local_ip()
{
	/* The address used to reach a public IP is almost always the LAN address
	 * of the default route. Nothing is actually sent. */
	std::string ip = local_ip_for("8.8.8.8");
	if (!ip.empty())
		return ip;
	auto ifaces = local_interfaces();
	if (!ifaces.empty())
		return ifaces.front().ip;
	return "127.0.0.1";
}

} // namespace vcast
