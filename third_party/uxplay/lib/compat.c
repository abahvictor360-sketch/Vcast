/*
 * Copyright (c) 2024 F. Duncanh, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *==================================================================
 * modified for Vcast 2026: Win32 thread trampoline, relative condition wait
 * and the wall-clock helper declared in threads.h.
 */

#include <stdlib.h>
#include <string.h>
#include "compat.h"

#define SECOND_IN_NSECS_U64 1000000000ULL

#ifdef _WIN32
#include <process.h>

#define MAX_SOCKET_ERROR_MESSAGE_LENGTH 256

/* Windows (winsock2) socket error message text */
char *wsa_strerror(int errnum) {
    /* thread-local: several UxPlay threads may log socket errors at once */
#if defined(_MSC_VER)
    static __declspec(thread) char message[MAX_SOCKET_ERROR_MESSAGE_LENGTH];
#else
    static __thread char message[MAX_SOCKET_ERROR_MESSAGE_LENGTH];
#endif
    message[0] = '\0';
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                   0, (DWORD) errnum, 0, message, sizeof(message), 0);
    char *nl = strchr(message, '\r');
    if (nl) {
        *nl = 0;  /* remove any trailing newline, or truncate to one line */
    }
    nl = strchr(message, '\n');
    if (nl) {
        *nl = 0;
    }
    return message;
}

typedef struct vcast_thread_start_s {
    vcast_thread_func_t func;
    void *arg;
} vcast_thread_start_t;

static unsigned __stdcall vcast_thread_trampoline(void *param) {
    vcast_thread_start_t start = *(vcast_thread_start_t *) param;
    free(param);
    start.func(start.arg);
    return 0;
}

int vcast_thread_create(thread_handle_t *handle, vcast_thread_func_t func, void *arg) {
    vcast_thread_start_t *start = (vcast_thread_start_t *) malloc(sizeof(*start));
    uintptr_t h;
    if (!start) {
        return -1;
    }
    start->func = func;
    start->arg = arg;
    h = _beginthreadex(NULL, 0, vcast_thread_trampoline, start, 0, NULL);
    if (h == 0) {
        free(start);
        return -1;
    }
    *handle = (thread_handle_t) h;
    return 0;
}

void vcast_thread_join(thread_handle_t handle) {
    if (handle) {
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
    }
}

uint64_t vcast_realtime_ns(void) {
    FILETIME ft;
    ULARGE_INTEGER t;
    /* 100 ns ticks since 1601-01-01 */
    GetSystemTimePreciseAsFileTime(&ft);
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (t.QuadPart - 116444736000000000ULL) * 100ULL;
}

#else /* POSIX */

#include <errno.h>

int vcast_cond_timedwait_ms(pthread_cond_t *cond, pthread_mutex_t *mutex, unsigned int ms) {
    struct timespec wait_time;
    clock_gettime(CLOCK_REALTIME, &wait_time);
    wait_time.tv_sec += ms / 1000;
    wait_time.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (wait_time.tv_nsec >= 1000000000L) {
        wait_time.tv_sec += 1;
        wait_time.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, mutex, &wait_time);
}

uint64_t vcast_realtime_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return ((uint64_t) time.tv_nsec) + (uint64_t) time.tv_sec * SECOND_IN_NSECS_U64;
}

#endif
