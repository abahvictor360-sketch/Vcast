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

/*
 * Headless end-to-end test: runs libobs without the frontend, starts the
 * Vcast output with real encoders, discovers a (fake) DLNA TV and casts to it.
 * See tests/run-e2e.sh, which supplies the fake TV and checks what it received.
 *
 * Usage: vcast-e2e-test <obs-plugin-dir> <obs-plugin-data-dir> <seconds>
 */

#include "cast-manager.h"
#include "dlna.h"
#include "net-util.h"
#include "vcast-output.h"

#include <obs-module.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

extern "C" const char *obs_module_text(const char *lookup)
{
	return lookup;
}

extern "C" obs_module_t *obs_current_module(void)
{
	return nullptr;
}

static bool load_module(const std::string &bin_dir, const std::string &data_dir, const char *name)
{
	obs_module_t *module = nullptr;
	std::string bin = bin_dir + "/" + name + ".so";
	std::string data = data_dir + "/" + name;
	if (obs_open_module(&module, bin.c_str(), data.c_str()) != MODULE_SUCCESS || !obs_init_module(module)) {
		fprintf(stderr, "failed to load module %s\n", name);
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <obs-plugin-dir> <obs-plugin-data-dir> <seconds>\n", argv[0]);
		return 2;
	}
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
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed\n");
		return 1;
	}
	obs_audio_info oai = {48000, SPEAKERS_STEREO};
	if (!obs_reset_audio(&oai))
		return 1;

	if (!load_module(argv[1], argv[2], "obs-x264") || !load_module(argv[1], argv[2], "obs-ffmpeg") ||
	    !load_module(argv[1], argv[2], "image-source"))
		return 1;
	obs_post_load_modules();
	vcast_register_output();

	/* Something moving to encode: a colour source plus an audible test tone would be nicer,
	 * but a static colour is enough to exercise the encoders, muxer and server. */
	obs_data_t *cs = obs_data_create();
	obs_data_set_int(cs, "color", 0xFF3366CC);
	obs_data_set_int(cs, "width", 1280);
	obs_data_set_int(cs, "height", 720);
	obs_source_t *color = obs_source_create("color_source_v3", "color", cs, nullptr);
	obs_data_release(cs);
	obs_set_output_source(0, color);

	auto &manager = vcast::CastManager::instance();
	manager.settings().port = 18090;
	manager.settings().video_bitrate = 2500;
	manager.settings().resolution = vcast::Resolution::P480;
	manager.settings().title = "Vcast <e2e> & test";

	std::string error;
	if (!manager.start_sharing(&error)) {
		fprintf(stderr, "start_sharing failed: %s\n", error.c_str());
		return 1;
	}
	printf("SHARING %s\n", manager.stream_url().c_str());
	fflush(stdout);

	auto devices = vcast::dlna_discover(3000);
	printf("MULTICAST_FOUND %zu\n", devices.size());
	if (devices.empty())
		devices = vcast::dlna_discover(2000, "127.0.0.1");
	printf("FOUND %zu\n", devices.size());

	int rc = 1;
	for (auto &d : devices) {
		printf("DEVICE name='%s' ip=%s control=%s type=%s\n", d.name.c_str(), d.ip.c_str(),
		       d.control_url.c_str(), d.service_type.c_str());
		if (d.name != "Vcast Fake TV")
			continue;
		if (!manager.cast_to(d, &error)) {
			fprintf(stderr, "cast_to failed: %s\n", error.c_str());
			break;
		}
		printf("CASTING\n");
		fflush(stdout);
		for (int i = 0; i < seconds; i++) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			printf("viewers=%zu\n", manager.client_count());
			fflush(stdout);
		}
		manager.stop_cast();
		rc = 0;
		break;
	}

	manager.stop_sharing();
	manager.shutdown();
	obs_set_output_source(0, nullptr);
	obs_source_release(color);
	obs_shutdown();
	printf("DONE rc=%d leaks=%ld\n", rc, bnum_allocs());
	return rc;
}
