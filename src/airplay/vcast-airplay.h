/*
 * Vcast AirPlay receiver: a small C API over UxPlay's AirPlay mirroring server
 * (third_party/uxplay), so that iPhone/iPad/Mac "Screen Mirroring" (and AirPlay
 * audio) can target OBS. Video and audio are delivered compressed and already
 * decrypted; decoding (e.g. with FFmpeg) is up to the caller.
 *
 * Copyright (C) 2026 Vcast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Threads: every callback is invoked from an internal thread, never from the
 * caller of vcast_airplay_create()/destroy():
 *   on_connect, on_volume        the receiver's HTTP/RTSP thread
 *   on_video                     the mirroring (TCP) thread of the session
 *   on_audio_format, on_audio    the audio (UDP RTP) thread of the session
 *   on_disconnect                the HTTP/RTSP thread or the internal supervisor
 *   on_log                       any of these threads
 * Callbacks of one kind are never invoked concurrently with each other, but
 * video, audio and the others may run at the same time on different threads.
 * Callbacks must return quickly (copy the data), must not call
 * vcast_airplay_destroy(), and on_log must not call back into this library.
 * No callback is running or invoked any more once vcast_airplay_destroy()
 * returns; once destroy has started, only on_log may still be called. Do not
 * hold a lock in the caller of vcast_airplay_destroy() that a callback takes.
 *
 * Timestamps (ts_ns): the sender's presentation timestamps (audio and video
 * share the sender's clock), mapped to this computer's monotonic clock in
 * nanoseconds, on the same base as OBS's os_gettime_ns() (CLOCK_MONOTONIC on
 * Linux, CLOCK_UPTIME_RAW on macOS, QueryPerformanceCounter on Windows); see
 * vcast_airplay_now_ns(). As in UxPlay's own renderers, the offset between
 * the two clocks is fixed when the first audio or video frame of a session
 * arrives (that frame gets ts_ns = now), so audio and video stay in sync with
 * each other; drift between the sender's clock and ours is not corrected
 * (typically well under 0.1 s per hour). Audio timestamps run ahead of video
 * (AirPlay sends audio early, with its intended playout time). 0 means
 * unknown (only if the sender gave no timestamp).
 *
 * Network: the receiver listens on TCP (RTSP/HTTP control + mirroring stream)
 * and UDP (NTP timing, audio control, audio data), see vcast_airplay_create().
 * It is advertised over mDNS/DNS-SD (UDP 5353 multicast) as _airplay._tcp and
 * _raop._tcp: on Windows and Linux by a built-in responder (no Bonjour/Avahi
 * needed), on macOS through the system's mDNSResponder. Firewalls must allow
 * these ports inbound (on Windows: allow OBS on private networks).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vcast_airplay vcast_airplay_t;

enum { VCAST_AIRPLAY_VIDEO_H264 = 1, VCAST_AIRPLAY_VIDEO_H265 = 2 };
enum { VCAST_AIRPLAY_AUDIO_AAC_ELD = 1, VCAST_AIRPLAY_AUDIO_ALAC = 2, VCAST_AIRPLAY_AUDIO_AAC_LC = 3 };

/* on_log levels */
enum {
	VCAST_AIRPLAY_LOG_DEBUG = 0,
	VCAST_AIRPLAY_LOG_INFO = 1,
	VCAST_AIRPLAY_LOG_WARNING = 2,
	VCAST_AIRPLAY_LOG_ERROR = 3,
};

struct vcast_airplay_callbacks {
	void *opaque;
	/* A client (identified by the name it reports, may be NULL) started a
	 * session. If another client takes over, on_disconnect is called for the
	 * first one before on_connect for the new one. */
	void (*on_connect)(void *opaque, const char *device_name);
	/* The session ended (all of the client's connections closed, or the
	 * receiver reset a dead connection). */
	void (*on_disconnect)(void *opaque);
	/* Annex-B H.264/H.265 access unit, already decrypted, with 4-byte start
	 * codes; SPS/PPS(/VPS) are delivered inline before keyframes and whenever
	 * they change. keyframe: the access unit contains an IDR (H.264) or IRAP
	 * (H.265) picture. ts_ns: see above. Corrupt (undecryptable) units are
	 * dropped, not delivered. */
	void (*on_video)(void *opaque, int codec, const uint8_t *data, size_t size, uint64_t ts_ns, int keyframe);
	/* Called before the first audio frame of every audio stream and when the
	 * format changes. config: AAC AudioSpecificConfig (AAC-ELD, AAC-LC) or the
	 * 36-byte ALAC magic cookie ('alac' atom), suitable as FFmpeg extradata.
	 * AirPlay audio is 44100 Hz stereo (16 bit for ALAC); AAC-ELD frames are
	 * 480 samples (screen mirroring), ALAC frames 352 samples (music). */
	void (*on_audio_format)(void *opaque, int codec, int sample_rate, int channels, const uint8_t *config,
				size_t config_size);
	/* One decrypted compressed audio frame (raw AAC/ALAC access unit, no ADTS
	 * header added). */
	void (*on_audio)(void *opaque, const uint8_t *data, size_t size, uint64_t ts_ns);
	/* Optional: the sender's volume slider, in dB: -30.0 (min) .. 0.0 (max),
	 * -144.0 means muted. */
	void (*on_volume)(void *opaque, float volume_db);
	/* Optional: library log messages (level: VCAST_AIRPLAY_LOG_*), without a
	 * trailing newline. */
	void (*on_log)(void *opaque, int level, const char *msg);
};

