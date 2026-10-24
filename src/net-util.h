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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET vcast_socket_t;
#define VCAST_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
typedef int vcast_socket_t;
#define VCAST_INVALID_SOCKET (-1)
#endif

namespace vcast {

struct LocalInterface {
	std::string name;
	std::string ip; // dotted IPv4
};

/* Must be called once before any socket is used (WSAStartup on Windows). */
bool net_init();
void net_shutdown();

void socket_close(vcast_socket_t s);
/* Shut the socket down in both directions so blocked calls on other threads return. */
void socket_shutdown(vcast_socket_t s);
void socket_set_timeouts(vcast_socket_t s, int recv_ms, int send_ms);
void socket_set_nosigpipe(vcast_socket_t s);

/* Sends everything or fails. */
bool socket_send_all(vcast_socket_t s, const void *data, size_t size);

/* TCP connect with a timeout. Returns VCAST_INVALID_SOCKET on failure. */
vcast_socket_t tcp_connect(const std::string &host, uint16_t port, int timeout_ms, std::string *error);

/* IPv4 interfaces that are up, not loopback and have an address. */
std::vector<LocalInterface> local_interfaces();

/* The local IPv4 address the OS would use to reach remote_ip (no traffic is sent). */
std::string local_ip_for(const std::string &remote_ip);

/* Best guess at the machine's LAN address, used when no target device is known. */
std::string primary_local_ip();

int last_socket_error();

} // namespace vcast
