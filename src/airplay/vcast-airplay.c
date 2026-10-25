/*
 * Vcast AirPlay receiver: wrapper around UxPlay's raop/dnssd library.
 *
 * Copyright (C) 2026 Vcast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vcast-airplay.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

/* UxPlay */
#include "threads.h"
#include "raop.h"
#include "dnssd.h"
#include "logger.h"
#include "utils.h"
#include "netutils.h"

#ifdef __APPLE__
#include <time.h>
#elif !defined(_WIN32)
#include <time.h>
#endif

#define VCAST_AP_NAME_MAX 50        /* "<12 hex>@<name>" must fit a 63-byte DNS label */
#define VCAST_AP_FEEDBACK_LIMIT 15  /* seconds without /feedback before a session is reset */
#define VCAST_AP_SAMPLE_RATE 44100
#define VCAST_AP_CHANNELS 2

struct vcast_airplay {
	struct vcast_airplay_callbacks cb;
	struct vcast_airplay_options opts;

	char name[VCAST_AP_NAME_MAX + 1];
	char hw_addr[6];
	char device_id[18];

	raop_t *raop;
	dnssd_t *dnssd;
	unsigned short port;
	bool raop_registered;
	bool airplay_registered;

	/* state shared between UxPlay's threads (lock) */
	mutex_handle_t lock;
	bool stopping;
	int open_connections;
	bool session_active;
	int missed_feedback;
	bool reset_requested;
	float volume_db;

	/* audio format of the current stream (audio thread + SETUP) */
	unsigned char audio_ct;
	unsigned short audio_spf;
	bool audio_format_pending;
	int audio_codec_sent;

	/* supervisor thread: feedback timeouts and connection resets */
	thread_handle_t supervisor;
	bool supervisor_running;
	mutex_handle_t sup_lock;
	cond_handle_t sup_cond;
	bool sup_stop;
};

/* ------------------------------------------------------------------------- */
/* helpers */

static void set_err(char *err, size_t err_size, const char *fmt, ...)
{
	va_list ap;
	if (!err || !err_size)
		return;
	va_start(ap, fmt);
	vsnprintf(err, err_size, fmt, ap);
	va_end(ap);
}

static bool is_stopping(vcast_airplay_t *ap)
{
	bool stopping;
	MUTEX_LOCK(ap->lock);
	stopping = ap->stopping;
	MUTEX_UNLOCK(ap->lock);
	return stopping;
}

static void ap_log(vcast_airplay_t *ap, int level, const char *fmt, ...)
{
	char msg[512];
	va_list args;
	if (!ap->cb.on_log || level < ap->opts.log_level)
		return;
	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	ap->cb.on_log(ap->cb.opaque, level, msg);
}

uint64_t vcast_airplay_now_ns(void)
{
#if defined(_WIN32)
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;
	uint64_t q, f;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	q = (uint64_t)now.QuadPart;
	f = (uint64_t)freq.QuadPart;
	/* q * 1e9 / f without overflow, like OBS's util_mul_div64() */
	return (q / f) * 1000000000ULL + ((q % f) * 1000000000ULL) / f;
#elif defined(__APPLE__)
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* UxPlay's local times are wall-clock (Unix epoch) nanoseconds: map one onto
 * the monotonic clock. 0 stays 0 (unknown). */
static uint64_t local_to_monotonic(uint64_t wall_ns)
{
	if (!wall_ns)
		return 0;
	uint64_t wall_now = vcast_realtime_ns();
	uint64_t mono_now = vcast_airplay_now_ns();
	int64_t delta = (int64_t)(wall_ns - wall_now);
	if (delta < 0 && (uint64_t)(-delta) >= mono_now)
		return 0;
	return (uint64_t)((int64_t)mono_now + delta);
}

/* Does the Annex-B access unit contain a random access picture? */
static bool annexb_is_keyframe(const uint8_t *data, size_t size, bool h265)
{
	size_t i = 0;
	while (i + 3 < size) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			uint8_t nal = data[i + 3];
			if (h265) {
				int type = (nal >> 1) & 0x3f;
				if (type >= 16 && type <= 21)
					return true;
			} else if ((nal & 0x1f) == 5) {
				return true;
			}
			i += 3;
		} else {
			i++;
		}
	}
	return false;
}

