/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Copyright (C) 2020 Jaslo Ziska
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 *===================================================================
 * modified by fduncanh 2021-2022
 * rewritten for Vcast 2026 on mbedTLS 3.6 (AES, GCM, SHA-512, MD5, X25519,
 * CTR-DRBG, base64) and orlp/ed25519 (Ed25519), replacing OpenSSL. The
 * behaviour of every function matches the OpenSSL version (verified by
 * tests/airplay/crypto-test.c against the original), except:
 *  - errors are logged to stderr and reported, never exit() the process;
 *  - AES-CBC ignores a trailing partial block (OpenSSL aborted the program);
 *  - ed25519_key_generate() does not read or write PEM key files: the key is
 *    always derived from the device id (the keyfile argument is ignored).
 */

#include "crypto.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha512.h>
#include <mbedtls/md5.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/bignum.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>

#include "vcast_ed25519.h"
#include "threads.h"
#include "utils.h"

#define SALT_PK "UxPlay-Persistent-Not-Secure-Public-Key"
#define SHA512_DIGEST_LENGTH 64
#define MD5_DIGEST_LENGTH 16

static void crypto_error(const char *location, int ret) {
    fprintf(stderr, "Crypto error at %s: mbedTLS error -0x%04x\n", location, (unsigned int) -ret);
}

// Random numbers: one process-wide CTR-DRBG seeded from the platform entropy
// source. mbedTLS is built without MBEDTLS_THREADING_C, so it is serialized here.

static mutex_handle_t rng_mutex = MUTEX_INITIALIZER;
static bool rng_seeded = false;
static mbedtls_entropy_context rng_entropy;
static mbedtls_ctr_drbg_context rng_drbg;

int crypto_random(void *unused, unsigned char *buf, size_t len) {
    int ret = 0;
    (void) unused;
    MUTEX_LOCK(rng_mutex);
    if (!rng_seeded) {
        static const char personalization[] = "vcast-airplay";
        mbedtls_entropy_init(&rng_entropy);
        mbedtls_ctr_drbg_init(&rng_drbg);
        ret = mbedtls_ctr_drbg_seed(&rng_drbg, mbedtls_entropy_func, &rng_entropy,
                                    (const unsigned char *) personalization, sizeof(personalization) - 1);
        if (ret == 0) {
            rng_seeded = true;
        } else {
            mbedtls_ctr_drbg_free(&rng_drbg);
            mbedtls_entropy_free(&rng_entropy);
            crypto_error(__func__, ret);
        }
    }
    if (rng_seeded) {
        /* mbedtls_ctr_drbg_random is limited to MBEDTLS_CTR_DRBG_MAX_REQUEST per call */
        while (len > 0 && ret == 0) {
            size_t n = len > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : len;
            ret = mbedtls_ctr_drbg_random(&rng_drbg, buf, n);
            buf += n;
            len -= n;
        }
        if (ret) {
            crypto_error(__func__, ret);
        }
    }
    MUTEX_UNLOCK(rng_mutex);
    return ret;
}

int get_random_bytes(unsigned char *buf, int num) {
    if (num < 0) {
        return 0;
    }
    return crypto_random(NULL, buf, (size_t) num) == 0 ? 1 : 0;
}

// AES (128 bit), CTR and CBC modes, no padding

typedef enum aes_mode_e { AES_MODE_CTR, AES_MODE_CBC } aes_mode_t;

struct aes_ctx_s {
    mbedtls_aes_context aes;
    aes_mode_t mode;
    uint8_t key[AES_128_BLOCK_SIZE];
    uint8_t iv[AES_128_BLOCK_SIZE];
    aes_direction_t direction;
    uint8_t block_offset;
    /* running state: CTR counter block + keystream position, or CBC chaining value */
    uint8_t state[AES_128_BLOCK_SIZE];
    uint8_t stream_block[AES_128_BLOCK_SIZE];
    size_t nc_off;
};

