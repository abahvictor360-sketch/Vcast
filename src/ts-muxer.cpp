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

#include "ts-muxer.h"

#include <cstring>

namespace vcast {

namespace {

constexpr uint16_t kPidPat = 0x0000;
constexpr uint16_t kPidPmt = 0x1000;
constexpr uint16_t kPidVideo = 0x0100;
constexpr uint16_t kPidAudio = 0x0101;
constexpr uint8_t kStreamTypeH264 = 0x1B;
constexpr uint8_t kStreamTypeAacAdts = 0x0F;

uint32_t crc32_mpeg(const uint8_t *data, size_t size)
{
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < size; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
	}
	return crc;
}

void put_timestamp(std::vector<uint8_t> &v, uint8_t prefix, int64_t ts)
{
	ts &= 0x1FFFFFFFFLL;
	v.push_back((uint8_t)((prefix << 4) | (((ts >> 30) & 0x07) << 1) | 1));
	v.push_back((uint8_t)(ts >> 22));
	v.push_back((uint8_t)((((ts >> 15) & 0x7F) << 1) | 1));
	v.push_back((uint8_t)(ts >> 7));
	v.push_back((uint8_t)(((ts & 0x7F) << 1) | 1));
}

/* Finds the next Annex-B start code at or after pos. Returns size if none. */
size_t find_start_code(const uint8_t *data, size_t size, size_t pos, size_t *sc_len)
{
	for (size_t i = pos; i + 3 <= size; i++) {
		if (data[i] == 0 && data[i + 1] == 0) {
			if (data[i + 2] == 1) {
				*sc_len = 3;
				return i;
			}
			if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
				*sc_len = 4;
				return i;
			}
		}
	}
	*sc_len = 0;
	return size;
}

/* Bitmask of the H.264 NAL unit types present in an Annex-B buffer. */
uint32_t nal_types(const uint8_t *data, size_t size, int *first_type)
{
	uint32_t mask = 0;
	size_t sc_len = 0;
	size_t pos = find_start_code(data, size, 0, &sc_len);
	*first_type = -1;
	while (pos < size) {
		size_t nal = pos + sc_len;
		if (nal < size) {
			int type = data[nal] & 0x1F;
			if (*first_type < 0)
				*first_type = type;
			mask |= 1u << type;
		}
		pos = find_start_code(data, size, nal, &sc_len);
	}
	return mask;
}

int aac_freq_index(int sample_rate)
{
	static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
				    22050, 16000, 12000, 11025, 8000,  7350};
	for (int i = 0; i < 13; i++) {
		if (rates[i] == sample_rate)
			return i;
	}
	return 3;
}

} // namespace

TsMuxer::TsMuxer()
{
	reset();
}

void TsMuxer::reset()
{
	cc_pat_ = cc_pmt_ = cc_video_ = cc_audio_ = 0;
	last_pcr_ = -1;
}

void TsMuxer::set_video_headers(const uint8_t *data, size_t size)
{
	video_headers_.assign(data, data + size);
}

void TsMuxer::set_audio_format(int sample_rate, int channels, const uint8_t *asc, size_t asc_size)
{
	aac_object_type_ = 2;
	aac_freq_index_ = aac_freq_index(sample_rate);
	aac_channel_config_ = channels > 0 && channels <= 6 ? channels : 2;
	if (channels == 8)
		aac_channel_config_ = 7;

	if (asc && asc_size >= 2) {
		int object_type = asc[0] >> 3;
		int freq_index = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);
		int channel_config = (asc[1] >> 3) & 0x0F;
		/* ADTS can only signal object types 1-4. */
		if (object_type >= 1 && object_type <= 4)
			aac_object_type_ = object_type;
		if (freq_index < 13)
			aac_freq_index_ = freq_index;
		if (channel_config > 0 && channel_config < 8)
			aac_channel_config_ = channel_config;
	}
}

void TsMuxer::write_section(uint16_t pid, uint8_t &cc, const std::vector<uint8_t> &section, std::vector<uint8_t> &out)
{
	uint8_t pkt[kPacketSize];
	memset(pkt, 0xFF, sizeof(pkt));
	pkt[0] = 0x47;
	pkt[1] = (uint8_t)(0x40 | ((pid >> 8) & 0x1F));
	pkt[2] = (uint8_t)(pid & 0xFF);
	pkt[3] = (uint8_t)(0x10 | (cc & 0x0F));
	cc = (cc + 1) & 0x0F;
	pkt[4] = 0; // pointer_field
	memcpy(pkt + 5, section.data(), section.size());
	out.insert(out.end(), pkt, pkt + kPacketSize);
}

