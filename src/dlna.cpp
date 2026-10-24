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

#include "dlna.h"
#include "http-client.h"
#include "http-server.h"
#include "net-util.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <set>

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace vcast {

namespace {

const char *const kSsdpAddress = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string xml_unescape(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] != '&') {
			out += s[i];
			continue;
		}
		size_t semi = s.find(';', i);
		if (semi == std::string::npos) {
			out += s[i];
			continue;
		}
		std::string entity = s.substr(i + 1, semi - i - 1);
		if (entity == "amp")
			out += '&';
		else if (entity == "lt")
			out += '<';
		else if (entity == "gt")
			out += '>';
		else if (entity == "quot")
			out += '"';
		else if (entity == "apos")
			out += '\'';
		else if (!entity.empty() && entity[0] == '#') {
			long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
					    ? strtol(entity.c_str() + 2, nullptr, 16)
					    : strtol(entity.c_str() + 1, nullptr, 10);
			if (code > 0 && code < 128)
				out += (char)code;
		} else {
			out += s.substr(i, semi - i + 1);
		}
		i = semi;
	}
	return out;
}

std::string trim(const std::string &s)
{
	size_t first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return std::string();
	size_t last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

std::string ssdp_header(const std::string &response, const std::string &name)
{
	std::string lowered = lower(response);
	std::string key = "\n" + lower(name) + ":";
	size_t pos = lowered.find(key);
	if (pos == std::string::npos)
		return std::string();
	pos += key.size();
	size_t end = response.find('\n', pos);
	return trim(response.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

bool soap_call(const DlnaDevice &device, const std::string &action, const std::string &arguments, std::string *error)
{
	std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
			   "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
			   "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
			   action + " xmlns:u=\"" + device.service_type + "\">" + arguments + "</u:" + action +
			   "></s:Body></s:Envelope>";

	HttpResponse response;
	std::string err;
	bool ok = http_request("POST", device.control_url,
			       {{"Content-Type", "text/xml; charset=\"utf-8\""},
				{"SOAPAction", "\"" + device.service_type + "#" + action + "\""}},
			       body, response, 5000, &err);
	if (!ok) {
		if (error)
			*error = err;
		return false;
	}
	if (response.status != 200) {
		std::string code = xml_tag_text(response.body, "errorCode");
		std::string desc = xml_tag_text(response.body, "errorDescription");
		if (error) {
			*error = action + " failed (HTTP " + std::to_string(response.status) + ")";
			if (!code.empty())
				*error += ", UPnP error " + code;
			if (!desc.empty())
				*error += ": " + desc;
		}
		obs_log(LOG_WARNING, "%s on '%s' failed: HTTP %d %s %s", action.c_str(), device.name.c_str(),
			response.status, code.c_str(), desc.c_str());
		return false;
	}
	return true;
}

} // namespace

std::string xml_escape(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '\'':
			out += "&apos;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

std::string xml_tag_text(const std::string &xml, const std::string &tag, size_t from, size_t *end)
{
	size_t pos = from;
	while ((pos = xml.find('<', pos)) != std::string::npos) {
		size_t name_start = pos + 1;
		size_t name_end = xml.find_first_of(" \t\r\n/>", name_start);
		if (name_end == std::string::npos)
			break;
		std::string full_name = xml.substr(name_start, name_end - name_start);
		std::string local =
			full_name.substr(full_name.find(':') == std::string::npos ? 0 : full_name.find(':') + 1);
		if (local != tag) {
			pos = name_end;
			continue;
		}
		size_t open_end = xml.find('>', name_end);
		if (open_end == std::string::npos)
			break;
		if (xml[open_end - 1] == '/') { // <tag/>
			if (end)
				*end = open_end + 1;
			return std::string();
		}
		std::string close = "</" + full_name + ">";
		size_t close_pos = xml.find(close, open_end + 1);
		if (close_pos == std::string::npos)
			break;
		if (end)
			*end = close_pos + close.size();
		return trim(xml_unescape(xml.substr(open_end + 1, close_pos - open_end - 1)));
	}
	if (end)
		*end = std::string::npos;
	return std::string();
}

bool dlna_describe(const std::string &location, DlnaDevice &device, std::string *error)
{
	HttpResponse response;
	if (!http_request("GET", location, {}, std::string(), response, 3000, error))
		return false;
	if (response.status != 200) {
		if (error)
			*error = "device description returned HTTP " + std::to_string(response.status);
		return false;
	}

	const std::string &xml = response.body;
	device.location = location;
	device.name = xml_tag_text(xml, "friendlyName");
	device.manufacturer = xml_tag_text(xml, "manufacturer");
	device.model = xml_tag_text(xml, "modelName");
	device.udn = xml_tag_text(xml, "UDN");
	Url url;
	if (parse_url(location, url))
		device.ip = url.host;

	std::string base = xml_tag_text(xml, "URLBase");
	if (base.empty())
		base = location;

	/* Walk every <service> block in the tree: renderers are often embedded devices. */
	size_t pos = 0;
	for (;;) {
		size_t end = std::string::npos;
		std::string service = xml_tag_text(xml, "service", pos, &end);
		if (end == std::string::npos)
			break;
		std::string type = xml_tag_text(service, "serviceType");
		if (type.find(":service:AVTransport:") != std::string::npos) {
			device.service_type = type;
			device.control_url = resolve_url(base, xml_tag_text(service, "controlURL"));
			break;
		}
		pos = end;
	}

	if (device.name.empty())
		device.name = device.model.empty() ? device.ip : device.model;
	if (device.udn.empty())
		device.udn = location;

	if (!device.valid()) {
		if (error)
			*error = "'" + device.name + "' does not support media playback (no AVTransport service)";
		return false;
	}
	return true;
}

std::vector<DlnaDevice> dlna_discover(int timeout_ms, const std::string &unicast_ip)
{
	std::vector<vcast_socket_t> sockets;

	auto open_socket = [&](const std::string &iface_ip) {
		vcast_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == VCAST_INVALID_SOCKET)
			return;
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_port = 0;
		if (iface_ip.empty() || inet_pton(AF_INET, iface_ip.c_str(), &local.sin_addr) != 1)
			local.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
			socket_close(s);
			return;
		}
		int ttl = 4;
		setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
		if (!iface_ip.empty()) {
			in_addr iface{};
			inet_pton(AF_INET, iface_ip.c_str(), &iface);
			setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&iface, sizeof(iface));
		}
		sockets.push_back(s);
	};

	if (unicast_ip.empty()) {
		for (auto &iface : local_interfaces())
			open_socket(iface.ip);
	}
	if (sockets.empty())
		open_socket(std::string());

	std::string target_ip = unicast_ip.empty() ? kSsdpAddress : unicast_ip;
	sockaddr_in dest{};
	dest.sin_family = AF_INET;
	dest.sin_port = htons(kSsdpPort);
	if (inet_pton(AF_INET, target_ip.c_str(), &dest.sin_addr) != 1) {
		for (vcast_socket_t s : sockets)
			socket_close(s);
		return {};
	}

	const char *targets[] = {"urn:schemas-upnp-org:device:MediaRenderer:1",
				 "urn:schemas-upnp-org:service:AVTransport:1"};
	int mx = std::max(1, std::min(3, timeout_ms / 1000 - 1));

	auto send_search = [&]() {
		for (const char *st : targets) {
			std::string msg = std::string("M-SEARCH * HTTP/1.1\r\n"
						      "HOST: ") +
					  target_ip +
					  ":1900\r\n"
					  "MAN: \"ssdp:discover\"\r\n"
					  "MX: " +
					  std::to_string(mx) + "\r\nST: " + st +
					  "\r\nUSER-AGENT: Vcast/1.0 UPnP/1.1 DLNADOC/1.50\r\n\r\n";
			for (vcast_socket_t s : sockets)
				sendto(s, msg.c_str(), (int)msg.size(), 0, reinterpret_cast<sockaddr *>(&dest),
				       sizeof(dest));
		}
	};

	std::set<std::string> locations;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	auto resend_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
	bool resent = false;
	send_search();

	while (!sockets.empty()) {
		auto now = std::chrono::steady_clock::now();
		if (now >= deadline)
			break;
		if (!resent && now >= resend_at) {
			send_search(); // UDP is lossy, especially on Wi-Fi
			resent = true;
		}

		fd_set rfds;
		FD_ZERO(&rfds);
		vcast_socket_t max_fd = 0;
		for (vcast_socket_t s : sockets) {
			FD_SET(s, &rfds);
			max_fd = std::max(max_fd, s);
		}
		timeval tv{0, 100000};
		if (select((int)max_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
			continue;

		for (vcast_socket_t s : sockets) {
			if (!FD_ISSET(s, &rfds))
				continue;
			char buf[2048];
			sockaddr_in from{};
			socklen_t from_len = sizeof(from);
			int n = (int)recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from),
					      &from_len);
			if (n <= 0)
				continue;
			std::string response(buf, (size_t)n);
			std::string location = ssdp_header(response, "LOCATION");
			if (!location.empty())
				locations.insert(location);
		}
	}

	for (vcast_socket_t s : sockets)
		socket_close(s);

	/* Fetch all descriptions in parallel so one slow device doesn't hold up the list. */
	std::vector<std::future<DlnaDevice>> pending;
	for (const auto &location : locations) {
		pending.push_back(std::async(std::launch::async, [location]() {
			DlnaDevice device;
			std::string error;
			if (!dlna_describe(location, device, &error))
				obs_log(LOG_DEBUG, "Ignoring %s: %s", location.c_str(), error.c_str());
			return device;
		}));
	}

	std::vector<DlnaDevice> devices;
	std::set<std::string> seen;
	for (auto &f : pending) {
		DlnaDevice device = f.get();
		if (!device.valid() || !seen.insert(device.udn).second)
			continue;
		devices.push_back(device);
	}
	std::sort(devices.begin(), devices.end(),
		  [](const DlnaDevice &a, const DlnaDevice &b) { return lower(a.name) < lower(b.name); });

	obs_log(LOG_INFO, "Discovery found %zu renderer(s) from %zu SSDP response(s)", devices.size(),
		locations.size());
	return devices;
}