static aes_ctx_t *aes_init(const uint8_t *key, const uint8_t *iv, aes_mode_t mode, aes_direction_t direction) {
    int ret;
    aes_ctx_t *ctx = calloc(1, sizeof(aes_ctx_t));
    assert(ctx != NULL);
    if (!ctx) {
        return NULL;
    }
    mbedtls_aes_init(&ctx->aes);
    ctx->mode = mode;
    ctx->block_offset = 0;
    ctx->direction = direction;
    memcpy(ctx->key, key, AES_128_BLOCK_SIZE);
    memcpy(ctx->iv, iv, AES_128_BLOCK_SIZE);
    memcpy(ctx->state, iv, AES_128_BLOCK_SIZE);
    ctx->nc_off = 0;
    /* CTR mode only ever uses the forward cipher */
    if (mode == AES_MODE_CBC && direction == AES_DECRYPT) {
        ret = mbedtls_aes_setkey_dec(&ctx->aes, key, 128);
    } else {
        ret = mbedtls_aes_setkey_enc(&ctx->aes, key, 128);
    }
    if (ret) {
        crypto_error(__func__, ret);
    }
    return ctx;
}

static void aes_reset(aes_ctx_t *ctx) {
    memcpy(ctx->state, ctx->iv, AES_128_BLOCK_SIZE);
    memset(ctx->stream_block, 0, sizeof(ctx->stream_block));
    ctx->nc_off = 0;
}

static void aes_destroy(aes_ctx_t *ctx) {
    if (ctx) {
        mbedtls_aes_free(&ctx->aes);
        mbedtls_platform_zeroize(ctx, sizeof(*ctx));
        free(ctx);
    }
}

static void aes_ctr_crypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    if (len <= 0) {
        return;
    }
    /* like OpenSSL's EVP CTR mode, the keystream continues across calls,
     * including from the middle of a block */
    int ret = mbedtls_aes_crypt_ctr(&ctx->aes, (size_t) len, &ctx->nc_off, ctx->state,
                                    ctx->stream_block, in, out);
    if (ret) {
        crypto_error(__func__, ret);
    }
}

// AES CTR

aes_ctx_t *aes_ctr_init(const uint8_t *key, const uint8_t *iv) {
    return aes_init(key, iv, AES_MODE_CTR, AES_ENCRYPT);
}

void aes_ctr_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    aes_ctr_crypt(ctx, in, out, len);
    ctx->block_offset = (ctx->block_offset + len) % AES_128_BLOCK_SIZE;
}

void aes_ctr_start_fresh_block(aes_ctx_t *ctx) {
    uint8_t waste[AES_128_BLOCK_SIZE] = { 0 };
    if (ctx->block_offset == 0) return;
    aes_ctr_encrypt(ctx, waste, waste, AES_128_BLOCK_SIZE - ctx->block_offset);
}

void aes_ctr_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    /* (as in the original, decryption does not update block_offset) */
    aes_ctr_crypt(ctx, in, out, len);
}

void aes_ctr_reset(aes_ctx_t *ctx) {
    aes_reset(ctx);
}

void aes_ctr_destroy(aes_ctx_t *ctx) {
    aes_destroy(ctx);
}

// AES CBC

aes_ctx_t *aes_cbc_init(const uint8_t *key, const uint8_t *iv, aes_direction_t direction) {
    return aes_init(key, iv, AES_MODE_CBC, direction);
}

static void aes_cbc_crypt(aes_ctx_t *ctx, int mode, const uint8_t *in, uint8_t *out, int len) {
    if (len <= 0) {
        return;
    }
    /* without padding only whole blocks can be processed */
    size_t whole = ((size_t) len / AES_128_BLOCK_SIZE) * AES_128_BLOCK_SIZE;
    if (whole != (size_t) len) {
        fprintf(stderr, "Crypto error at %s: AES-CBC length %d is not a multiple of 16\n", __func__, len);
    }
    if (whole) {
        int ret = mbedtls_aes_crypt_cbc(&ctx->aes, mode, whole, ctx->state, in, out);
        if (ret) {
            crypto_error(__func__, ret);
        }
    }
}

void aes_cbc_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    assert(ctx->direction == AES_ENCRYPT);
    aes_cbc_crypt(ctx, MBEDTLS_AES_ENCRYPT, in, out, len);
}

void aes_cbc_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    assert(ctx->direction == AES_DECRYPT);
    aes_cbc_crypt(ctx, MBEDTLS_AES_DECRYPT, in, out, len);
}

void aes_cbc_reset(aes_ctx_t *ctx) {
    aes_reset(ctx);
}

void aes_cbc_destroy(aes_ctx_t *ctx) {
    aes_destroy(ctx);
}

// X25519 (RFC 7748), via mbedTLS's Curve25519 ECDH