void TsMuxer::write_psi(std::vector<uint8_t> &out)
{
	auto finish = [](std::vector<uint8_t> &s) {
		size_t section_length = s.size() - 3 + 4; // after length field, including CRC
		s[1] = (uint8_t)(0xB0 | ((section_length >> 8) & 0x0F));
		s[2] = (uint8_t)(section_length & 0xFF);
		uint32_t crc = crc32_mpeg(s.data(), s.size());
		s.push_back((uint8_t)(crc >> 24));
		s.push_back((uint8_t)(crc >> 16));
		s.push_back((uint8_t)(crc >> 8));
		s.push_back((uint8_t)crc);
	};

	std::vector<uint8_t> pat = {
		0x00, // table_id
		0x00,
		0x00, // section_length (filled in)
		0x00,
		0x01, // transport_stream_id
		0xC1, // version 0, current_next 1
		0x00,
		0x00, // section_number, last_section_number
		0x00,
		0x01, // program_number 1
		(uint8_t)(0xE0 | (kPidPmt >> 8)),
		(uint8_t)(kPidPmt & 0xFF),
	};
	finish(pat);
	write_section(kPidPat, cc_pat_, pat, out);

	std::vector<uint8_t> pmt = {
		0x02, // table_id
		0x00,
		0x00, // section_length
		0x00,
		0x01, // program_number
		0xC1,
		0x00,
		0x00,
		(uint8_t)(0xE0 | (kPidVideo >> 8)),
		(uint8_t)(kPidVideo & 0xFF), // PCR PID
		0xF0,
		0x00, // program_info_length
		kStreamTypeH264,
		(uint8_t)(0xE0 | (kPidVideo >> 8)),
		(uint8_t)(kPidVideo & 0xFF),
		0xF0,
		0x00,
	};
	if (has_audio_) {
		const uint8_t audio[] = {kStreamTypeAacAdts, (uint8_t)(0xE0 | (kPidAudio >> 8)),
					 (uint8_t)(kPidAudio & 0xFF), 0xF0, 0x00};
		pmt.insert(pmt.end(), audio, audio + sizeof(audio));
	}
	finish(pmt);
	write_section(kPidPmt, cc_pmt_, pmt, out);
}

