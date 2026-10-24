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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcast {

class Receiver;

/*
 * Demuxes a live container byte stream (WebM/Matroska or fragmented MP4, as
 * produced by a browser's MediaRecorder) on its own thread and feeds the
 * packets to a StreamDecoder. Bytes are pushed as they arrive from the network.
 */
class ContainerIngest {
public:
	ContainerIngest(std::shared_ptr<Receiver> receiver, uint64_t session, std::string label);
	~ContainerIngest();

	ContainerIngest(const ContainerIngest &) = delete;
	ContainerIngest &operator=(const ContainerIngest &) = delete;

	void start();
	/* Returns false if the ingest has stopped (e.g. unsupported format). */
	bool push(const uint8_t *data, size_t size);
	/* Signals end of stream and waits for the demux thread. */
	void finish();

	bool running() const { return !stopped_; }

private:
	static int read_packet(void *opaque, uint8_t *buf, int buf_size);
	void run();

	std::shared_ptr<Receiver> receiver_;
	uint64_t session_;
	std::string label_;
	std::thread thread_;

	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::vector<uint8_t>> chunks_;
	size_t chunk_offset_ = 0;
	size_t queued_ = 0;
	bool eof_ = false;
	std::atomic<bool> stopped_{false};
};

} // namespace vcast