struct x25519_key_s {
    unsigned char pub[X25519_KEY_SIZE];
    unsigned char priv[X25519_KEY_SIZE];   /* clamped scalar, little endian */
    bool has_priv;
};

/* public = X25519(priv, 9) */
static int x25519_compute_public(x25519_key_t *key) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d;
    size_t olen = 0;
    int ret;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (!ret) ret = mbedtls_mpi_read_binary_le(&d, key->priv, X25519_KEY_SIZE);
    if (!ret) ret = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, crypto_random, NULL);
    if (!ret) ret = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen,
                                                   key->pub, X25519_KEY_SIZE);
    if (!ret && olen != X25519_KEY_SIZE) ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    if (ret) {
        crypto_error(__func__, ret);
    }
    return ret;
}

static void x25519_clamp(unsigned char priv[X25519_KEY_SIZE]) {
    priv[0] &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
}

x25519_key_t *x25519_key_from_private_raw(const unsigned char data[X25519_KEY_SIZE]) {
    x25519_key_t *key = (x25519_key_t *) calloc(1, sizeof(x25519_key_t));
    assert(key);
    if (!key) {
        return NULL;
    }
    memcpy(key->priv, data, X25519_KEY_SIZE);
    x25519_clamp(key->priv);
    key->has_priv = true;
    x25519_compute_public(key);
    return key;
}

x25519_key_t *x25519_key_generate(void) {
    unsigned char priv[X25519_KEY_SIZE];
    if (crypto_random(NULL, priv, sizeof(priv))) {
        crypto_error(__func__, -1);
    }
    x25519_key_t *key = x25519_key_from_private_raw(priv);
    mbedtls_platform_zeroize(priv, sizeof(priv));
    return key;
}

x25519_key_t *x25519_key_from_raw(const unsigned char data[X25519_KEY_SIZE]) {
    x25519_key_t *key = (x25519_key_t *) calloc(1, sizeof(x25519_key_t));
    assert(key);
    if (!key) {
        return NULL;
    }
    memcpy(key->pub, data, X25519_KEY_SIZE);
    key->has_priv = false;
    return key;
}

void x25519_key_get_raw(unsigned char data[X25519_KEY_SIZE], const x25519_key_t *key) {
    assert(key);
    memcpy(data, key->pub, X25519_KEY_SIZE);
}

void x25519_key_destroy(x25519_key_t *key) {
    if (key) {
        mbedtls_platform_zeroize(key, sizeof(*key));
        free(key);
    }
}

int x25519_derive_secret(unsigned char secret[X25519_KEY_SIZE], const x25519_key_t *ours, const x25519_key_t *theirs) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Qp;
    mbedtls_mpi d, z;
    int ret;

    assert(ours);
    assert(theirs);
    memset(secret, 0, X25519_KEY_SIZE);
    if (!ours->has_priv) {
        crypto_error(__func__, MBEDTLS_ERR_ECP_BAD_INPUT_DATA);
        return -1;
    }

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Qp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    /* reading the u-coordinate masks its most significant bit, as RFC 7748 requires */
    if (!ret) ret = mbedtls_ecp_point_read_binary(&grp, &Qp, theirs->pub, X25519_KEY_SIZE);
    if (!ret) ret = mbedtls_mpi_read_binary_le(&d, ours->priv, X25519_KEY_SIZE);
    if (!ret) ret = mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, crypto_random, NULL);
    if (!ret) ret = mbedtls_mpi_write_binary_le(&z, secret, X25519_KEY_SIZE);
    if (!ret) {
        /* an all-zero result means a small-order peer key (OpenSSL rejects it too) */
        unsigned char acc = 0;
        for (int i = 0; i < X25519_KEY_SIZE; i++) {
            acc |= secret[i];
        }
        if (!acc) ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Qp);
    mbedtls_ecp_group_free(&grp);
    if (ret) {
        memset(secret, 0, X25519_KEY_SIZE);
        crypto_error(__func__, ret);
        return -1;
    }
    return 0;
}

// GCM AES 128 (16 byte IV, 16 byte tag, no AAD)

int gcm_encrypt(const unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext,
                unsigned char *key, unsigned char *iv, unsigned char *tag)
{
    mbedtls_gcm_context ctx;
    int ret;
    if (plaintext_len < 0) {
        return -1;
    }
    mbedtls_gcm_init(&ctx);
    ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (!ret) ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, (size_t) plaintext_len,
                                              iv, 16, NULL, 0, plaintext, ciphertext, 16, tag);
    mbedtls_gcm_free(&ctx);
    if (ret) {
        crypto_error(__func__, ret);
        return -1;
    }
    return plaintext_len;
}

