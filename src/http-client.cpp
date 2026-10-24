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

#include "http-client.h"
#include "net-util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace vcast {

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string trim(const std::string &s)
{
	size_t first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return std::string();
	size_t last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

bool decode_chunked(const std::string &in, std::string &out)
{
	size_t pos = 0;
	out.clear();
	while (pos < in.size()) {
		size_t line_end = in.find("\r\n", pos);
		if (line_end == std::string::npos)
			return false;
		size_t len = strtoul(in.substr(pos, line_end - pos).c_str(), nullptr, 16);
		pos = line_end + 2;
		if (len == 0)
			return true;
		if (pos + len > in.size())
			return false;
		out.append(in, pos, len);
		pos += len + 2;
	}
	return false;
}

} // namespace

bool parse_url(const std::string &url, Url &out)
{
	const std::string scheme = "http://";
	if (lower(url.substr(0, scheme.size())) != scheme)
		return false;
	std::string rest = url.substr(scheme.size());
	size_t slash = rest.find('/');
	std::string authority = rest.substr(0, slash);
	out.path = slash == std::string::npos ? "/" : rest.substr(slash);

	size_t colon = authority.rfind(':');
	if (colon != std::string::npos) {
		out.host = authority.substr(0, colon);
		int port = atoi(authority.c_str() + colon + 1);
		if (port <= 0 || port > 65535)
			return false;
		out.port = (uint16_t)port;
	} else {
		out.host = authority;
		out.port = 80;
	}
	return !out.host.empty();
}

std::string resolve_url(const std::string &base, const std::string &relative)
{
	if (lower(relative.substr(0, 7)) == "http://")
		return relative;
	Url b;
	if (!parse_url(base, b))
		return relative;
	std::string origin = "http://" + b.host + ":" + std::to_string(b.port);
	if (!relative.empty() && relative[0] == '/')
		return origin + relative;
	std::string dir = b.path.substr(0, b.path.rfind('/') + 1);
	if (dir.empty())
		dir = "/";
	return origin + dir + relative;
}

bool http_request(const std::string &method, const std::string &url,
		  const std::vector<std::pair<std::string, std::string>> &headers, const std::string &body,
		  HttpResponse &response, int timeout_ms, std::string *error)
{
	Url u;
	if (!parse_url(url, u)) {
		if (error)
			*error = "invalid URL: " + url;
		return false;
	}

	vcast_socket_t s = tcp_connect(u.host, u.port, timeout_ms, error);
	if (s == VCAST_INVALID_SOCKET)
		return false;
	socket_set_timeouts(s, timeout_ms, timeout_ms);

	std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
			  "\r\nConnection: close\r\nUser-Agent: Vcast/1.0 UPnP/1.0 DLNADOC/1.50\r\n";
	for (auto &h : headers)
		req += h.first + ": " + h.second + "\r\n";
	if (!body.empty() || method == "POST")
		req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	req += "\r\n";
	req += body;

	if (!socket_send_all(s, req.data(), req.size())) {
		socket_close(s);
		if (error)
			*error = "failed to send request to " + u.host;
		return false;
	}

	std::string raw;
	char buf[4096];
	size_t header_end = std::string::npos;
	long long content_length = -1;
	for (;;) {
		int n = (int)recv(s, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		raw.append(buf, (size_t)n);
		if (header_end == std::string::npos) {
			header_end = raw.find("\r\n\r\n");
			if (header_end != std::string::npos) {
				std::string lowered = lower(raw.substr(0, header_end));
				size_t cl = lowered.find("\r\ncontent-length:");
				if (cl != std::string::npos)
					content_length = atoll(lowered.c_str() + cl + 17);
			}
		}
		if (header_end != std::string::npos && content_length >= 0 &&
		    raw.size() >= header_end + 4 + (size_t)content_length)
			break;
		if (raw.size() > 4 * 1024 * 1024)
			break;
	}
	socket_close(s);

	if (header_end == std::string::npos) {
		if (error)
			*error = "no response from " + u.host;
		return false;
	}

	std::string head = raw.substr(0, header_end);
	size_t line_end = head.find("\r\n");
	std::string status_line = head.substr(0, line_end);
	size_t sp = status_line.find(' ');
	response.status = sp == std::string::npos ? 0 : atoi(status_line.c_str() + sp + 1);
	response.headers.clear();

	size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
	while (pos < head.size()) {
		size_t end = head.find("\r\n", pos);
		if (end == std::string::npos)
			end = head.size();
		std::string line = head.substr(pos, end - pos);
		size_t colon = line.find(':');
		if (colon != std::string::npos)
			response.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
		pos = end + 2;
	}

	std::string payload = raw.substr(header_end + 4);
	if (content_length >= 0 && payload.size() > (size_t)content_length)
		payload.resize((size_t)content_length);
	auto te = response.headers.find("transfer-encoding");
	if (te != response.headers.end() && lower(te->second).find("chunked") != std::string::npos) {
		std::string decoded;
		decode_chunked(payload, decoded);
		payload = decoded;
	}
	response.body = payload;
	return true;
}

} // namespace vcast