bool dlna_play(const DlnaDevice &device, const std::string &url, const std::string &title, std::string *error)
{
	std::string protocol_info = std::string("http-get:*:") + kStreamMimeType + ":" + kDlnaContentFeatures;
	std::string didl = "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
			   "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
			   "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
			   "<item id=\"vcast-live\" parentID=\"0\" restricted=\"1\">"
			   "<dc:title>" +
			   xml_escape(title) +
			   "</dc:title>"
			   "<upnp:class>object.item.videoItem</upnp:class>"
			   "<res protocolInfo=\"" +
			   protocol_info + "\">" + xml_escape(url) + "</res></item></DIDL-Lite>";

	std::string set_uri = "<InstanceID>0</InstanceID><CurrentURI>" + xml_escape(url) +
			      "</CurrentURI><CurrentURIMetaData>" + xml_escape(didl) + "</CurrentURIMetaData>";

	std::string err;
	if (!soap_call(device, "SetAVTransportURI", set_uri, &err)) {
		/* Some TVs refuse a new URI while something is playing; stop and retry once. */
		soap_call(device, "Stop", "<InstanceID>0</InstanceID>", nullptr);
		if (!soap_call(device, "SetAVTransportURI", set_uri, &err)) {
			if (error)
				*error = err;
			return false;
		}
	}

	if (!soap_call(device, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", &err)) {
		/* A few renderers start automatically and reject Play while transitioning. */
		obs_log(LOG_WARNING, "Play request was rejected by '%s' (%s); it may still start on its own",
			device.name.c_str(), err.c_str());
	}
	obs_log(LOG_INFO, "Casting %s to '%s'", url.c_str(), device.name.c_str());
	return true;
}

bool dlna_stop(const DlnaDevice &device, std::string *error)
{
	return soap_call(device, "Stop", "<InstanceID>0</InstanceID>", error);
}

} // namespace vcast
