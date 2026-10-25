/*
 * airplay-smoke: run a Vcast AirPlay receiver and print every callback as one
 * line on stdout (for tests/airplay/airplay-e2e.py, or manual tests with a real
 * iPhone/Mac).
 *
 *   airplay-smoke [-n name] [-p port_base] [-t seconds] [-v] [-pairing] [-h265]
 *                 [-video file.h264]
 *       runs until stdin reaches EOF (or the timeout), then destroys the receiver
 *   airplay-smoke --fpdecrypt <keymsg hex (164 bytes)> <ekey hex (72 bytes)>
 *       prints the AES key UxPlay's FairPlay code derives (test helper)
 *
 * Copyright (C) 2026 Vcast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcast-airplay.h"
#include "threads.h"
#include "playfair/playfair.h"

static FILE *video_file;
static mutex_handle_t out_lock = MUTEX_INITIALIZER;
static unsigned long video_frames, audio_frames;

static void out(const char *fmt, ...)
{
	va_list ap;
	MUTEX_LOCK(out_lock);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	MUTEX_UNLOCK(out_lock);
}

static void hexstr(char *dst, const uint8_t *data, size_t n)
{
	for (size_t i = 0; i < n; i++)
		sprintf(dst + 2 * i, "%02x", data[i]);
	dst[2 * n] = '\0';
}

static void on_connect(void *o, const char *name)
{
	(void)o;
	out("CONNECT %s\n", name ? name : "(null)");
}

static void on_disconnect(void *o)
{
	(void)o;
	out("DISCONNECT\n");
}

static void on_video(void *o, int codec, const uint8_t *data, size_t size, uint64_t ts, int key)
{
	char head[2 * 48 + 1];
	(void)o;
	video_frames++;
	hexstr(head, data, size < 48 ? size : 48);
	out("VIDEO codec=%d size=%zu ts=%llu key=%d now=%llu head=%s\n", codec, size, (unsigned long long)ts, key,
	    (unsigned long long)vcast_airplay_now_ns(), head);
	if (video_file)
		fwrite(data, 1, size, video_file);
}

static void on_audio_format(void *o, int codec, int rate, int ch, const uint8_t *config, size_t n)
{
	char hex[2 * 64 + 1];
	(void)o;
	hexstr(hex, config, n < 64 ? n : 64);
	out("AUDIOFMT codec=%d rate=%d channels=%d config=%s\n", codec, rate, ch, hex);
}

static void on_audio(void *o, const uint8_t *data, size_t size, uint64_t ts)
{
	char head[2 * 16 + 1];
	(void)o;
	audio_frames++;
	hexstr(head, data, size < 16 ? size : 16);
	out("AUDIO size=%zu ts=%llu now=%llu head=%s\n", size, (unsigned long long)ts,
	    (unsigned long long)vcast_airplay_now_ns(), head);
}

static void on_volume(void *o, float db)
{
	(void)o;
	out("VOLUME %.2f\n", (double)db);
}

static void on_log(void *o, int level, const char *msg)
{
	(void)o;
	/* keep the protocol lines parseable: one log message per line */
	char buf[600];
	snprintf(buf, sizeof(buf), "%s", msg);
	for (char *p = buf; *p; p++)
		if (*p == '\n' || *p == '\r')
			*p = ' ';
	out("LOG %d %s\n", level, buf);
}

static int fpdecrypt(const char *keymsg_hex, const char *ekey_hex)
{
	unsigned char keymsg[164], ekey[72], key[16];
	if (strlen(keymsg_hex) != 328 || strlen(ekey_hex) != 144)
		return 2;
	for (int i = 0; i < 164; i++) {
		unsigned int b;
		sscanf(keymsg_hex + 2 * i, "%2x", &b);
		keymsg[i] = (unsigned char)b;
	}
	for (int i = 0; i < 72; i++) {
		unsigned int b;
		sscanf(ekey_hex + 2 * i, "%2x", &b);
		ekey[i] = (unsigned char)b;
	}
	playfair_decrypt(keymsg, ekey, key);
	for (int i = 0; i < 16; i++)
		printf("%02x", key[i]);
	printf("\n");
	return 0;
}

static THREAD_RETVAL stdin_thread(void *arg)
{
	(void)arg;
	while (getchar() != EOF) {
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *name = "Vcast Test";
	unsigned short port_base = 0;
	int seconds = 0;
	struct vcast_airplay_options opts;
	vcast_airplay_default_options(&opts);

	if (argc == 4 && !strcmp(argv[1], "--fpdecrypt"))
		return fpdecrypt(argv[2], argv[3]);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc)
			name = argv[++i];
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			port_base = (unsigned short)atoi(argv[++i]);
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v"))
			opts.log_level = VCAST_AIRPLAY_LOG_DEBUG;
		else if (!strcmp(argv[i], "-pairing"))
			opts.enable_pairing = 1;
		else if (!strcmp(argv[i], "-h265"))
			opts.enable_h265 = 1;
		else if (!strcmp(argv[i], "-video") && i + 1 < argc)
			video_file = fopen(argv[++i], "wb");
		else {
			fprintf(stderr, "usage: %s [-n name] [-p port_base] [-t seconds] [-v] [-pairing] [-h265] "
					"[-video file]\n",
				argv[0]);
			return 2;
		}
	}

	struct vcast_airplay_callbacks cb;
	memset(&cb, 0, sizeof(cb));
	cb.on_connect = on_connect;
	cb.on_disconnect = on_disconnect;
	cb.on_video = on_video;
	cb.on_audio_format = on_audio_format;
	cb.on_audio = on_audio;
	cb.on_volume = on_volume;
	cb.on_log = on_log;

	char err[256];
	vcast_airplay_t *ap = vcast_airplay_create_ex(name, port_base, &cb, &opts, err, sizeof(err));
	if (!ap) {
		out("ERROR %s\n", err);
		return 1;
	}
	char id[32];
	out("READY port=%u deviceid=%s\n", (unsigned)vcast_airplay_get_port(ap),
	    vcast_airplay_get_device_id(ap, id, sizeof(id)));

	thread_handle_t t;
	THREAD_CREATE(t, stdin_thread, NULL);
	(void)t;
	for (int ms = 0; !seconds || ms < seconds * 1000; ms += 100) {
		sleepms(100);
		/* stdin thread exits at EOF; poll it cheaply via feof */
		if (feof(stdin))
			break;
	}
	uint64_t t0 = vcast_airplay_now_ns();
	vcast_airplay_destroy(ap);
	out("DESTROYED in %llu ms (video frames %lu, audio frames %lu)\n",
	    (unsigned long long)((vcast_airplay_now_ns() - t0) / 1000000), video_frames, audio_frames);
	if (video_file)
		fclose(video_file);
	return 0;
}