/* MPEG-4 AudioSpecificConfig / ALAC magic cookie for an AirPlay audio type */
static int audio_config(unsigned char ct, unsigned short spf, uint8_t *config, size_t *config_size)
{
	switch (ct) {
	case 8: /* AAC-ELD: AOT 39 (escape), 44100 Hz, 2 channels, ELDSpecificConfig */
		config[0] = 0xf8;
		config[1] = 0xe8;
		/* frameLengthFlag: 1 = 480 samples per frame, 0 = 512 */
		config[2] = (spf == 512) ? 0x40 : 0x50;
		config[3] = 0x00;
		*config_size = 4;
		return VCAST_AIRPLAY_AUDIO_AAC_ELD;
	case 4: /* AAC-LC: AOT 2, 44100 Hz, 2 channels, 1024 samples */
		config[0] = 0x12;
		config[1] = 0x10;
		*config_size = 2;
		return VCAST_AIRPLAY_AUDIO_AAC_LC;
	case 2: { /* ALAC: 'alac' atom (size, type, version) + ALACSpecificConfig */
		uint32_t frame_length = spf ? spf : 352;
		uint8_t cookie[36] = {
			0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00, /* atom header */
			0, 0, 0, 0,                                                          /* frameLength */
			0x00,                                                                /* compatibleVersion */
			0x10,                                                                /* bitDepth 16 */
			0x28, 0x0a, 0x0e,                                                    /* pb, mb, kb */
			0x02,                                                                /* numChannels */
			0x00, 0xff,                                                          /* maxRun */
			0x00, 0x00, 0x00, 0x00,                                              /* maxFrameBytes */
			0x00, 0x00, 0x00, 0x00,                                              /* avgBitRate */
			0x00, 0x00, 0xac, 0x44,                                              /* sampleRate 44100 */
		};
		cookie[12] = (uint8_t)(frame_length >> 24);
		cookie[13] = (uint8_t)(frame_length >> 16);
		cookie[14] = (uint8_t)(frame_length >> 8);
		cookie[15] = (uint8_t)frame_length;
		memcpy(config, cookie, sizeof(cookie));
		*config_size = sizeof(cookie);
		return VCAST_AIRPLAY_AUDIO_ALAC;
	}
	default:
		*config_size = 0;
		return 0;
	}
}

/* UxPlay drops frames whose first byte shows a failed decryption */
static bool audio_frame_valid(unsigned char ct, const unsigned char *data, int len)
{
	if (len <= 0)
		return false;
	switch (ct) {
	case 8:
		switch (data[0]) {
		case 0x8c:
		case 0x8d:
		case 0x8e:
		case 0x80:
		case 0x81:
		case 0x82:
			return true;
		default:
			return false;
		}
	case 2:
		return data[0] == 0x20;
	default:
		return true;
	}
}

/* ------------------------------------------------------------------------- */
/* session state */

static void end_session_locked(vcast_airplay_t *ap, bool *notify)
{
	*notify = ap->session_active && !ap->stopping;
	ap->session_active = false;
	ap->missed_feedback = 0;
}

static void notify_disconnect(vcast_airplay_t *ap, bool notify)
{
	if (notify) {
		ap_log(ap, VCAST_AIRPLAY_LOG_INFO, "AirPlay client disconnected");
		if (ap->cb.on_disconnect)
			ap->cb.on_disconnect(ap->cb.opaque);
	}
}

static void wake_supervisor(vcast_airplay_t *ap)
{
	MUTEX_LOCK(ap->sup_lock);
	COND_SIGNAL(ap->sup_cond);
	MUTEX_UNLOCK(ap->sup_lock);
}

