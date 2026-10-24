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

#include "vcast-settings.h"

#include <obs-module.h>
#include <util/platform.h>

#include <cstring>

namespace vcast {

namespace {

std::string config_file()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("settings.json");
	std::string result = path ? path : "";
	bfree(path);
	return result;
}

} // namespace

void Settings::load()
{
	std::string path = config_file();
	if (path.empty())
		return;
	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data)
		return;

	obs_data_set_default_int(data, "port", port);
	obs_data_set_default_string(data, "video_encoder", video_encoder.c_str());
	obs_data_set_default_int(data, "video_bitrate", video_bitrate);
	obs_data_set_default_int(data, "audio_bitrate", audio_bitrate);
	obs_data_set_default_int(data, "resolution", (int)resolution);
	obs_data_set_default_string(data, "title", title.c_str());

	port = (int)obs_data_get_int(data, "port");
	video_encoder = obs_data_get_string(data, "video_encoder");
	video_bitrate = (int)obs_data_get_int(data, "video_bitrate");
	audio_bitrate = (int)obs_data_get_int(data, "audio_bitrate");
	resolution = (Resolution)obs_data_get_int(data, "resolution");
	title = obs_data_get_string(data, "title");
	last_device_location = obs_data_get_string(data, "last_device_location");
	obs_data_release(data);

	if (port < 1024 || port > 65000)
		port = 8090;
	if (video_bitrate < 500)
		video_bitrate = 500;
	if (audio_bitrate < 64)
		audio_bitrate = 64;
}

void Settings::save() const
{
	std::string path = config_file();
	if (path.empty())
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "port", port);
	obs_data_set_string(data, "video_encoder", video_encoder.c_str());
	obs_data_set_int(data, "video_bitrate", video_bitrate);
	obs_data_set_int(data, "audio_bitrate", audio_bitrate);
	obs_data_set_int(data, "resolution", (int)resolution);
	obs_data_set_string(data, "title", title.c_str());
	obs_data_set_string(data, "last_device_location", last_device_location.c_str());
	obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
	obs_data_release(data);
}

std::vector<std::pair<std::string, std::string>> available_h264_encoders()
{
	std::vector<std::pair<std::string, std::string>> encoders;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		const char *codec = obs_get_encoder_codec(id);
		if (!codec || strcmp(codec, "h264") != 0)
			continue;
		uint32_t caps = obs_get_encoder_caps(id);
		if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const char *name = obs_encoder_get_display_name(id);
		std::pair<std::string, std::string> entry{id, name ? name : id};
		if (strcmp(id, "obs_x264") == 0)
			encoders.insert(encoders.begin(), entry);
		else
			encoders.push_back(entry);
	}
	return encoders;
}

} // namespace vcast
