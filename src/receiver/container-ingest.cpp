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

#include "container-ingest.h"
#include "receiver.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
}

namespace vcast {

namespace {

/* A sender this far behind is dropped rather than buffering forever. */
constexpr size_t kMaxQueuedBytes = 32 * 1024 * 1024;
constexpr int kAvioBufferSize = 64 * 1024;

} // namespace

ContainerIngest::ContainerIngest(std::shared_ptr<Receiver> receiver, uint64_t session, std::string label)
	: receiver_(std::move(receiver)),
	  session_(session),
	  label_(std::move(label))
{
}

ContainerIngest::~ContainerIngest()
{
	finish();
}

void ContainerIngest::start()
{
	thread_ = std::thread(&ContainerIngest::run, this);
}

bool ContainerIngest::push(const uint8_t *data, size_t size)
{
	if (stopped_)
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (queued_ > kMaxQueuedBytes) {
		obs_log(LOG_WARNING, "[%s] receiver is falling behind, dropping the sender", label_.c_str());
		eof_ = true;
		cv_.notify_all();
		return false;
	}
	chunks_.emplace_back(data, data + size);
	queued_ += size;
	cv_.notify_all();
	return true;
}

void ContainerIngest::finish()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		eof_ = true;
		cv_.notify_all();
	}
	if (thread_.joinable())
		thread_.join();
}

int ContainerIngest::read_packet(void *opaque, uint8_t *buf, int buf_size)
{
	auto *self = static_cast<ContainerIngest *>(opaque);
	std::unique_lock<std::mutex> lock(self->mutex_);
	self->cv_.wait(lock, [&]() { return self->eof_ || !self->chunks_.empty(); });
	if (self->chunks_.empty())
		return AVERROR_EOF;

	int copied = 0;
	while (copied < buf_size && !self->chunks_.empty()) {
		auto &chunk = self->chunks_.front();
		size_t available = chunk.size() - self->chunk_offset_;
		size_t n = std::min(available, (size_t)(buf_size - copied));
		memcpy(buf + copied, chunk.data() + self->chunk_offset_, n);
		copied += (int)n;
		self->chunk_offset_ += n;
		self->queued_ -= n;
		if (self->chunk_offset_ == chunk.size()) {
			self->chunks_.pop_front();
			self->chunk_offset_ = 0;
		}
	}
	return copied;
}

void ContainerIngest::run()
{
	auto *io_buffer = static_cast<uint8_t *>(av_malloc(kAvioBufferSize));
	AVIOContext *io = avio_alloc_context(io_buffer, kAvioBufferSize, 0, this, &ContainerIngest::read_packet,
					     nullptr, nullptr);
	AVFormatContext *fmt = avformat_alloc_context();
	if (!io || !fmt) {
		stopped_ = true;
		return;
	}
	io->seekable = 0;
	fmt->pb = io;
	fmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOBUFFER;
	fmt->probesize = 256 * 1024;
	fmt->max_analyze_duration = 500000;

	int rc = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
	if (rc < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(rc, err, sizeof(err));
		obs_log(LOG_WARNING, "[%s] could not read the incoming stream: %s", label_.c_str(), err);
		avformat_free_context(fmt);
		av_freep(&io->buffer);
		avio_context_free(&io);
		stopped_ = true;
		return;
	}

	/* Stream parameters are available from the headers; don't wait for analysis. */
	int video_index = -1;
	int audio_index = -1;
	receiver_->feed(session_, [&](StreamDecoder &decoder) {
		for (unsigned i = 0; i < fmt->nb_streams; i++) {
			AVCodecParameters *par = fmt->streams[i]->codecpar;
			if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0) {
				if (decoder.configure_video(par->codec_id, par->extradata, (size_t)par->extradata_size))
					video_index = (int)i;
			} else if (par->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
				if (decoder.configure_audio(par->codec_id, par->sample_rate, par->ch_layout.nb_channels,
							    par->extradata, (size_t)par->extradata_size))
					audio_index = (int)i;
			}
		}
	});
	obs_log(LOG_INFO, "[%s] receiving %s (video stream %d, audio stream %d)", label_.c_str(), fmt->iformat->name,
		video_index, audio_index);

	AVPacket *pkt = av_packet_alloc();
	while (av_read_frame(fmt, pkt) >= 0) {
		int index = pkt->stream_index;
		if (index == video_index || index == audio_index) {
			AVRational tb = fmt->streams[index]->time_base;
			int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			int64_t pts_us = pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(pts, tb, AVRational{1, 1000000});
			bool alive = receiver_->feed(session_, [&](StreamDecoder &decoder) {
				if (index == video_index)
					decoder.decode_video(pkt->data, (size_t)pkt->size, pts_us);
				else
					decoder.decode_audio(pkt->data, (size_t)pkt->size, pts_us);
			});
			if (!alive) {
				av_packet_unref(pkt);
				break;
			}
		}
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);

	avformat_close_input(&fmt);
	av_freep(&io->buffer);
	avio_context_free(&io);
	stopped_ = true;
	obs_log(LOG_INFO, "[%s] stream ended", label_.c_str());
}

} // namespace vcast
