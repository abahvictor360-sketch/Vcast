/*
Vcast - cast to and from OBS on your local network
Copyright (C) 2026 Vcast contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Headless receiver test: runs libobs, creates a "Vcast Screen Receiver"
 * source and prints what it receives once per second. Test drivers (a fake
 * Android sender, a real browser) cast into it. See tests/run-receiver-e2e.sh.
 *
 * Usage: vcast-receiver-test <data-dir> <config-dir> <seconds>
 */

#include "net-util.h"
#include "receiver/receiver-source.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static std::string g_data_dir;
static std::string g_config_dir;

extern "C" const char *obs_module_text(const char *lookup)
{
	return lookup;
}

extern "C" obs_module_t *obs_current_module(void)
{
	return nullptr;
}

/* The real versions resolve paths through the loaded module; point them at the repo instead. */
extern "C" char *obs_find_module_file(obs_module_t *, const char *file)
{
	std::string path = g_data_dir + "/" + file;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return nullptr;
	fclose(f);
	return bstrdup(path.c_str());
}

extern "C" char *obs_module_get_config_path(obs_module_t *, const char *file)
{
	return bstrdup((g_config_dir + "/" + file).c_str());
}

static std::atomic<uint64_t> g_audio_frames{0};

static void audio_cb(void *, obs_source_t *, const struct audio_data *audio, bool muted)
{
	if (!muted)
		g_audio_frames += audio->frames;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <data-dir> <config-dir> <seconds>\n", argv[0]);
		return 2;
	}
	g_data_dir = argv[1];
	g_config_dir = argv[2];
	int seconds = atoi(argv[3]);

	if (!obs_startup("en-US", nullptr, nullptr))
		return 1;
	vcast::net_init();

	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = 1280;
	ovi.base_height = ovi.output_height = 720;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 1;
	obs_audio_info oai = {48000, SPEAKERS_STEREO};
	if (!obs_reset_audio(&oai))
		return 1;

	vcast_register_receiver_source();

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "receiver_name", "Vcast Test Receiver");
	obs_data_set_bool(settings, "airplay", false);
	obs_source_t *source = obs_source_create(VCAST_RECEIVER_SOURCE_ID, "receiver", settings, nullptr);
	obs_data_release(settings);
	if (!source) {
		fprintf(stderr, "could not create the receiver source\n");
		return 1;
	}
	obs_set_output_source(0, source);
	obs_source_add_audio_capture_callback(source, audio_cb, nullptr);

	printf("READY %s\n", vcast_receiver_web_url().c_str());
	fflush(stdout);

	for (int i = 0; i < seconds; i++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		printf("STATS t=%d width=%u height=%u audio_frames=%llu\n", i + 1, obs_source_get_width(source),
		       obs_source_get_height(source), (unsigned long long)g_audio_frames.load());
		fflush(stdout);
	}

	obs_source_remove_audio_capture_callback(source, audio_cb, nullptr);
	obs_set_output_source(0, nullptr);
	obs_source_release(source);
	vcast_receiver_shutdown();
	obs_shutdown();
	printf("DONE leaks=%ld\n", bnum_allocs());
	return 0;
}
