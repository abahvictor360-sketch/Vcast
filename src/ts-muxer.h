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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vcast {

/*
 * Minimal live MPEG-2 Transport Stream muxer for one H.264 video stream and
 * one AAC audio stream. MPEG-TS over HTTP is the format smart TVs (DLNA
 * renderers), VLC, Kodi and most media players handle best for live input.
 *
 * Timestamps are in 90 kHz units.
 */
class TsMuxer {
public:
	static constexpr size_t kPacketSize = 188;

	TsMuxer();

	void reset();

	/* SPS/PPS in Annex-B form; re-sent in front of every keyframe. */
	void set_video_headers(const uint8_t *data, size_t size);

	/* AudioSpecificConfig from the AAC encoder (may be empty), with fallbacks. */
	void set_audio_format(int sample_rate, int channels, const uint8_t *asc, size_t asc_size);

	void set_has_audio(bool has_audio) { has_audio_ = has_audio; }

	/* Appends whole 188-byte TS packets to out. */
	void write_video(const uint8_t *data, size_t size, int64_t pts, int64_t dts, bool keyframe,
			 std::vector<uint8_t> &out);
	void write_audio(const uint8_t *data, size_t size, int64_t pts, std::vector<uint8_t> &out);

private:
	void write_psi(std::vector<uint8_t> &out);
	void write_section(uint16_t pid, uint8_t &cc, const std::vector<uint8_t> &section, std::vector<uint8_t> &out);
	void write_pes(uint16_t pid, uint8_t &cc, uint8_t stream_id, const uint8_t *payload, size_t payload_size,
		       int64_t pts, int64_t dts, bool write_dts, bool write_pcr, int64_t pcr, bool random_access,
		       std::vector<uint8_t> &out);

	std::vector<uint8_t> video_headers_;
	std::vector<uint8_t> scratch_;
	bool has_audio_ = true;

	int aac_object_type_ = 2; // AAC-LC
	int aac_freq_index_ = 3;  // 48 kHz
	int aac_channel_config_ = 2;

	uint8_t cc_pat_ = 0;
	uint8_t cc_pmt_ = 0;
	uint8_t cc_video_ = 0;
	uint8_t cc_audio_ = 0;
	int64_t last_pcr_ = -1;
};

} // namespace vcast
