/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *=================================================================
 * modified by fduncanh 2022
 * modified for Vcast 2026: native Win32 threads (no pthreads/unistd.h on
 * Windows), relative timed condition wait, static mutex initializer and a
 * wall-clock helper, so the library builds with MSVC.
 */

#ifndef THREADS_H
#define THREADS_H

#include <stdint.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

/* Thread functions keep the POSIX signature void *(*)(void *); a small
 * trampoline (compat.c) adapts them to _beginthreadex. */
typedef HANDLE thread_handle_t;
typedef void *(*vcast_thread_func_t)(void *);
int vcast_thread_create(thread_handle_t *handle, vcast_thread_func_t func, void *arg);
void vcast_thread_join(thread_handle_t handle);

#define THREAD_RETVAL void *
#define THREAD_CREATE(handle, func, arg) \
	if (vcast_thread_create(&(handle), (func), (arg))) handle = 0
#define THREAD_JOIN(handle) vcast_thread_join(handle)

#define sleepms(x) Sleep((DWORD) (x))

typedef SRWLOCK mutex_handle_t;
typedef CONDITION_VARIABLE cond_handle_t;

#define MUTEX_INITIALIZER SRWLOCK_INIT
#define MUTEX_CREATE(handle) InitializeSRWLock(&(handle))
#define MUTEX_LOCK(handle) AcquireSRWLockExclusive(&(handle))
#define MUTEX_UNLOCK(handle) ReleaseSRWLockExclusive(&(handle))
#define MUTEX_DESTROY(handle) ((void) 0)

#define COND_CREATE(handle) InitializeConditionVariable(&(handle))
#define COND_SIGNAL(handle) WakeConditionVariable(&(handle))
#define COND_DESTROY(handle) ((void) 0)
/* wait (mutex held) for at most ms milliseconds */
#define COND_TIMEDWAIT_MS(cond, mutex, ms) \
	SleepConditionVariableSRW(&(cond), &(mutex), (DWORD) (ms), 0)

#else /* POSIX */

#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define sleepms(x) usleep((x)*1000)

typedef pthread_t thread_handle_t;

#define THREAD_RETVAL void *
#define THREAD_CREATE(handle, func, arg) \
	if (pthread_create(&(handle), NULL, func, arg)) handle = 0
#define THREAD_JOIN(handle) pthread_join(handle, NULL)

typedef pthread_mutex_t mutex_handle_t;

typedef pthread_cond_t cond_handle_t;

#define MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define MUTEX_CREATE(handle) pthread_mutex_init(&(handle), NULL)
#define MUTEX_LOCK(handle) pthread_mutex_lock(&(handle))
#define MUTEX_UNLOCK(handle) pthread_mutex_unlock(&(handle))
#define MUTEX_DESTROY(handle) pthread_mutex_destroy(&(handle))

#define COND_CREATE(handle) pthread_cond_init(&(handle), NULL)
#define COND_SIGNAL(handle) pthread_cond_signal(&(handle))
#define COND_DESTROY(handle) pthread_cond_destroy(&(handle))
/* wait (mutex held) for at most ms milliseconds */
#define COND_TIMEDWAIT_MS(cond, mutex, ms) vcast_cond_timedwait_ms(&(cond), &(mutex), (ms))
int vcast_cond_timedwait_ms(pthread_cond_t *cond, pthread_mutex_t *mutex, unsigned int ms);

#endif /* _WIN32 */

/* Wall clock (Unix epoch, nanoseconds), the clock UxPlay uses for NTP. */
uint64_t vcast_realtime_ns(void);

#endif /* THREADS_H */