int gcm_decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext,
                unsigned char *key, unsigned char *iv, unsigned char *tag)
{
    mbedtls_gcm_context ctx;
    int ret;
    if (ciphertext_len < 0) {
        return -1;
    }
    mbedtls_gcm_init(&ctx);
    ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (!ret) ret = mbedtls_gcm_auth_decrypt(&ctx, (size_t) ciphertext_len, iv, 16, NULL, 0,
                                             tag, 16, ciphertext, plaintext);
    mbedtls_gcm_free(&ctx);
    if (ret) {
        /* Verify failed */
        return -1;
    }
    return ciphertext_len;
}

// ED25519 (RFC 8032), via orlp/ed25519

struct ed25519_key_s {
    unsigned char pub[ED25519_KEY_SIZE];   /* first member: pairing_session_make_nonce() hashes it */
    unsigned char priv[64];                /* orlp's expanded private key */
    bool has_priv;
};

ed25519_key_t *ed25519_key_from_seed(const unsigned char seed[ED25519_KEY_SIZE]) {
    ed25519_key_t *key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(key);
    if (!key) {
        return NULL;
    }
    vcast_orlp_ed25519_create_keypair(key->pub, key->priv, seed);
    key->has_priv = true;
    return key;
}

ed25519_key_t *ed25519_key_generate(const char *device_id, const char *keyfile, int *result) {
    unsigned char hash[SHA512_DIGEST_LENGTH] = {0};
    *result = 0;

    if (keyfile && strlen(keyfile)) {
        fprintf(stderr, "Crypto: key files are not supported in this build; "
                "the key is derived from the device id\n");
    }
    /* generate (insecure) persistent keypair using device_id */
    const char salt[] = SALT_PK;
    sha_ctx_t *ctx = sha_init();
    sha_update(ctx, (const unsigned char *) salt, (int) strlen(salt));
    sha_update(ctx, (const unsigned char *) device_id, (int) strlen(device_id));
    sha_final(ctx, hash, NULL);
    sha_destroy(ctx);
    ed25519_key_t *key = ed25519_key_from_seed(hash);   /* the first 32 bytes are the seed */
    mbedtls_platform_zeroize(hash, sizeof(hash));
    return key;
}

ed25519_key_t *ed25519_key_from_raw(const unsigned char data[ED25519_KEY_SIZE]) {
    ed25519_key_t *key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(key);
    if (!key) {
        return NULL;
    }
    memcpy(key->pub, data, ED25519_KEY_SIZE);
    key->has_priv = false;
    return key;
}

void ed25519_key_get_raw(unsigned char data[ED25519_KEY_SIZE], const ed25519_key_t *key) {
    assert(key);
    memcpy(data, key->pub, ED25519_KEY_SIZE);
}

ed25519_key_t *ed25519_key_copy(const ed25519_key_t *key) {
    assert(key);
    ed25519_key_t *new_key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(new_key);
    if (new_key) {
        memcpy(new_key, key, sizeof(ed25519_key_t));
    }
    return new_key;
}

void ed25519_sign(unsigned char *signature, size_t signature_len,
                  const unsigned char *data, size_t data_len,
                  const ed25519_key_t *key)
{
    unsigned char sig[64];
    assert(key);
    if (!key->has_priv || signature_len < sizeof(sig)) {
        crypto_error(__func__, -1);
        memset(signature, 0, signature_len);
        return;
    }
    vcast_orlp_ed25519_sign(sig, data, data_len, key->pub, key->priv);
    memcpy(signature, sig, sizeof(sig));
}

int ed25519_verify(const unsigned char *signature, size_t signature_len,
                   const unsigned char *data, size_t data_len,
                   const ed25519_key_t *key)
{
    assert(key);
    if (signature_len != 64) {
        return 0;
    }
    return vcast_orlp_ed25519_verify(signature, data, data_len, key->pub) == 1 ? 1 : 0;
}

void ed25519_key_destroy(ed25519_key_t *key) {
    if (key) {
        mbedtls_platform_zeroize(key, sizeof(*key));
        free(key);
    }
}

// SHA 512

struct sha_ctx_s {
    mbedtls_sha512_context digest_ctx;
};

