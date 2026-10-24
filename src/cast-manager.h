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

#include "dlna.h"
#include "vcast-settings.h"

#include <obs.h>

#include <mutex>
#include <string>

namespace vcast {

/*
 * Owns the Vcast output and its encoders, and remembers which TV we are
 * casting to. start/stop sharing must be called from the UI thread; the
 * cast_to/stop_cast calls block on the network and belong on a worker thread.
 */
class CastManager {
public:
	static CastManager &instance();

	Settings &settings() { return settings_; }

	/* Starts encoding and the HTTP stream server. */
	bool start_sharing(std::string *error);
	void stop_sharing();
	bool sharing() const;
	uint16_t port() const;
	size_t client_count() const;

	/* URL that a device at device_ip should use (picks the right local interface). */
	std::string stream_url(const std::string &device_ip = std::string()) const;

	/* Blocking: tells the TV to play our stream. Sharing must already be running. */
	bool cast_to(const DlnaDevice &device, std::string *error);
	/* Blocking: tells the current TV (if any) to stop. */
	void stop_cast();
	bool casting(DlnaDevice *device = nullptr) const;

	/* Stops everything and frees OBS objects. Call before OBS shuts down. */
	void shutdown();

private:
	CastManager() = default;
	bool ensure_encoders(std::string *error);

	Settings settings_;
	obs_output_t *output_ = nullptr;
	obs_encoder_t *video_encoder_ = nullptr;
	obs_encoder_t *audio_encoder_ = nullptr;

	mutable std::mutex device_mutex_;
	DlnaDevice device_;
	bool casting_ = false;
};

} // namespace vcast
