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

#include <string>
#include <vector>

namespace vcast {

/* A UPnP/DLNA MediaRenderer (smart TV, streaming box, speaker, Kodi...). */
struct DlnaDevice {
	std::string udn;  // unique device id
	std::string name; // friendlyName
	std::string manufacturer;
	std::string model;
	std::string location;     // device description URL
	std::string ip;           // device address
	std::string control_url;  // AVTransport control URL (absolute)
	std::string service_type; // e.g. urn:schemas-upnp-org:service:AVTransport:1

	bool valid() const { return !control_url.empty(); }
};

/*
 * Discovers renderers with SSDP (the same mechanism phones use to find TVs
 * for "cast"/"play to"). Blocks for about timeout_ms. With unicast_ip set,
 * only that address is asked, which works even where multicast is blocked.
 */
std::vector<DlnaDevice> dlna_discover(int timeout_ms, const std::string &unicast_ip = std::string());

/* Builds a device from a description URL (for TVs that do not answer SSDP). */
bool dlna_describe(const std::string &location, DlnaDevice &device, std::string *error);

/* Tells the renderer to play url and starts playback. Blocking. */
bool dlna_play(const DlnaDevice &device, const std::string &url, const std::string &title, std::string *error);

/* Stops playback on the renderer. Blocking. */
bool dlna_stop(const DlnaDevice &device, std::string *error);

/* Helpers exposed for tests. */
std::string xml_escape(const std::string &s);
std::string xml_tag_text(const std::string &xml, const std::string &tag, size_t from = 0, size_t *end = nullptr);

} // namespace vcast