/* Optional settings for vcast_airplay_create_ex(). Initialize with
 * vcast_airplay_default_options(), then change fields. */
struct vcast_airplay_options {
	/* Display advertised to the sender (it picks the stream resolution and
	 * frame rate from this). Defaults: 1920 x 1080, 60 fps. */
	int width;
	int height;
	int fps;
	/* Also accept H.265/HEVC mirroring (AirPlay feature bit 42); off by
	 * default (H.264 only). */
	int enable_h265;
	/* Ask clients for AirPlay "legacy pairing" (pair-setup/pair-verify with
	 * Ed25519/X25519, feature bit 27) before streaming. Off by default, as in
	 * UxPlay: current iOS/macOS connect without it, and faster. */
	int enable_pairing;
	/* 1: a new client may take over while another one is streaming (the first
	 * is disconnected); 0: it is refused while the first is connected.
	 * Default 1. */
	int allow_takeover;
	/* Library log verbosity forwarded to on_log: VCAST_AIRPLAY_LOG_DEBUG ..
	 * VCAST_AIRPLAY_LOG_ERROR. Default VCAST_AIRPLAY_LOG_INFO. */
	int log_level;
};

void vcast_airplay_default_options(struct vcast_airplay_options *opts);

/* name: shown on the iPhone (UTF-8; '.' is replaced, at most 50 bytes are
 * used). Its hash also gives the receiver's device id (a locally administered
 * MAC address) and pairing key, so a receiver keeps its identity across
 * restarts as long as the name is the same, and two receivers with different
 * names never share one. Names must be unique on the network.
 *
 * port_base: 0 = UxPlay's classic fixed ports: TCP 7000 (RTSP/HTTP) and 7100
 * (mirroring), UDP 7011 (timing), 6001 (audio control), 6000 (audio data); if
 * TCP 7000 is taken (e.g. by macOS's own AirPlay Receiver), dynamic ports are
 * used instead. Otherwise TCP port_base (RTSP/HTTP) and port_base+1
 * (mirroring), UDP port_base (timing), port_base+1 (audio control) and
 * port_base+2 (audio data), so receivers at least 3 ports apart can coexist.
 * The UDP ports and the mirroring port are opened when a session starts.
 *
 * Returns NULL and fills err (if err_size > 0) on failure. cb is copied; the
 * function pointers in it may be NULL. Callbacks are invoked from internal
 * threads (see the top of this file). */
vcast_airplay_t *vcast_airplay_create(const char *name, unsigned short port_base, const struct vcast_airplay_callbacks *cb,
				      char *err, size_t err_size);
/* Same, with options (NULL = defaults). */
vcast_airplay_t *vcast_airplay_create_ex(const char *name, unsigned short port_base,
					 const struct vcast_airplay_callbacks *cb,
					 const struct vcast_airplay_options *opts, char *err, size_t err_size);

/* Stops everything: sends mDNS goodbyes, closes client connections, joins all
 * threads. Safe to call while a client is connected. NULL is ignored. */
void vcast_airplay_destroy(vcast_airplay_t *ap);

/* The TCP port of the RTSP/HTTP server (the one advertised over mDNS). */
unsigned short vcast_airplay_get_port(const vcast_airplay_t *ap);

/* The device id, as advertised ("aa:bb:cc:dd:ee:ff"). Returns buf. */
const char *vcast_airplay_get_device_id(const vcast_airplay_t *ap, char *buf, size_t buf_size);

/* Current time on the clock used for ts_ns (same as OBS's os_gettime_ns()). */
uint64_t vcast_airplay_now_ns(void);

#ifdef __cplusplus
}
#endif
