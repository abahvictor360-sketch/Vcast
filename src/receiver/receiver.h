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

#pragma once

#include "stream-decoder.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace vcast {

/*
 * What every ingest path (AirPlay, Android app, browser) talks to: a single
 * "screen" that one sender at a time can cast to. A new sender replaces the
 * current one, like a TV switching to whoever cast last.
 */
class Receiver {
public:
	explicit Receiver(obs_source_t *source) : decoder_(source) {}
	virtual ~Receiver() = default;

	virtual std::string name() const = 0;
	/* Stable id used in URLs. */
	virtual std::string id() const = 0;

	/* Starts a session for a sender; stop is called if another sender takes over. */
	uint64_t begin_session(const std::string &kind, const std::string &device, std::function<void()> stop);
	bool is_active(uint64_t session) const;
	void end_session(uint64_t session);
	void set_device_name(uint64_t session, const std::string &device);

	/* "iPhone (AirPlay)" style description of the current sender, empty when idle. */
	std::string current_sender() const;

	/* Runs fn with the decoder if session is still the active sender and the
	 * OBS source still exists. Returns false if the sender should stop. */
	template<typename Fn> bool feed(uint64_t session, Fn &&fn)
	{
		std::shared_lock<std::shared_mutex> lock(alive_mutex_);
		if (!alive_ || !is_active(session))
			return false;
		fn(decoder_);
		return true;
	}

	/* Called when the OBS source is destroyed: stops the sender and makes all
	 * further feed() calls fail. Ingest threads may still hold the object. */
	void close();
	bool alive() const { return alive_; }

	void set_keep_last_frame(bool keep) { keep_last_frame_ = keep; }

private:
	StreamDecoder decoder_;
	mutable std::mutex mutex_;
	uint64_t next_session_ = 1;
	uint64_t active_ = 0;
	std::string kind_;
	std::string device_;
	std::function<void()> stop_;
	bool keep_last_frame_ = false;
	std::shared_mutex alive_mutex_;
	bool alive_ = true;
};

} // namespace vcast