sha_ctx_t *sha_init(void) {
    sha_ctx_t *ctx = (sha_ctx_t *) calloc(1, sizeof(sha_ctx_t));
    assert(ctx != NULL);
    mbedtls_sha512_init(&ctx->digest_ctx);
    int ret = mbedtls_sha512_starts(&ctx->digest_ctx, 0);
    if (ret) {
        crypto_error(__func__, ret);
    }
    return ctx;
}

void sha_update(sha_ctx_t *ctx, const uint8_t *in, int len) {
    if (len <= 0) {
        return;
    }
    int ret = mbedtls_sha512_update(&ctx->digest_ctx, in, (size_t) len);
    if (ret) {
        crypto_error(__func__, ret);
    }
}

void sha_final(sha_ctx_t *ctx, uint8_t *out, unsigned int *len) {
    int ret = mbedtls_sha512_finish(&ctx->digest_ctx, out);
    if (ret) {
        crypto_error(__func__, ret);
    }
    if (len) {
        *len = SHA512_DIGEST_LENGTH;
    }
}

void sha_reset(sha_ctx_t *ctx) {
    mbedtls_sha512_free(&ctx->digest_ctx);
    mbedtls_sha512_init(&ctx->digest_ctx);
    int ret = mbedtls_sha512_starts(&ctx->digest_ctx, 0);
    if (ret) {
        crypto_error(__func__, ret);
    }
}

void sha_destroy(sha_ctx_t *ctx) {
    if (ctx) {
        mbedtls_sha512_free(&ctx->digest_ctx);
        free(ctx);
    }
}

//MD5
struct md5_ctx_s {
    mbedtls_md5_context digest_ctx;
};

md5_ctx_t *md5_init(void) {
    md5_ctx_t *ctx = (md5_ctx_t *) calloc(1, sizeof(md5_ctx_t));
    assert(ctx != NULL);
    mbedtls_md5_init(&ctx->digest_ctx);
    int ret = mbedtls_md5_starts(&ctx->digest_ctx);
    if (ret) {
        crypto_error(__func__, ret);
    }
    return ctx;
}

void md5_update(md5_ctx_t *ctx, const uint8_t *in, int len) {
    if (len <= 0) {
        return;
    }
    int ret = mbedtls_md5_update(&ctx->digest_ctx, in, (size_t) len);
    if (ret) {
        crypto_error(__func__, ret);
    }
}

void md5_final(md5_ctx_t *ctx, uint8_t *out, unsigned int *len) {
    int ret = mbedtls_md5_finish(&ctx->digest_ctx, out);
    if (ret) {
        crypto_error(__func__, ret);
    }
    if (len) {
        *len = MD5_DIGEST_LENGTH;
    }
}

void md5_reset(md5_ctx_t *ctx) {
    mbedtls_md5_free(&ctx->digest_ctx);
    mbedtls_md5_init(&ctx->digest_ctx);
    int ret = mbedtls_md5_starts(&ctx->digest_ctx);
    if (ret) {
        crypto_error(__func__, ret);
    }
}

void md5_destroy(md5_ctx_t *ctx) {
    if (ctx) {
        mbedtls_md5_free(&ctx->digest_ctx);
        free(ctx);
    }
}

char *get_md5(char *string) {
    unsigned char hash[MD5_DIGEST_LENGTH] = {0};
    md5_ctx_t *ctx = NULL;
    ctx = md5_init();
    md5_update(ctx, (const unsigned char *) string, (int) strlen(string));
    md5_final(ctx, hash, NULL);
    md5_destroy(ctx);
    ctx = NULL;
    char *result_str = utils_hex_to_string(hash, MD5_DIGEST_LENGTH);
    return result_str;   //must free result_str after use
}

void pk_to_base64(const unsigned char *pk, int pk_len, char *pk_base64, int len) {
    memset(pk_base64, 0, len);
    int len64 = (4 * (pk_len /3)) + (pk_len % 3 ? 4 : 0);

    assert (len > len64);
    if (pk_len < 0 || len <= len64) {
        return;
    }

    /* standard base64 with padding, no newlines; NUL-terminated */
    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *) pk_base64, (size_t) len, &olen, pk, (size_t) pk_len);
    if (ret || (int) olen != len64) {
        crypto_error(__func__, ret ? ret : -1);
        memset(pk_base64, 0, len);
    }
}