/* ------------------------------------------------------------------------- */
/* UxPlay callbacks */

static void cb_log(void *cls, int level, const char *msg)
{
	vcast_airplay_t *ap = cls;
	int lvl;
	char buf[1024];
	size_t len;

	if (level <= LOGGER_ERR)
		lvl = VCAST_AIRPLAY_LOG_ERROR;
	else if (level == LOGGER_WARNING)
		lvl = VCAST_AIRPLAY_LOG_WARNING;
	else if (level <= LOGGER_INFO)
		lvl = VCAST_AIRPLAY_LOG_INFO;
	else
		lvl = VCAST_AIRPLAY_LOG_DEBUG;
	if (!ap->cb.on_log || lvl < ap->opts.log_level)
		return;

	/* trim surrounding newlines, which UxPlay uses liberally */
	while (*msg == '\n' || *msg == '\r')
		msg++;
	snprintf(buf, sizeof(buf), "%s", msg);
	len = strlen(buf);
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
		buf[--len] = '\0';
	if (!len)
		return;
	ap->cb.on_log(ap->cb.opaque, lvl, buf);
}

static void cb_conn_init(void *cls)
{
	vcast_airplay_t *ap = cls;
	MUTEX_LOCK(ap->lock);
	ap->open_connections++;
	MUTEX_UNLOCK(ap->lock);
}

static void cb_conn_destroy(void *cls)
{
	vcast_airplay_t *ap = cls;
	bool notify = false;
	MUTEX_LOCK(ap->lock);
	if (ap->open_connections > 0)
		ap->open_connections--;
	if (ap->open_connections == 0)
		end_session_locked(ap, &notify);
	MUTEX_UNLOCK(ap->lock);
	notify_disconnect(ap, notify);
}

static void cb_conn_feedback(void *cls)
{
	vcast_airplay_t *ap = cls;
	MUTEX_LOCK(ap->lock);
	ap->missed_feedback = 0;
	MUTEX_UNLOCK(ap->lock);
}

static void cb_conn_reset(void *cls, int reason)
{
	vcast_airplay_t *ap = cls;
	ap_log(ap, VCAST_AIRPLAY_LOG_WARNING, "AirPlay connection lost (reason %d): resetting", reason);
	MUTEX_LOCK(ap->lock);
	ap->reset_requested = true;
	MUTEX_UNLOCK(ap->lock);
	wake_supervisor(ap);
}

static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit)
{
	vcast_airplay_t *ap = cls;
	bool notify_old = false, stopping;

	*admit = true;
	MUTEX_LOCK(ap->lock);
	stopping = ap->stopping;
	if (ap->session_active)
		end_session_locked(ap, &notify_old);
	ap->session_active = !stopping;
	ap->missed_feedback = 0;
	MUTEX_UNLOCK(ap->lock);

	notify_disconnect(ap, notify_old);
	if (stopping)
		return;
	ap_log(ap, VCAST_AIRPLAY_LOG_INFO, "AirPlay client connected: %s (%s, device id %s)", name ? name : "?",
	       model ? model : "?", deviceid ? deviceid : "?");
	if (ap->cb.on_connect)
		ap->cb.on_connect(ap->cb.opaque, name);
}