void TsMuxer::write_pes(uint16_t pid, uint8_t &cc, uint8_t stream_id, const uint8_t *payload, size_t payload_size,
			int64_t pts, int64_t dts, bool write_dts, bool write_pcr, int64_t pcr, bool random_access,
			std::vector<uint8_t> &out)
{
	std::vector<uint8_t> header;
	header.reserve(19);
	header.push_back(0x00);
	header.push_back(0x00);
	header.push_back(0x01);
	header.push_back(stream_id);

	size_t header_data_len = write_dts ? 10 : 5;
	size_t pes_len = 3 + header_data_len + payload_size;
	/* Unbounded length (0) is allowed for video in a transport stream. */
	if (stream_id >= 0xE0 || pes_len > 0xFFFF)
		pes_len = 0;
	header.push_back((uint8_t)(pes_len >> 8));
	header.push_back((uint8_t)(pes_len & 0xFF));
	header.push_back(0x84); // '10', data_alignment_indicator
	header.push_back(write_dts ? 0xC0 : 0x80);
	header.push_back((uint8_t)header_data_len);
	put_timestamp(header, write_dts ? 0x3 : 0x2, pts);
	if (write_dts)
		put_timestamp(header, 0x1, dts);

	size_t total = header.size() + payload_size;
	size_t pos = 0;
	bool first = true;

	auto byte_at = [&](size_t i) -> uint8_t {
		return i < header.size() ? header[i] : payload[i - header.size()];
	};

	while (pos < total) {
		uint8_t pkt[kPacketSize];
		pkt[0] = 0x47;
		pkt[1] = (uint8_t)((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
		pkt[2] = (uint8_t)(pid & 0xFF);

		uint8_t af[kPacketSize];
		size_t af_len = 0; // adaptation field bytes after the length byte
		bool has_af = false;
		if (first && (write_pcr || random_access)) {
			has_af = true;
			af[af_len++] = (uint8_t)((random_access ? 0x40 : 0x00) | (write_pcr ? 0x10 : 0x00));
			if (write_pcr) {
				uint64_t base = (uint64_t)pcr & 0x1FFFFFFFFULL;
				af[af_len++] = (uint8_t)(base >> 25);
				af[af_len++] = (uint8_t)(base >> 17);
				af[af_len++] = (uint8_t)(base >> 9);
				af[af_len++] = (uint8_t)(base >> 1);
				af[af_len++] = (uint8_t)(((base & 1) << 7) | 0x7E);
				af[af_len++] = 0x00;
			}
		}

		size_t remaining = total - pos;
		size_t space = 184 - (has_af ? 1 + af_len : 0);
		if (remaining < space) {
			size_t stuffing = space - remaining;
			if (!has_af) {
				has_af = true;
				if (stuffing >= 2) {
					af[af_len++] = 0x00; // no flags
					stuffing -= 2;
				} else {
					stuffing -= 1; // length byte only
				}
			}
			memset(af + af_len, 0xFF, stuffing);
			af_len += stuffing;
			space = remaining;
		}

		pkt[3] = (uint8_t)((has_af ? 0x30 : 0x10) | (cc & 0x0F));
		cc = (cc + 1) & 0x0F;

		size_t off = 4;
		if (has_af) {
			pkt[off++] = (uint8_t)af_len;
			memcpy(pkt + off, af, af_len);
			off += af_len;
		}
		size_t n = kPacketSize - off;
		if (pos < header.size()) {
			for (size_t i = 0; i < n; i++)
				pkt[off + i] = byte_at(pos + i);
		} else {
			memcpy(pkt + off, payload + (pos - header.size()), n);
		}
		pos += n;
		first = false;
		out.insert(out.end(), pkt, pkt + kPacketSize);
	}
}

void TsMuxer::write_video(const uint8_t *data, size_t size, int64_t pts, int64_t dts, bool keyframe,
			  std::vector<uint8_t> &out)
{
	if (keyframe)
		write_psi(out);

	int first_type = -1;
	uint32_t types = nal_types(data, size, &first_type);

	scratch_.clear();
	/* Access unit delimiter: many TV decoders rely on it to find frame boundaries. */
	if (first_type != 9) {
		static const uint8_t aud[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};
		scratch_.insert(scratch_.end(), aud, aud + sizeof(aud));
	}
	/* Make every keyframe independently decodable for clients that join late. */
	if (keyframe && !(types & (1u << 7)) && !video_headers_.empty())
		scratch_.insert(scratch_.end(), video_headers_.begin(), video_headers_.end());
	scratch_.insert(scratch_.end(), data, data + size);

	/* PCR runs slightly behind DTS so the decoder has a small buffer. */
	int64_t pcr = dts - 9000;
	if (pcr < 0)
		pcr = 0;
	if (pcr < last_pcr_)
		pcr = last_pcr_;
	last_pcr_ = pcr;

	write_pes(kPidVideo, cc_video_, 0xE0, scratch_.data(), scratch_.size(), pts, dts, pts != dts, true, pcr,
		  keyframe, out);
}

void TsMuxer::write_audio(const uint8_t *data, size_t size, int64_t pts, std::vector<uint8_t> &out)
{
	if (!has_audio_)
		return;

	size_t frame_len = size + 7;
	if (frame_len > 0x1FFF)
		return;

	scratch_.clear();
	scratch_.reserve(frame_len);
	scratch_.push_back(0xFF);
	scratch_.push_back(0xF1); // MPEG-4, layer 0, no CRC
	scratch_.push_back((uint8_t)(((aac_object_type_ - 1) << 6) | (aac_freq_index_ << 2) |
				     ((aac_channel_config_ >> 2) & 0x01)));
	scratch_.push_back((uint8_t)(((aac_channel_config_ & 0x03) << 6) | ((frame_len >> 11) & 0x03)));
	scratch_.push_back((uint8_t)((frame_len >> 3) & 0xFF));
	scratch_.push_back((uint8_t)(((frame_len & 0x07) << 5) | 0x1F));
	scratch_.push_back(0xFC);
	scratch_.insert(scratch_.end(), data, data + size);

	write_pes(kPidAudio, cc_audio_, 0xC0, scratch_.data(), scratch_.size(), pts, pts, false, false, 0, false, out);
}

} // namespace vcast
