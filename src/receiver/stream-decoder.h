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

#include <obs.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

struct SwsContext;

namespace vcast {

/*
 * Decodes the compressed audio/video a sender pushes (H.264/H.265/VP8/VP9/AV1
 * video; AAC incl. AAC-ELD, ALAC, Opus audio) and hands the frames to an OBS
 * async source. Video and audio may be fed from different threads.
 *
 * Timestamps are in microseconds in the sender's clock (any origin). They are
 * mapped onto the OBS clock so that the first packet plays "now", and the
 * mapping is re-anchored if the sender drifts too far, which keeps latency low.
 */
class StreamDecoder {
public:
	explicit StreamDecoder(obs_source_t *source);
	~StreamDecoder();

	StreamDecoder(const StreamDecoder &) = delete;
	StreamDecoder &operator=(const StreamDecoder &) = delete;

	/* extradata is optional (e.g. avcC, AudioSpecificConfig, ALAC cookie). */
	bool configure_video(AVCodecID codec, const uint8_t *extradata, size_t extradata_size);
	bool configure_audio(AVCodecID codec, int sample_rate, int channels, const uint8_t *extradata,
			     size_t extradata_size);

	/* pts_us < 0 means "present immediately". */
	void decode_video(const uint8_t *data, size_t size, int64_t pts_us);
	void decode_audio(const uint8_t *data, size_t size, int64_t pts_us);

	/* Fixed picture size for the OBS source, e.g. 1920x1080: every cast is
	 * scaled to fit inside it, with black bars. 0x0 keeps the device's size. */
	void set_output_size(uint32_t width, uint32_t height);

	/* Drops decoder state for a new sender. clear_picture blanks the source. */
	void reset(bool clear_picture);

	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	uint64_t video_frames() const { return video_frames_; }
	uint64_t audio_frames() const { return audio_frames_; }

private:
	struct Codec {
		std::mutex mutex;
		AVCodecContext *ctx = nullptr;
		AVCodecID id = AV_CODEC_ID_NONE;
		std::vector<uint8_t> extradata;
		int sample_rate = 0;
		int channels = 0;
		bool failed = false;
	};

	bool open_codec(Codec &codec, AVCodecID id, const uint8_t *extradata, size_t extradata_size, int sample_rate,
			int channels);
	static void close_codec(Codec &codec);
	uint64_t map_timestamp(int64_t pts_us);
	void output_video(AVFrame *frame, uint64_t ts);
	void output_frame(AVFrame *frame, uint64_t ts);
	void output_audio(AVFrame *frame, uint64_t ts);
	AVFrame *fit_to_output(AVFrame *frame, uint32_t out_w, uint32_t out_h, bool full_range);

	obs_source_t *source_;
	Codec video_;
	Codec audio_;
	AVPacket *video_packet_ = nullptr;
	AVPacket *audio_packet_ = nullptr;
	AVFrame *video_frame_ = nullptr;
	AVFrame *audio_frame_ = nullptr;

	/* Letterboxing into the fixed output size; guarded by video_.mutex. */
	std::atomic<uint32_t> out_width_{1920};
	std::atomic<uint32_t> out_height_{1080};
	SwsContext *sws_ = nullptr;
	AVFrame *scaled_frame_ = nullptr;
	int scaled_src_w_ = 0;
	int scaled_src_h_ = 0;
	int scaled_src_fmt_ = -1;
	bool scaled_full_range_ = false;

	std::mutex clock_mutex_;
	bool anchored_ = false;
	int64_t anchor_pts_us_ = 0;
	uint64_t anchor_obs_ns_ = 0;

	std::atomic<uint32_t> width_{0};
	std::atomic<uint32_t> height_{0};
	std::atomic<uint64_t> video_frames_{0};
	std::atomic<uint64_t> audio_frames_{0};
	bool warned_format_ = false;
};

} // namespace vcast