static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *using_screen, bool *is_media,
				uint64_t *audio_format)
{
	vcast_airplay_t *ap = cls;
	ap_log(ap, VCAST_AIRPLAY_LOG_DEBUG, "audio format: ct=%d spf=%d usingScreen=%d isMedia=%d audioFormat=0x%llx",
	       (int)*ct, (int)*spf, (int)*using_screen, (int)*is_media, (unsigned long long)*audio_format);
	MUTEX_LOCK(ap->lock);
	ap->audio_ct = *ct;
	ap->audio_spf = *spf;
	ap->audio_format_pending = true;
	MUTEX_UNLOCK(ap->lock);
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data)
{
	vcast_airplay_t *ap = cls;
	unsigned char ct;
	unsigned short spf;
	bool send_format, stopping;
	(void)ntp;

	if (!data || !data->data || data->data_len <= 0)
		return;

	MUTEX_LOCK(ap->lock);
	stopping = ap->stopping;
	if (data->ct && data->ct != ap->audio_ct) {
		ap->audio_ct = data->ct;
		ap->audio_format_pending = true;
	}
	ct = ap->audio_ct;
	spf = ap->audio_spf;
	send_format = ap->audio_format_pending;
	ap->audio_format_pending = false;
	MUTEX_UNLOCK(ap->lock);
	if (stopping)
		return;

	if (send_format) {
		uint8_t config[64];
		size_t config_size = 0;
		int codec = audio_config(ct, spf, config, &config_size);
		ap->audio_codec_sent = codec;
		if (!codec) {
			ap_log(ap, VCAST_AIRPLAY_LOG_WARNING, "unsupported AirPlay audio type ct=%d", (int)ct);
		} else if (ap->cb.on_audio_format) {
			ap->cb.on_audio_format(ap->cb.opaque, codec, VCAST_AP_SAMPLE_RATE, VCAST_AP_CHANNELS, config,
					       config_size);
		}
	}
	if (!ap->audio_codec_sent)
		return;
	if (!audio_frame_valid(ct, data->data, data->data_len)) {
		ap_log(ap, VCAST_AIRPLAY_LOG_DEBUG, "dropping invalid audio frame (ct=%d, first byte 0x%02x)", (int)ct,
		       (unsigned int)data->data[0]);
		return;
	}
	if (ap->cb.on_audio)
		ap->cb.on_audio(ap->cb.opaque, data->data, (size_t)data->data_len,
				local_to_monotonic(data->ntp_time_local));
}

static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data)
{
	vcast_airplay_t *ap = cls;
	(void)ntp;

	if (!data || !data->data || data->data_len <= 4 || is_stopping(ap))
		return;
	/* UxPlay marks units that failed to decrypt/parse by overwriting the first start code */
	if (data->data[0] != 0) {
		ap_log(ap, VCAST_AIRPLAY_LOG_DEBUG, "dropping invalid video unit (%d bytes)", data->data_len);
		return;
	}
	if (ap->cb.on_video) {
		size_t size = (size_t)data->data_len;
		bool keyframe = annexb_is_keyframe(data->data, size, data->is_h265);
		ap->cb.on_video(ap->cb.opaque, data->is_h265 ? VCAST_AIRPLAY_VIDEO_H265 : VCAST_AIRPLAY_VIDEO_H264,
				data->data, size, local_to_monotonic(data->ntp_time_local), keyframe ? 1 : 0);
	}
}

static int cb_video_set_codec_and_size(void *cls, video_codec_t codec, unsigned short width_source,
				       unsigned short height_source, unsigned short x_offset, unsigned short y_offset,
				       unsigned short width, unsigned short height)
{
	vcast_airplay_t *ap = cls;
	ap_log(ap, VCAST_AIRPLAY_LOG_INFO, "mirroring %s: source %ux%u, picture %ux%u at (%u,%u)",
	       codec == VIDEO_CODEC_H265 ? "H.265" : "H.264", (unsigned)width_source, (unsigned)height_source,
	       (unsigned)width, (unsigned)height, (unsigned)x_offset, (unsigned)y_offset);
	if (codec == VIDEO_CODEC_H265 && !ap->opts.enable_h265)
		return -1;
	return 0;
}

static void cb_video_reset(void *cls, reset_type_t type)
{
	vcast_airplay_t *ap = cls;
	ap_log(ap, VCAST_AIRPLAY_LOG_DEBUG, "video reset (type %d)", (int)type);
}

static void cb_video_pause(void *cls)
{
	vcast_airplay_t *ap = cls;
	ap_log(ap, VCAST_AIRPLAY_LOG_DEBUG, "mirroring paused by the client");
}

