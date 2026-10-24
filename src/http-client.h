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

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vcast {

struct Url {
	std::string host;
	uint16_t port = 80;
	std::string path = "/";
};

bool parse_url(const std::string &url, Url &out);

/* Resolves a possibly relative URL (as found in UPnP descriptions) against base. */
std::string resolve_url(const std::string &base, const std::string &relative);

struct HttpResponse {
	int status = 0;
	std::map<std::string, std::string> headers; // lower-case names
	std::string body;
};

/* Minimal blocking HTTP/1.1 client for talking to devices on the LAN. */
bool http_request(const std::string &method, const std::string &url,
		  const std::vector<std::pair<std::string, std::string>> &headers, const std::string &body,
		  HttpResponse &response, int timeout_ms, std::string *error);

} // namespace vcast
