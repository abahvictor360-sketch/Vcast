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

#include "vcast-output.h"
#include "http-server.h"
#include "ts-muxer.h"

#include <plugin-support.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {

/* Start timestamps a little above zero so B-frame DTS and the PCR never go negative. */
constexpr int64_t kTimestampOffset = 126000; // 1.4 s in 90 kHz

struct VcastOutput {
	obs_output_t *output = nullptr;
	vcast::HttpStreamServer server;
	vcast::TsMuxer muxer;
	std::vector<uint8_t> buffer;
	std::mutex mutex;
	bool configured = false;
	bool waiting_for_keyframe = true;
	std::atomic<uint64_t> total_bytes{0};
};

int64_t to_90khz(int64_t ts, const encoder_packet *packet)
{
	if (packet->timebase_den <= 0)
		return 0;
	return ts * 90000 * packet->timebase_num / packet->timebase_den + kTimestampOffset;
}

const char *vcast_output_get_name(void *)
{
	return obs_module_text("Vcast.OutputName");
}

void *vcast_output_create(obs_data_t *, obs_output_t *output)
{
	auto *data = new VcastOutput();
	data->output = output;
	return data;
}

void vcast_output_destroy(void *ptr)
{
	auto *data = static_cast<VcastOutput *>(ptr);
	data->server.stop();
	delete data;
}

void vcast_output_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "port", 8090);
	obs_data_set_default_int(settings, "port_attempts", 10);
}

bool configure_muxer(VcastOutput *data)
{
	obs_encoder_t *venc = obs_output_get_video_encoder(data->output);
	if (!venc)
		return false;

	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	if (obs_encoder_get_extra_data(venc, &extra, &extra_size) && extra && extra_size)
		data->muxer.set_video_headers(extra, extra_size);

	obs_encoder_t *aenc = obs_output_get_audio_encoder(data->output, 0);
	data->muxer.set_has_audio(aenc != nullptr);
	if (aenc) {
		uint8_t *asc = nullptr;
		size_t asc_size = 0;
		obs_encoder_get_extra_data(aenc, &asc, &asc_size);
		audio_t *audio = obs_encoder_audio(aenc);
		int channels = audio ? (int)audio_output_get_channels(audio) : 2;
		data->muxer.set_audio_format((int)obs_encoder_get_sample_rate(aenc), channels, asc, asc_size);
	}
	return true;
}

bool vcast_output_start(void *ptr)
{
	auto *data = static_cast<VcastOutput *>(ptr);

	if (!obs_output_can_begin_data_capture(data->output, 0))
		return false;
	if (!obs_output_initialize_encoders(data->output, 0))
		return false;

	obs_data_t *settings = obs_output_get_settings(data->output);
	int port = (int)obs_data_get_int(settings, "port");
	int attempts = (int)obs_data_get_int(settings, "port_attempts");
	obs_data_release(settings);

	{
		std::lock_guard<std::mutex> lock(data->mutex);
		data->muxer.reset();
		data->configured = false;
		data->waiting_for_keyframe = true;
		data->total_bytes = 0;
	}

	std::string error;
	if (!data->server.start((uint16_t)port, attempts, &error)) {
		obs_log(LOG_ERROR, "%s", error.c_str());
		obs_output_set_last_error(data->output, error.c_str());
		return false;
	}

	if (!obs_output_begin_data_capture(data->output, 0)) {
		data->server.stop();
		return false;
	}
	obs_log(LOG_INFO, "Vcast output started");
	return true;
}

void vcast_output_stop(void *ptr, uint64_t)
{
	auto *data = static_cast<VcastOutput *>(ptr);
	obs_output_end_data_capture(data->output);
	if (!data->server.running())
		return;
	data->server.stop();
	obs_log(LOG_INFO, "Vcast output stopped (%llu bytes sent)", (unsigned long long)data->total_bytes.load());
}

void vcast_output_encoded_packet(void *ptr, encoder_packet *packet)
{
	auto *data = static_cast<VcastOutput *>(ptr);
	if (!packet) {
		obs_output_signal_stop(data->output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	std::lock_guard<std::mutex> lock(data->mutex);
	if (!data->configured) {
		if (!configure_muxer(data))
			return;
		data->configured = true;
	}

	data->buffer.clear();
	bool keyframe = false;
	if (packet->type == OBS_ENCODER_VIDEO) {
		keyframe = packet->keyframe;
		if (data->waiting_for_keyframe) {
			if (!keyframe)
				return;
			data->waiting_for_keyframe = false;
		}
		data->muxer.write_video(packet->data, packet->size, to_90khz(packet->pts, packet),
					to_90khz(packet->dts, packet), keyframe, data->buffer);
	} else if (packet->type == OBS_ENCODER_AUDIO) {
		if (data->waiting_for_keyframe)
			return;
		data->muxer.write_audio(packet->data, packet->size, to_90khz(packet->pts, packet), data->buffer);
	}

	if (!data->buffer.empty()) {
		data->server.broadcast(data->buffer.data(), data->buffer.size(), keyframe);
		data->total_bytes += data->buffer.size();
	}
}

uint64_t vcast_output_total_bytes(void *ptr)
{
	return static_cast<VcastOutput *>(ptr)->total_bytes.load();
}

} // namespace

void vcast_register_output()
{
	obs_output_info info = {};
	info.id = VCAST_OUTPUT_ID;
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	info.encoded_video_codecs = "h264";
	info.encoded_audio_codecs = "aac";
	info.get_name = vcast_output_get_name;
	info.create = vcast_output_create;
	info.destroy = vcast_output_destroy;
	info.start = vcast_output_start;
	info.stop = vcast_output_stop;
	info.encoded_packet = vcast_output_encoded_packet;
	info.get_defaults = vcast_output_defaults;
	info.get_total_bytes = vcast_output_total_bytes;
	obs_register_output(&info);
}

uint16_t vcast_output_port(obs_output_t *output)
{
	auto *data = output ? static_cast<VcastOutput *>(obs_obj_get_data(output)) : nullptr;
	return data && data->server.running() ? data->server.port() : 0;
}

size_t vcast_output_client_count(obs_output_t *output)
{
	auto *data = output ? static_cast<VcastOutput *>(obs_obj_get_data(output)) : nullptr;
	return data ? data->server.client_count() : 0;
}