static void cb_video_resume(void *cls)
{
	vcast_airplay_t *ap = cls;
	ap_log(ap, VCAST_AIRPLAY_LOG_DEBUG, "mirroring resumed by the client");
}

static void cb_flush(void *cls)
{
	(void)cls;
}

static double cb_audio_set_client_volume(void *cls)
{
	vcast_airplay_t *ap = cls;
	double volume;
	MUTEX_LOCK(ap->lock);
	volume = (double)ap->volume_db;
	MUTEX_UNLOCK(ap->lock);
	return volume;
}

static void cb_audio_set_volume(void *cls, float volume)
{
	vcast_airplay_t *ap = cls;
	MUTEX_LOCK(ap->lock);
	ap->volume_db = volume;
	MUTEX_UNLOCK(ap->lock);
	if (ap->cb.on_volume && !is_stopping(ap))
		ap->cb.on_volume(ap->cb.opaque, volume);
}

/* ------------------------------------------------------------------------- */
/* supervisor: resets dead sessions like UxPlay's main loop does */

static void reset_connections(vcast_airplay_t *ap)
{
	unsigned short port = ap->port;
	ap_log(ap, VCAST_AIRPLAY_LOG_INFO, "resetting AirPlay connections");
	raop_stop_httpd(ap->raop);
	raop_remove_known_connections(ap->raop);
	if (raop_start_httpd(ap->raop, &port) < 0)
		ap_log(ap, VCAST_AIRPLAY_LOG_ERROR, "could not restart the AirPlay server on TCP port %u",
		       (unsigned)ap->port);
}

