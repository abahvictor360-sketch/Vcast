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
#include <utility>
#include <vector>

namespace vcast {

enum class Resolution : int {
	Canvas = 0,
	P1080 = 1,
	P720 = 2,
	P480 = 3,
};

struct Settings {
	int port = 8090;
	std::string video_encoder = "obs_x264";
	int video_bitrate = 6000; // kbps
	int audio_bitrate = 160;  // kbps
	Resolution resolution = Resolution::Canvas;
	std::string title = "OBS Studio";
	std::string last_device_location;

	void load();
	void save() const;
};

/* (id, display name) of every usable H.264 encoder, software encoder first. */
std::vector<std::pair<std::string, std::string>> available_h264_encoders();

} // namespace vcast
