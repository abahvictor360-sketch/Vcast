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

#include "cast-manager.h"
#include "http-server.h"
#include "net-util.h"
#include "vcast-output.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <cmath>

namespace vcast {

namespace {

const char *pick_audio_encoder()
{
	static const char *candidates[] = {"ffmpeg_aac", "CoreAudio_AAC", "libfdk_aac"};
	for (const char *id : candidates) {
		if (obs_get_encoder_codec(id))
			return id;
	}
	return nullptr;
}

void scaled_size(Resolution res, uint32_t &width, uint32_t &height)
{
	obs_video_info ovi = {};
	width = height = 0;
	if (res == Resolution::Canvas || !obs_get_video_info(&ovi) || ovi.output_height == 0)
		return;

	uint32_t target = res == Resolution::P1080 ? 1080 : res == Resolution::P720 ? 720 : 480;
	if (target >= ovi.output_height)
		return; // never upscale
	double aspect = (double)ovi.output_width / (double)ovi.output_height;
	height = target;
	width = (uint32_t)std::lround(target * aspect) & ~1u;
}

} // namespace

CastManager &CastManager::instance()
{
	static CastManager manager;
	return manager;
}

bool CastManager::ensure_encoders(std::string *error)
{
	std::string video_id = settings_.video_encoder;
	if (!obs_get_encoder_codec(video_id.c_str()))
		video_id = "obs_x264";

	if (video_encoder_ && video_id != obs_encoder_get_id(video_encoder_)) {
		obs_encoder_release(video_encoder_);
		video_encoder_ = nullptr;
	}

	obs_data_t *vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", settings_.video_bitrate);
	/* Short GOP: TVs and late joiners can start decoding within a second. */
	obs_data_set_int(vs, "keyint_sec", 1);
	obs_data_set_string(vs, "profile", "high");
	obs_data_set_int(vs, "bf", 0);
	obs_data_set_string(vs, "preset", "veryfast");
	obs_data_set_string(vs, "tune", "zerolatency");

	if (!video_encoder_)
		video_encoder_ = obs_video_encoder_create(video_id.c_str(), "vcast_video", vs, nullptr);
	else
		obs_encoder_update(video_encoder_, vs);
	obs_data_release(vs);

	if (!video_encoder_) {
		if (error)
			*error = obs_module_text("Vcast.Error.VideoEncoder");
		return false;
	}

	uint32_t width, height;
	scaled_size(settings_.resolution, width, height);
	obs_encoder_set_scaled_size(video_encoder_, width, height);
	obs_encoder_set_video(video_encoder_, obs_get_video());

	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", settings_.audio_bitrate);
	if (!audio_encoder_) {
		const char *audio_id = pick_audio_encoder();
		if (audio_id)
			audio_encoder_ = obs_audio_encoder_create(audio_id, "vcast_audio", as, 0, nullptr);
	} else {
		obs_encoder_update(audio_encoder_, as);
	}
	obs_data_release(as);

	if (!audio_encoder_) {
		if (error)
			*error = obs_module_text("Vcast.Error.AudioEncoder");
		return false;
	}
	obs_encoder_set_audio(audio_encoder_, obs_get_audio());
	return true;
}

bool CastManager::start_sharing(std::string *error)
{
	if (!output_) {
		output_ = obs_output_create(VCAST_OUTPUT_ID, "vcast_output", nullptr, nullptr);
		if (!output_) {
			if (error)
				*error = "Could not create the Vcast output";
			return false;
		}
	}
	if (obs_output_active(output_))
		return true;

	if (!ensure_encoders(error))
		return false;

	obs_data_t *os = obs_data_create();
	obs_data_set_int(os, "port", settings_.port);
	obs_data_set_int(os, "port_attempts", 10);
	obs_output_update(output_, os);
	obs_data_release(os);

	obs_output_set_video_encoder(output_, video_encoder_);
	obs_output_set_audio_encoder(output_, audio_encoder_, 0);

	if (!obs_output_start(output_)) {
		const char *last = obs_output_get_last_error(output_);
		if (error)
			*error = last && *last ? last : obs_module_text("Vcast.Error.StartFailed");
		return false;
	}
	return true;
}

void CastManager::stop_sharing()
{
	if (output_ && obs_output_active(output_))
		obs_output_stop(output_);
}

bool CastManager::sharing() const
{
	return output_ && obs_output_active(output_);
}

uint16_t CastManager::port() const
{
	return vcast_output_port(output_);
}

size_t CastManager::client_count() const
{
	return vcast_output_client_count(output_);
}

std::string CastManager::stream_url(const std::string &device_ip) const
{
	uint16_t p = port();
	if (!p)
		return std::string();
	std::string ip = device_ip.empty() ? std::string() : local_ip_for(device_ip);
	if (ip.empty())
		ip = primary_local_ip();
	return "http://" + ip + ":" + std::to_string(p) + kStreamPath;
}

bool CastManager::cast_to(const DlnaDevice &device, std::string *error)
{
	std::string url = stream_url(device.ip);
	if (url.empty()) {
		if (error)
			*error = obs_module_text("Vcast.Error.NotSharing");
		return false;
	}

	DlnaDevice previous;
	if (casting(&previous) && previous.udn != device.udn)
		dlna_stop(previous, nullptr);

	if (!dlna_play(device, url, settings_.title, error))
		return false;

	std::lock_guard<std::mutex> lock(device_mutex_);
	device_ = device;
	casting_ = true;
	return true;
}

void CastManager::stop_cast()
{
	DlnaDevice device;
	{
		std::lock_guard<std::mutex> lock(device_mutex_);
		if (!casting_)
			return;
		device = device_;
		casting_ = false;
	}
	std::string error;
	if (!dlna_stop(device, &error))
		obs_log(LOG_WARNING, "Could not stop '%s': %s", device.name.c_str(), error.c_str());
}

bool CastManager::casting(DlnaDevice *device) const
{
	std::lock_guard<std::mutex> lock(device_mutex_);
	if (casting_ && device)
		*device = device_;
	return casting_;
}

void CastManager::shutdown()
{
	stop_cast();
	if (output_) {
		if (obs_output_active(output_))
			obs_output_force_stop(output_);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (video_encoder_) {
		obs_encoder_release(video_encoder_);
		video_encoder_ = nullptr;
	}
	if (audio_encoder_) {
		obs_encoder_release(audio_encoder_);
		audio_encoder_ = nullptr;
	}
}

} // namespace vcast
