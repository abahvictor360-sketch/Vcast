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

#include "stream-decoder.h"

#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace vcast {

namespace {

/* Re-anchor the clock when a packet would play this far from "now". */
constexpr int64_t kMaxEarlyNs = 1000000000LL;
constexpr int64_t kMaxLateNs = 1000000000LL;

video_format obs_format(AVPixelFormat fmt)
{
	switch (fmt) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUV422P10LE:
		return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUVA420P:
		return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGR0:
		return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_GRAY8:
		return VIDEO_FORMAT_Y800;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

audio_format obs_audio_format(AVSampleFormat fmt)
{
	switch (fmt) {
	case AV_SAMPLE_FMT_U8:
		return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16:
		return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:
		return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT:
		return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

speaker_layout obs_speakers(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

} // namespace

StreamDecoder::StreamDecoder(obs_source_t *source) : source_(source)
{
	video_packet_ = av_packet_alloc();
	audio_packet_ = av_packet_alloc();
	video_frame_ = av_frame_alloc();
	audio_frame_ = av_frame_alloc();
}

StreamDecoder::~StreamDecoder()
{
	close_codec(video_);
	close_codec(audio_);
	av_packet_free(&video_packet_);
	av_packet_free(&audio_packet_);
	av_frame_free(&video_frame_);
	av_frame_free(&audio_frame_);
	av_frame_free(&scaled_frame_);
	sws_freeContext(sws_);
}

void StreamDecoder::set_output_size(uint32_t width, uint32_t height)
{
	if (!width || !height || width > 7680 || height > 4320)
		width = height = 0;
	/* Keep the chroma planes whole. */
	out_width_ = width & ~1u;
	out_height_ = height & ~1u;
}

/* Scales frame to fit inside out_w x out_h, centred on black. The result
 * keeps the source's YUV range so the colour conversion stays unchanged. */
AVFrame *StreamDecoder::fit_to_output(AVFrame *frame, uint32_t out_w, uint32_t out_h, bool full_range)
{
	if (frame->width <= 0 || frame->height <= 0)
		return nullptr;

	bool reconfigure = !scaled_frame_ || scaled_frame_->width != (int)out_w ||
			   scaled_frame_->height != (int)out_h || scaled_src_w_ != frame->width ||
			   scaled_src_h_ != frame->height || scaled_src_fmt_ != frame->format ||
			   scaled_full_range_ != full_range;
	if (!reconfigure)
		av_frame_make_writable(scaled_frame_);

	if (reconfigure) {
		av_frame_free(&scaled_frame_);
		scaled_frame_ = av_frame_alloc();
		if (!scaled_frame_)
			return nullptr;
		scaled_frame_->format = AV_PIX_FMT_YUV420P;
		scaled_frame_->width = (int)out_w;
		scaled_frame_->height = (int)out_h;
		if (av_frame_get_buffer(scaled_frame_, 32) < 0) {
			av_frame_free(&scaled_frame_);
			return nullptr;
		}
		scaled_src_w_ = frame->width;
		scaled_src_h_ = frame->height;
		scaled_src_fmt_ = frame->format;
		scaled_full_range_ = full_range;
	}

	/* Fit the picture inside the output, keeping its shape. */
	double scale = std::min((double)out_w / frame->width, (double)out_h / frame->height);
	int fit_w = std::max(2, (int)(frame->width * scale + 0.5) & ~1);
	int fit_h = std::max(2, (int)(frame->height * scale + 0.5) & ~1);
	fit_w = std::min(fit_w, (int)out_w);
	fit_h = std::min(fit_h, (int)out_h);
	int x = ((int)out_w - fit_w) / 2 & ~1;
	int y = ((int)out_h - fit_h) / 2 & ~1;

	sws_ = sws_getCachedContext(sws_, frame->width, frame->height, (AVPixelFormat)frame->format, fit_w, fit_h,
				    AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (!sws_)
		return nullptr;
	/* Same matrix in and out: only the size changes, not the colours. */
	const int *coeffs = sws_getCoefficients(SWS_CS_ITU709);
	sws_setColorspaceDetails(sws_, coeffs, full_range, coeffs, full_range, 0, 1 << 16, 1 << 16);

	if (reconfigure) {
		/* Black borders; only redrawn when the layout changes. */
		uint8_t black_y = full_range ? 0 : 16;
		for (int row = 0; row < (int)out_h; row++)
			memset(scaled_frame_->data[0] + (size_t)row * scaled_frame_->linesize[0], black_y, out_w);
		for (int plane = 1; plane < 3; plane++)
			for (int row = 0; row < (int)out_h / 2; row++)
				memset(scaled_frame_->data[plane] + (size_t)row * scaled_frame_->linesize[plane], 128,
				       out_w / 2);
	}

	uint8_t *dst[4] = {
		scaled_frame_->data[0] + (size_t)y * scaled_frame_->linesize[0] + x,
		scaled_frame_->data[1] + (size_t)(y / 2) * scaled_frame_->linesize[1] + x / 2,
		scaled_frame_->data[2] + (size_t)(y / 2) * scaled_frame_->linesize[2] + x / 2,
		nullptr,
	};
	if (sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, scaled_frame_->linesize) <= 0)
		return nullptr;

	scaled_frame_->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	/* Keep the matrix output_frame would have picked for the original size. */
	bool sd = frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
		  (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height < 720);
	scaled_frame_->colorspace = sd ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
	return scaled_frame_;
}

void StreamDecoder::close_codec(Codec &codec)
{
	avcodec_free_context(&codec.ctx);
	codec.id = AV_CODEC_ID_NONE;
	codec.extradata.clear();
	codec.failed = false;
}

bool StreamDecoder::open_codec(Codec &codec, AVCodecID id, const uint8_t *extradata, size_t extradata_size,
			       int sample_rate, int channels)
{
	bool same = codec.ctx && codec.id == id && codec.sample_rate == sample_rate && codec.channels == channels &&
		    codec.extradata.size() == extradata_size &&
		    (extradata_size == 0 || memcmp(codec.extradata.data(), extradata, extradata_size) == 0);
	if (same)
		return true;

	close_codec(codec);
	codec.id = id;
	codec.sample_rate = sample_rate;
	codec.channels = channels;
	if (extradata && extradata_size)
		codec.extradata.assign(extradata, extradata + extradata_size);

	const AVCodec *decoder = avcodec_find_decoder(id);
	if (!decoder) {
		obs_log(LOG_ERROR, "No FFmpeg decoder for %s", avcodec_get_name(id));
		codec.failed = true;
		return false;
	}

	codec.ctx = avcodec_alloc_context3(decoder);
	if (!codec.ctx) {
		codec.failed = true;
		return false;
	}
	if (extradata && extradata_size) {
		codec.ctx->extradata =
			static_cast<uint8_t *>(av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
		memcpy(codec.ctx->extradata, extradata, extradata_size);
		codec.ctx->extradata_size = (int)extradata_size;
	}
	if (decoder->type == AVMEDIA_TYPE_AUDIO) {
		codec.ctx->sample_rate = sample_rate;
		av_channel_layout_default(&codec.ctx->ch_layout, channels > 0 ? channels : 2);
	} else {
		/* Low latency: output each frame as soon as it is decoded. */
		codec.ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
		codec.ctx->thread_count = 0;
		codec.ctx->thread_type = FF_THREAD_SLICE;
	}

	int rc = avcodec_open2(codec.ctx, decoder, nullptr);
	if (rc < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(rc, err, sizeof(err));
		obs_log(LOG_ERROR, "Could not open %s decoder: %s", avcodec_get_name(id), err);
		avcodec_free_context(&codec.ctx);
		codec.failed = true;
		return false;
	}
	obs_log(LOG_INFO, "Decoder ready: %s%s", decoder->name,
		decoder->type == AVMEDIA_TYPE_AUDIO ? " (audio)" : " (video)");
	return true;
}

bool StreamDecoder::configure_video(AVCodecID codec, const uint8_t *extradata, size_t extradata_size)
{
	std::lock_guard<std::mutex> lock(video_.mutex);
	return open_codec(video_, codec, extradata, extradata_size, 0, 0);
}

bool StreamDecoder::configure_audio(AVCodecID codec, int sample_rate, int channels, const uint8_t *extradata,
				    size_t extradata_size)
{
	std::lock_guard<std::mutex> lock(audio_.mutex);
	return open_codec(audio_, codec, extradata, extradata_size, sample_rate, channels);
}

uint64_t StreamDecoder::map_timestamp(int64_t pts_us)
{
	uint64_t now = os_gettime_ns();
	if (pts_us < 0)
		return now;

	std::lock_guard<std::mutex> lock(clock_mutex_);
	if (anchored_) {
		int64_t ts = (int64_t)anchor_obs_ns_ + (pts_us - anchor_pts_us_) * 1000;
		int64_t diff = ts - (int64_t)now;
		if (diff <= kMaxEarlyNs && diff >= -kMaxLateNs && ts > 0)
			return (uint64_t)ts;
	}
	anchored_ = true;
	anchor_pts_us_ = pts_us;
	anchor_obs_ns_ = now;
	return now;
}

void StreamDecoder::decode_video(const uint8_t *data, size_t size, int64_t pts_us)
{
	std::lock_guard<std::mutex> lock(video_.mutex);
	if (!video_.ctx || !data || !size)
		return;

	av_packet_unref(video_packet_);
	if (av_new_packet(video_packet_, (int)size) < 0)
		return;
	memcpy(video_packet_->data, data, size);
	video_packet_->pts = pts_us;
	video_packet_->dts = pts_us;

	if (avcodec_send_packet(video_.ctx, video_packet_) < 0)
		return;
	while (avcodec_receive_frame(video_.ctx, video_frame_) == 0) {
		int64_t pts = video_frame_->best_effort_timestamp;
		output_video(video_frame_, map_timestamp(pts == AV_NOPTS_VALUE ? -1 : pts));
		av_frame_unref(video_frame_);
	}
}

void StreamDecoder::decode_audio(const uint8_t *data, size_t size, int64_t pts_us)
{
	std::lock_guard<std::mutex> lock(audio_.mutex);
	if (!audio_.ctx || !data || !size)
		return;

	av_packet_unref(audio_packet_);
	if (av_new_packet(audio_packet_, (int)size) < 0)
		return;
	memcpy(audio_packet_->data, data, size);
	audio_packet_->pts = pts_us;

	if (avcodec_send_packet(audio_.ctx, audio_packet_) < 0)
		return;
	bool first = true;
	while (avcodec_receive_frame(audio_.ctx, audio_frame_) == 0) {
		/* Several frames from one packet follow on directly; let OBS place them. */
		uint64_t ts = map_timestamp(first ? pts_us : -1);
		output_audio(audio_frame_, ts);
		first = false;
		av_frame_unref(audio_frame_);
	}
}

void StreamDecoder::output_video(AVFrame *frame, uint64_t ts)
{
	uint32_t out_w = out_width_, out_h = out_height_;
	if (out_w && out_h &&
	    (frame->width != (int)out_w || frame->height != (int)out_h ||
	     obs_format((AVPixelFormat)frame->format) == VIDEO_FORMAT_NONE)) {
		bool full = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P ||
			    frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P;
		/* The decoder's size, so the dock can still say what was cast. */
		uint32_t cast_w = (uint32_t)frame->width, cast_h = (uint32_t)frame->height;
		AVFrame *scaled = fit_to_output(frame, out_w, out_h, full);
		if (scaled) {
			output_frame(scaled, ts);
			width_ = cast_w;
			height_ = cast_h;
			return;
		}
	}
	output_frame(frame, ts);
}

void StreamDecoder::output_frame(AVFrame *frame, uint64_t ts)
{
	video_format format = obs_format((AVPixelFormat)frame->format);
	if (format == VIDEO_FORMAT_NONE) {
		if (!warned_format_) {
			obs_log(LOG_WARNING, "Unsupported decoded pixel format %s",
				av_get_pix_fmt_name((AVPixelFormat)frame->format));
			warned_format_ = true;
		}
		return;
	}

	obs_source_frame2 out = {};
	for (int i = 0; i < MAX_AV_PLANES && i < AV_NUM_DATA_POINTERS; i++) {
		out.data[i] = frame->data[i];
		out.linesize[i] = (uint32_t)(frame->linesize[i] > 0 ? frame->linesize[i] : 0);
	}
	out.width = (uint32_t)frame->width;
	out.height = (uint32_t)frame->height;
	out.timestamp = ts;
	out.format = format;

	bool full = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P ||
		    frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P;
	out.range = full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

	video_colorspace cs = VIDEO_CS_709;
	if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
		cs = VIDEO_CS_601;
	else if (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height < 720)
		cs = VIDEO_CS_601;
	video_format_get_parameters_for_format(cs, out.range, format, out.color_matrix, out.color_range_min,
					       out.color_range_max);

	width_ = out.width;
	height_ = out.height;
	video_frames_++;
	obs_source_output_video2(source_, &out);
}

void StreamDecoder::output_audio(AVFrame *frame, uint64_t ts)
{
	audio_format format = obs_audio_format((AVSampleFormat)frame->format);
	int channels = frame->ch_layout.nb_channels;
	speaker_layout speakers = obs_speakers(channels);
	if (format == AUDIO_FORMAT_UNKNOWN || speakers == SPEAKERS_UNKNOWN)
		return;

	obs_source_audio out = {};
	for (int i = 0; i < MAX_AV_PLANES && i < AV_NUM_DATA_POINTERS; i++)
		out.data[i] = frame->data[i];
	out.frames = (uint32_t)frame->nb_samples;
	out.speakers = speakers;
	out.format = format;
	out.samples_per_sec = (uint32_t)frame->sample_rate;
	out.timestamp = ts;
	audio_frames_++;
	obs_source_output_audio(source_, &out);
}

void StreamDecoder::reset(bool clear_picture)
{
	{
		std::lock_guard<std::mutex> lock(video_.mutex);
		close_codec(video_);
	}
	{
		std::lock_guard<std::mutex> lock(audio_.mutex);
		close_codec(audio_);
	}
	{
		std::lock_guard<std::mutex> lock(clock_mutex_);
		anchored_ = false;
	}
	width_ = height_ = 0;
	warned_format_ = false;
	if (clear_picture)
		obs_source_output_video(source_, nullptr);
}

} // namespace vcast
