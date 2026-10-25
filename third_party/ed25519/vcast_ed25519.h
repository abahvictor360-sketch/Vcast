/*
 * Vcast: include orlp/ed25519 under a private prefix.
 *
 * The vendored orlp/ed25519 sources are compiled with the same renames as
 * compile definitions (see cmake/vcast/airplay.cmake), because UxPlay's own
 * crypto.h already uses the names ed25519_sign/ed25519_verify, and because
 * names like sha512_init are too generic to export from a static library.
 * Keep this list in sync with VCAST_ED25519_RENAMES in airplay.cmake.
 */
#ifndef VCAST_ED25519_H
#define VCAST_ED25519_H

#define ED25519_NO_SEED 1
#define ed25519_create_keypair vcast_orlp_ed25519_create_keypair
#define ed25519_sign vcast_orlp_ed25519_sign
#define ed25519_verify vcast_orlp_ed25519_verify
#define ed25519_add_scalar vcast_orlp_ed25519_add_scalar
#define ed25519_key_exchange vcast_orlp_ed25519_key_exchange
#define sha512_init vcast_orlp_sha512_init
#define sha512_update vcast_orlp_sha512_update
#define sha512_final vcast_orlp_sha512_final
#define sha512 vcast_orlp_sha512

#include "src/ed25519.h"

/* Undo the renames again (UxPlay's crypto.h defines ed25519_sign/verify with
 * other signatures); callers use the vcast_orlp_ names directly. */
#undef ed25519_create_keypair
#undef ed25519_sign
#undef ed25519_verify
#undef ed25519_add_scalar
#undef ed25519_key_exchange
#undef sha512_init
#undef sha512_update
#undef sha512_final
#undef sha512

#endif
