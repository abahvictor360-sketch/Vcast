# Third-party code (AirPlay receiver)

Built by `cmake/vcast/airplay.cmake` into the static library `vcast-airplay`
(API: `src/airplay/vcast-airplay.h`). Warnings are disabled for these sources and
they are never built with warnings-as-errors.

| Directory | Upstream | Version | License |
|---|---|---|---|
| `uxplay/` | https://github.com/FDH2/UxPlay (`lib/` only) | commit `116e39069e40f09350a372233937a55c890a02f8` (2026-10-24) | GPL-3.0-or-later (`uxplay/LICENSE`); several files LGPL-2.1-or-later, llhttp MIT (`uxplay/lib/llhttp/LICENSE-MIT`), playfair GPL-3.0 (`uxplay/lib/playfair/LICENSE.md`), srp.c MIT |
| `libplist/` | https://github.com/libimobiledevice/libplist (C library, libcnary) | tag `2.7.0` (`cf5897a71ea412ea2aeb1e2f6b5ea74d4fabfd8c`) | LGPL-2.1-or-later (`libplist/COPYING`, `COPYING.LESSER`) |
| `ed25519/` | https://github.com/orlp/ed25519 (`src/`, signing/verification only) | commit `b1f19fab4aebe607805620d25a5e42566ce46a0e` | zlib (`ed25519/license.txt`) |

Because UxPlay is GPL-3.0, a build that includes `vcast-airplay` must be
distributed under the GPL-3.0-or-later.

The first commit that added this directory contains the unmodified upstream
files; `git diff` against it shows every Vcast change. Modified files carry a
"modified for Vcast 2026" note in their header.

## Changes to UxPlay `lib/`

Removed: `CMakeLists.txt` files (replaced by `cmake/vcast/airplay.cmake`).

No OpenSSL:
- `crypto.c`/`crypto.h`: reimplemented on mbedTLS 3.6 (AES-128 CTR/CBC, AES-GCM
  with 16-byte IV, SHA-512, MD5, X25519 via Curve25519 ECDH, CTR-DRBG, base64)
  and orlp/ed25519 (Ed25519). Same functions and behaviour (checked against the
  original by `tests/airplay/crypto-test.c`), except: errors are logged instead
  of calling `exit()`; AES-CBC ignores a trailing partial block; no PEM key file
  support (keys are always derived from the device id, as Vcast uses them);
  `x25519_derive_secret()` returns an error code; `x25519_key_from_private_raw()`,
  `ed25519_key_from_seed()` and `crypto_random()` added.
- `srp.c`: ported to `mbedtls_mpi`/`mbedtls_md` (verifier side only; the client
  side was already compiled out). Verifier output is always 256 bytes and the
  salt always has its top bit set, so pairing.c's size checks cannot fail.
- `pairing.c`: no `<openssl/sha.h>`; SRP buffers freed (memory leak).

Portability (MSVC, no pthreads/unistd on Windows):
- `threads.h`: Win32 implementation (SRWLOCK, CONDITION_VARIABLE,
  `_beginthreadex` trampoline in `compat.c`); `COND_TIMEDWAIT_MS`,
  `MUTEX_INITIALIZER` and `vcast_realtime_ns()` added; `raop_ntp.c` uses them
  instead of `pthread_cond_timedwait`/`clock_gettime`.
- `win_types.h` (new): `ssize_t`, `strcasecmp`, `strncasecmp` for MSVC;
  `http_request.c` includes `<strings.h>` on POSIX.
- `compat.h`: no `#define snprintf _snprintf`; `compat.c`: thread-safe
  `wsa_strerror` using `FormatMessageA` (OBS builds with `UNICODE`).
- `WIN32` checks changed to `_WIN32` (MSVC and `-std=c11` MinGW do not define `WIN32`).
- `raop_rtp_mirror.c`: Windows `SO_RCVTIMEO` takes a DWORD of milliseconds (the
  timeval made it infinite); keep-alive macros only if the SDK lacks them.
- `mdnsd/mdnsd.c`: Winsock multicast options as DWORDs; no `in6addr_any`.
- `netutils.c`: no `in6addr_any`; `SO_EXCLUSIVEADDRUSE` on Windows instead of
  `SO_REUSEADDR` (which lets a socket silently share a busy port); no
  `SO_REUSEADDR` on UDP sockets.
- `raop_ntp.c`: the Windows `SIO_TIMESTAMPING` path is compiled only with
  `VCAST_WIN_KERNEL_TIMESTAMPS` (it logged on every packet; untested).
- `llhttp/llhttp.h`: `LLHTTP_EXPORT` can be overridden (no dllexport from a static lib).

Robustness:
- `raop_rtp_stop`, `raop_ntp_stop`, `raop_rtp_mirror_stop`, `httpd_stop` also
  join a thread that ended on its own (before, its sockets stayed open and the
  service could not restart; fixed ports stayed bound).
- `raop_rtp_mirror.c`: the mirroring thread no longer calls
  `raop_rtp_mirror_stop()` (a self-join) on an unsupported codec.
- `raop_ntp.c`: the NTP thread cannot miss a stop request and sleep 3 s.
- `httpd.c`: `httpd_destroy()` closes the client connections (their threads ran
  on after `raop_destroy()`).
- `mirror_buffer.c`: a mirroring payload shorter than the keystream left over
  from the previous one no longer overruns the buffers (found with ASan).
- `fairplay_playfair.c`: FairPlay mode bytes outside 0..3 are rejected (they
  indexed 4-entry tables out of bounds).
- `raop_handlers.h`: the SRP public key buffer is freed.

mDNS:
- `mdnsd/dnssd_mdnsd.c`: the built-in responder announces its own host name,
  `<computer>-AirPlay-<id>.local`, instead of the computer's own, which Avahi or
  Windows' mDNS already announce (it does no conflict probing).
- `mdnsd/mdnsd.c`: announcements are repeated after 1 s and 3 s.
- `dns_sd/dns_sd.c` (used on macOS only): a private thread services the
  registrations with `DNSServiceProcessResult()`.

## Changes to libplist

- `src/bplist.c`: `typeof` → `__typeof__` (GNU path, for strict C modes).
- Built with `LIBPLIST_STATIC`, `PACKAGE_VERSION="2.7.0"` and, on POSIX,
  `HAVE_STRNDUP HAVE_GMTIME_R HAVE_LOCALTIME_R HAVE_MEMMEM` (no `config.h`).

## orlp/ed25519

Unmodified; `seed.c`, `key_exchange.c` and `add_scalar.c` not included. Its
public names are renamed with a `vcast_orlp_` prefix at compile time
(`vcast_ed25519.h` and `VCAST_ED25519_RENAMES` in `airplay.cmake`), because
UxPlay already defines `ed25519_sign`/`ed25519_verify`.
- raop_rtp_mirror.c: define SOL_TCP as IPPROTO_TCP on Windows (MSVC lacks it).