static THREAD_RETVAL supervisor_thread(void *arg)
{
	vcast_airplay_t *ap = arg;
	uint64_t last_tick = vcast_airplay_now_ns();

	for (;;) {
		bool stop, reset = false, requested = false;

		MUTEX_LOCK(ap->sup_lock);
		if (!ap->sup_stop)
			COND_TIMEDWAIT_MS(ap->sup_cond, ap->sup_lock, 250);
		stop = ap->sup_stop;
		MUTEX_UNLOCK(ap->sup_lock);
		if (stop)
			break;

		uint64_t now = vcast_airplay_now_ns();
		bool tick = now - last_tick >= 1000000000ULL;
		if (tick)
			last_tick = now;

		MUTEX_LOCK(ap->lock);
		if (ap->reset_requested) {
			ap->reset_requested = false;
			reset = requested = true;
		}
		if (tick && ap->session_active && ap->open_connections > 0) {
			if (++ap->missed_feedback > VCAST_AP_FEEDBACK_LIMIT) {
				reset = true;
				ap->missed_feedback = 0;
			}
		}
		MUTEX_UNLOCK(ap->lock);

		if (reset) {
			if (!requested)
				ap_log(ap, VCAST_AIRPLAY_LOG_WARNING,
				       "no response from the AirPlay client for %d seconds (or connection lost)",
				       VCAST_AP_FEEDBACK_LIMIT);
			reset_connections(ap);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* identity */

static void derive_identity(vcast_airplay_t *ap)
{
	static const char prefix[] = "vcast-airplay-device-id:";
	unsigned char hash[32];
	mbedtls_sha256_context ctx;

	mbedtls_sha256_init(&ctx);
	mbedtls_sha256_starts(&ctx, 0);
	mbedtls_sha256_update(&ctx, (const unsigned char *)prefix, sizeof(prefix) - 1);
	mbedtls_sha256_update(&ctx, (const unsigned char *)ap->name, strlen(ap->name));
	mbedtls_sha256_finish(&ctx, hash);
	mbedtls_sha256_free(&ctx);

	memcpy(ap->hw_addr, hash, sizeof(ap->hw_addr));
	/* a locally administered, unicast MAC address */
	ap->hw_addr[0] = (char)((hash[0] & 0xfc) | 0x02);
	snprintf(ap->device_id, sizeof(ap->device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
		 (unsigned)(unsigned char)ap->hw_addr[0], (unsigned)(unsigned char)ap->hw_addr[1],
		 (unsigned)(unsigned char)ap->hw_addr[2], (unsigned)(unsigned char)ap->hw_addr[3],
		 (unsigned)(unsigned char)ap->hw_addr[4], (unsigned)(unsigned char)ap->hw_addr[5]);
}

/* Copy the name, replacing characters that would break the DNS-SD service
 * names and cutting it to VCAST_AP_NAME_MAX bytes at a UTF-8 boundary. */
static void sanitize_name(char *dst, const char *src)
{
	size_t len = 0;
	if (!src)
		src = "";
	while (*src == ' ')
		src++;
	while (src[len] && len < VCAST_AP_NAME_MAX)
		len++;
	if (src[len]) {
		/* don't cut a multi-byte character */
		while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80)
			len--;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)dst[i];
		if (c == '.' || c < 0x20 || c == 0x7f)
			dst[i] = '-';
	}
	while (len > 0 && dst[len - 1] == ' ')
		dst[--len] = '\0';
	if (!len)
		snprintf(dst, VCAST_AP_NAME_MAX + 1, "%s", "Vcast");
	/* dnssd_init() would cut the name at a ".local" substring: '.' is gone */
}

/* ------------------------------------------------------------------------- */
/* create / destroy */

void vcast_airplay_default_options(struct vcast_airplay_options *opts)
{
	if (!opts)
		return;
	memset(opts, 0, sizeof(*opts));
	opts->width = 1920;
	opts->height = 1080;
	opts->fps = 60;
	opts->enable_h265 = 0;
	opts->enable_pairing = 0;
	opts->allow_takeover = 1;
	opts->log_level = VCAST_AIRPLAY_LOG_INFO;
}

static int start_raop(vcast_airplay_t *ap, unsigned short port_base, char *err, size_t err_size)
{
	raop_callbacks_t cbs;
	unsigned short tcp[2], udp[3];
	int ret;

	memset(&cbs, 0, sizeof(cbs));
	cbs.cls = ap;
	cbs.audio_process = cb_audio_process;
	cbs.video_process = cb_video_process;
	cbs.video_pause = cb_video_pause;
	cbs.video_resume = cb_video_resume;
	cbs.conn_feedback = cb_conn_feedback;
	cbs.conn_reset = cb_conn_reset;
	cbs.video_reset = cb_video_reset;
	cbs.video_set_codec_and_size = cb_video_set_codec_and_size;
	cbs.conn_init = cb_conn_init;
	cbs.conn_destroy = cb_conn_destroy;
	cbs.audio_flush = cb_flush;
	cbs.video_flush = cb_flush;
	cbs.audio_set_client_volume = cb_audio_set_client_volume;
	cbs.audio_set_volume = cb_audio_set_volume;
	cbs.audio_get_format = cb_audio_get_format;
	cbs.report_client_request = cb_report_client_request;

	ap->raop = raop_init(&cbs);
	if (!ap->raop) {
		set_err(err, err_size, "AirPlay: could not initialize the RAOP server");
		return -1;
	}
	raop_set_log_callback(ap->raop, cb_log, ap);
	switch (ap->opts.log_level) {
	case VCAST_AIRPLAY_LOG_DEBUG:
		raop_set_log_level(ap->raop, LOGGER_DEBUG);
		break;
	case VCAST_AIRPLAY_LOG_INFO:
		raop_set_log_level(ap->raop, LOGGER_INFO);
		break;
	case VCAST_AIRPLAY_LOG_WARNING:
		raop_set_log_level(ap->raop, LOGGER_WARNING);
		break;
	default:
		raop_set_log_level(ap->raop, LOGGER_ERR);
		break;
	}
	if (raop_init2(ap->raop, ap->opts.allow_takeover ? 1 : 0, ap->device_id, "")) {
		set_err(err, err_size, "AirPlay: could not initialize the RAOP server (keys)");
		return -1;
	}

	raop_set_plist(ap->raop, "width", ap->opts.width);
	raop_set_plist(ap->raop, "height", ap->opts.height);
	raop_set_plist(ap->raop, "refreshRate", ap->opts.fps);
	raop_set_plist(ap->raop, "maxFPS", ap->opts.fps);

	if (port_base) {
		tcp[0] = (unsigned short)(port_base + 1); /* mirroring data */
		tcp[1] = port_base;                       /* RTSP/HTTP */
		udp[0] = port_base;                       /* NTP timing */
		udp[1] = (unsigned short)(port_base + 1); /* audio control */
		udp[2] = (unsigned short)(port_base + 2); /* audio data */
	} else {
		tcp[0] = 7100;
		tcp[1] = 7000;
		udp[0] = 7011;
		udp[1] = 6001;
		udp[2] = 6000;
	}
	raop_set_tcp_ports(ap->raop, tcp);
	raop_set_udp_ports(ap->raop, udp);

	ap->port = tcp[1];
	ret = raop_start_httpd(ap->raop, &ap->port);
	if (ret <= 0 && !port_base) {
		ap_log(ap, VCAST_AIRPLAY_LOG_WARNING, "TCP port 7000 is not available, using dynamic ports");
		memset(tcp, 0, sizeof(tcp));
		memset(udp, 0, sizeof(udp));
		raop_set_tcp_ports(ap->raop, tcp);
		raop_set_udp_ports(ap->raop, udp);
		ap->port = 0;
		ret = raop_start_httpd(ap->raop, &ap->port);
	}
	if (ret <= 0) {
		set_err(err, err_size, "AirPlay: could not listen on TCP port %u (in use?)", (unsigned)tcp[1]);
		return -1;
	}
	raop_set_port(ap->raop, ap->port);
	raop_set_dnssd(ap->raop, ap->dnssd);
	return 0;
}

vcast_airplay_t *vcast_airplay_create(const char *name, unsigned short port_base, const struct vcast_airplay_callbacks *cb,
				      char *err, size_t err_size)
{
	return vcast_airplay_create_ex(name, port_base, cb, NULL, err, err_size);
}

vcast_airplay_t *vcast_airplay_create_ex(const char *name, unsigned short port_base,
					 const struct vcast_airplay_callbacks *cb,
					 const struct vcast_airplay_options *opts, char *err, size_t err_size)
{
	vcast_airplay_t *ap;
	int dnssd_error = 0;

	set_err(err, err_size, "%s", "");
	if (port_base > 65533) {
		set_err(err, err_size, "AirPlay: port base %u is too high", (unsigned)port_base);
		return NULL;
	}

	ap = calloc(1, sizeof(*ap));
	if (!ap) {
		set_err(err, err_size, "AirPlay: out of memory");
		return NULL;
	}
	if (cb)
		ap->cb = *cb;
	vcast_airplay_default_options(&ap->opts);
	if (opts) {
		ap->opts = *opts;
		if (ap->opts.width <= 0 || ap->opts.width > 7680)
			ap->opts.width = 1920;
		if (ap->opts.height <= 0 || ap->opts.height > 4320)
			ap->opts.height = 1080;
		if (ap->opts.fps <= 0 || ap->opts.fps > 255)
			ap->opts.fps = 60;
	}
	if (netutils_init() < 0) { /* WSAStartup on Windows */
		set_err(err, err_size, "AirPlay: could not initialize networking");
		free(ap);
		return NULL;
	}
	MUTEX_CREATE(ap->lock);
	MUTEX_CREATE(ap->sup_lock);
	COND_CREATE(ap->sup_cond);
	ap->volume_db = 0.0f;

	sanitize_name(ap->name, name);
	derive_identity(ap);

	/* mDNS records first: raop_set_dnssd() (in start_raop) hands it the public key */
	ap->dnssd = dnssd_init(ap->name, (int)strlen(ap->name), ap->hw_addr, (int)sizeof(ap->hw_addr), 0, &dnssd_error);
	if (!ap->dnssd || dnssd_error) {
		set_err(err, err_size, "AirPlay: could not initialize DNS-SD (error %d)", dnssd_error);
		goto fail;
	}
	/* as in UxPlay: AirPlay video (HLS) off; legacy pairing and H.265 optional */
	dnssd_set_airplay_features(ap->dnssd, 0, 0);
	dnssd_set_airplay_features(ap->dnssd, 4, 0);
	dnssd_set_airplay_features(ap->dnssd, 27, ap->opts.enable_pairing ? 1 : 0);
	dnssd_set_airplay_features(ap->dnssd, 42, ap->opts.enable_h265 ? 1 : 0);

	if (start_raop(ap, port_base, err, err_size) < 0)
		goto fail;

	ap->sup_stop = false;
	THREAD_CREATE(ap->supervisor, supervisor_thread, ap);
	ap->supervisor_running = ap->supervisor != 0;
	if (!ap->supervisor_running) {
		set_err(err, err_size, "AirPlay: could not start a thread");
		goto fail;
	}

	dnssd_error = dnssd_register_raop(ap->dnssd, ap->port);
	if (dnssd_error) {
		set_err(err, err_size, "AirPlay: mDNS registration failed (error %d; is UDP port 5353 usable?)",
			dnssd_error);
		goto fail;
	}
	ap->raop_registered = true;
	dnssd_error = dnssd_register_airplay(ap->dnssd, ap->port);
	if (dnssd_error) {
		set_err(err, err_size, "AirPlay: mDNS registration failed (error %d; is UDP port 5353 usable?)",
			dnssd_error);
		goto fail;
	}
	ap->airplay_registered = true;

	ap_log(ap, VCAST_AIRPLAY_LOG_INFO, "AirPlay receiver \"%s\" (device id %s) listening on TCP port %u", ap->name,
	       ap->device_id, (unsigned)ap->port);
	return ap;

fail:
	vcast_airplay_destroy(ap);
	return NULL;
}

void vcast_airplay_destroy(vcast_airplay_t *ap)
{
	if (!ap)
		return;

	MUTEX_LOCK(ap->lock);
	ap->stopping = true;
	MUTEX_UNLOCK(ap->lock);

	/* mDNS goodbyes, so senders drop the receiver right away */
	if (ap->dnssd) {
		if (ap->airplay_registered)
			dnssd_unregister_airplay(ap->dnssd);
		if (ap->raop_registered)
			dnssd_unregister_raop(ap->dnssd);
	}

	if (ap->supervisor_running) {
		MUTEX_LOCK(ap->sup_lock);
		ap->sup_stop = true;
		COND_SIGNAL(ap->sup_cond);
		MUTEX_UNLOCK(ap->sup_lock);
		THREAD_JOIN(ap->supervisor);
		ap->supervisor_running = false;
	}

	/* stops the server, closes client connections and joins all session threads */
	if (ap->raop)
		raop_destroy(ap->raop);
	ap->raop = NULL;

	if (ap->dnssd)
		dnssd_destroy(ap->dnssd);
	ap->dnssd = NULL;

	COND_DESTROY(ap->sup_cond);
	MUTEX_DESTROY(ap->sup_lock);
	MUTEX_DESTROY(ap->lock);
	free(ap);
	netutils_cleanup();
}

unsigned short vcast_airplay_get_port(const vcast_airplay_t *ap)
{
	return ap ? ap->port : 0;
}

const char *vcast_airplay_get_device_id(const vcast_airplay_t *ap, char *buf, size_t buf_size)
{
	if (buf && buf_size) {
		snprintf(buf, buf_size, "%s", ap ? ap->device_id : "");
	}
	return buf;
}
