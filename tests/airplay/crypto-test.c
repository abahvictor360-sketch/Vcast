/*
 * Tests for the mbedTLS/orlp port of UxPlay's crypto (crypto.c, srp.c, pairing.c):
 * published test vectors, and (when built with OpenSSL, VCAST_HAVE_OPENSSL_REFERENCE)
 * a comparison against the original OpenSSL implementation, including a full
 * pair-verify handshake with an OpenSSL client and an SRP pair-setup-pin.
 *
 * Copyright (C) 2026 Vcast contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <mbedtls/bignum.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha512.h>
#include <mbedtls/gcm.h>

#include "crypto.h"
#include "pairing.h"
#include "srp.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                  \
	do {                                              \
		checks++;                                 \
		if (!(cond)) {                            \
			failures++;                       \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);              \
			printf("\n");                     \
		}                                         \
	} while (0)

static size_t unhex(const char *hex, unsigned char *out)
{
	size_t n = strlen(hex) / 2;
	for (size_t i = 0; i < n; i++) {
		unsigned int b;
		sscanf(hex + 2 * i, "%2x", &b);
		out[i] = (unsigned char)b;
	}
	return n;
}

static bool eq_hex(const unsigned char *data, const char *hex)
{
	unsigned char buf[512];
	size_t n = unhex(hex, buf);
	return memcmp(data, buf, n) == 0;
}

#ifdef VCAST_HAVE_OPENSSL_REFERENCE
/* the original OpenSSL implementation (tests/airplay/reference, renamed) */
aes_ctx_t *ref_aes_ctr_init(const uint8_t *key, const uint8_t *iv);
void ref_aes_ctr_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void ref_aes_ctr_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void ref_aes_ctr_start_fresh_block(aes_ctx_t *ctx);
void ref_aes_ctr_reset(aes_ctx_t *ctx);
void ref_aes_ctr_destroy(aes_ctx_t *ctx);
aes_ctx_t *ref_aes_cbc_init(const uint8_t *key, const uint8_t *iv, aes_direction_t direction);
void ref_aes_cbc_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void ref_aes_cbc_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void ref_aes_cbc_reset(aes_ctx_t *ctx);
void ref_aes_cbc_destroy(aes_ctx_t *ctx);
int ref_gcm_encrypt(const unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext, unsigned char *key,
		    unsigned char *iv, unsigned char *tag);
int ref_gcm_decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext, unsigned char *key,
		    unsigned char *iv, unsigned char *tag);
x25519_key_t *ref_x25519_key_generate(void);
x25519_key_t *ref_x25519_key_from_raw(const unsigned char data[32]);
void ref_x25519_key_get_raw(unsigned char data[32], const x25519_key_t *key);
void ref_x25519_derive_secret(unsigned char secret[32], const x25519_key_t *ours, const x25519_key_t *theirs);
void ref_x25519_key_destroy(x25519_key_t *key);
ed25519_key_t *ref_ed25519_key_generate(const char *device_id, const char *keyfile, int *result);
ed25519_key_t *ref_ed25519_key_from_raw(const unsigned char data[32]);
void ref_ed25519_key_get_raw(unsigned char data[32], const ed25519_key_t *key);
void ref_ed25519_sign(unsigned char *signature, size_t signature_len, const unsigned char *data, size_t data_len,
		      const ed25519_key_t *key);
int ref_ed25519_verify(const unsigned char *signature, size_t signature_len, const unsigned char *data,
		       size_t data_len, const ed25519_key_t *key);
void ref_ed25519_key_destroy(ed25519_key_t *key);
void ref_pk_to_base64(const unsigned char *pk, int pk_len, char *pk_base64, int len);
char *ref_get_md5(char *string);
struct SRPVerifier *ref_srp_verifier_new(SRP_HashAlgorithm alg, SRP_NGType ng_type, const char *username,
					 const unsigned char *bytes_s, int len_s, const unsigned char *bytes_v,
					 int len_v, const unsigned char *bytes_A, int len_A,
					 const unsigned char *bytes_b, int len_b, const unsigned char **bytes_B,
					 int *len_B, const char *n_hex, const char *g_hex, int rfc5054_compat);
void ref_srp_verifier_delete(struct SRPVerifier *ver);
const unsigned char *ref_srp_verifier_get_session_key(struct SRPVerifier *ver, int *key_length);
void ref_srp_verifier_verify_session(struct SRPVerifier *ver, const unsigned char *user_M,
				     const unsigned char **bytes_HAMK);
int ref_srp_verifier_is_authenticated(struct SRPVerifier *ver);
void ref_srp_create_server_ephemeral_key(SRP_HashAlgorithm alg, SRP_NGType ng_type, const unsigned char *bytes_v,
					 int len_v, const unsigned char *bytes_b, int len_b,
					 const unsigned char **bytes_B, int *len_B, const char *n_hex,
					 const char *g_hex, int rfc5054_compat);
#endif

/* ------------------------------------------------------------------------- */

static const char *nist_key = "2b7e151628aed2a6abf7158809cf4f3c";
static const char *nist_pt = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
			     "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";

static void test_aes(void)
{
	unsigned char key[16], iv[16], pt[64], ct[64], out[64];
	unhex(nist_key, key);
	unhex(nist_pt, pt);

	/* SP 800-38A F.5.1 CTR-AES128, processed in odd-sized pieces */
	unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
	aes_ctx_t *ctr = aes_ctr_init(key, iv);
	aes_ctr_encrypt(ctr, pt, ct, 5);
	aes_ctr_encrypt(ctr, pt + 5, ct + 5, 20);
	aes_ctr_encrypt(ctr, pt + 25, ct + 25, 39);
	CHECK(eq_hex(ct, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
			 "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee"),
	      "AES-CTR vector");
	aes_ctr_reset(ctr);
	aes_ctr_decrypt(ctr, ct, out, 64);
	CHECK(!memcmp(out, pt, 64), "AES-CTR reset + decrypt");
	aes_ctr_destroy(ctr);

	/* SP 800-38A F.2.1/F.2.2 CBC-AES128 */
	unhex("000102030405060708090a0b0c0d0e0f", iv);
	aes_ctx_t *cbc = aes_cbc_init(key, iv, AES_ENCRYPT);
	aes_cbc_encrypt(cbc, pt, ct, 64);
	CHECK(eq_hex(ct, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
			 "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"),
	      "AES-CBC encrypt vector");
	aes_cbc_destroy(cbc);
	cbc = aes_cbc_init(key, iv, AES_DECRYPT);
	aes_cbc_decrypt(cbc, ct, out, 32);
	aes_cbc_decrypt(cbc, ct + 32, out + 32, 32); /* chaining continues */
	CHECK(!memcmp(out, pt, 64), "AES-CBC decrypt vector");
	aes_cbc_reset(cbc);
	memset(out, 0, sizeof(out));
	aes_cbc_decrypt(cbc, ct, out, 64);
	CHECK(!memcmp(out, pt, 64), "AES-CBC reset");
	aes_cbc_destroy(cbc);

#ifdef VCAST_HAVE_OPENSSL_REFERENCE
	/* random lengths and the mirror-buffer access pattern vs OpenSSL */
	unsigned char big[4096], a[4096], b[4096];
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = (unsigned char)(i * 7 + 3);
	aes_ctx_t *m = aes_ctr_init(key, iv), *r = ref_aes_ctr_init(key, iv);
	size_t off = 0;
	int lens[] = {1, 15, 16, 17, 31, 100, 3, 0, 255, 600};
	for (int i = 0; off < 2000; i++) {
		int len = lens[i % 10];
		if (i % 3 == 0) {
			aes_ctr_encrypt(m, big + off, a + off, len);
			ref_aes_ctr_encrypt(r, big + off, b + off, len);
			aes_ctr_start_fresh_block(m);
			ref_aes_ctr_start_fresh_block(r);
		} else {
			aes_ctr_decrypt(m, big + off, a + off, len);
			ref_aes_ctr_decrypt(r, big + off, b + off, len);
		}
		off += (size_t)len;
	}
	CHECK(!memcmp(a, b, off), "AES-CTR stream matches OpenSSL");
	aes_ctr_destroy(m);
	ref_aes_ctr_destroy(r);

	m = aes_cbc_init(key, iv, AES_DECRYPT);
	r = ref_aes_cbc_init(key, iv, AES_DECRYPT);
	for (int i = 0; i < 8; i++) {
		aes_cbc_decrypt(m, big + i * 160, a + i * 160, 160);
		aes_cbc_reset(m);
		ref_aes_cbc_decrypt(r, big + i * 160, b + i * 160, 160);
		ref_aes_cbc_reset(r);
	}
	CHECK(!memcmp(a, b, 1280), "AES-CBC matches OpenSSL");
	aes_cbc_destroy(m);
	ref_aes_cbc_destroy(r);
#endif
}

static void test_gcm(void)
{
	unsigned char key[16], iv[16], pt[32], ct[32], tag[16], out[32];
	unhex("feffe9928665731c6d6a8f9467308308", key);
	unhex("cafebabefacedbaddecaf888deadbeef", iv); /* 16-byte IV, as UxPlay uses */
	for (int i = 0; i < 32; i++)
		pt[i] = (unsigned char)i;
	CHECK(gcm_encrypt(pt, 32, ct, key, iv, tag) == 32, "GCM encrypt");

	/* independent check with mbedTLS's generic GCM API */
	mbedtls_gcm_context g;
	unsigned char ct2[32], tag2[16];
	mbedtls_gcm_init(&g);
	mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
	mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, 32, iv, 16, NULL, 0, pt, ct2, 16, tag2);
	mbedtls_gcm_free(&g);
	CHECK(!memcmp(ct, ct2, 32) && !memcmp(tag, tag2, 16), "GCM consistent");

	CHECK(gcm_decrypt(ct, 32, out, key, iv, tag) == 32 && !memcmp(out, pt, 32), "GCM decrypt");
	tag[3] ^= 1;
	CHECK(gcm_decrypt(ct, 32, out, key, iv, tag) < 0, "GCM rejects a bad tag");
	tag[3] ^= 1;
#ifdef VCAST_HAVE_OPENSSL_REFERENCE
	unsigned char rct[32], rtag[16];
	CHECK(ref_gcm_encrypt(pt, 32, rct, key, iv, rtag) == 32 && !memcmp(rct, ct, 32) && !memcmp(rtag, tag, 16),
	      "GCM (16-byte IV) matches OpenSSL");
	CHECK(ref_gcm_decrypt(ct, 32, out, key, iv, tag) == 32, "OpenSSL decrypts our GCM");
#endif
}

static void test_hashes(void)
{
	unsigned char h[64];
	unsigned int len = 0;
	sha_ctx_t *s = sha_init();
	sha_update(s, (const uint8_t *)"a", 1);
	sha_update(s, (const uint8_t *)"bc", 2);
	sha_final(s, h, &len);
	CHECK(len == 64 && eq_hex(h, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
				     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"),
	      "SHA-512 abc");
	sha_reset(s);
	sha_update(s, (const uint8_t *)"abc", 3);
	sha_final(s, h, NULL);
	CHECK(eq_hex(h, "ddaf35a193617aba"), "SHA-512 reset");
	sha_destroy(s);

	char *md5 = get_md5("abc");
	CHECK(md5 && !strcmp(md5, "900150983cd24fb0d6963f7d28e17f72"), "MD5 abc: %s", md5);
	free(md5);

	/* RFC 2617 digest example used by pairing_digest_verify() */
	const char *auth = "Digest username=\"Mufasa\",realm=\"testrealm@host.com\","
			   "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\",uri=\"/dir/index.html\",qop=auth,"
			   "nc=00000001,cnonce=\"0a4f113b\",response=\"6629fae49393a05397450978507c4ef1\","
			   "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";
	CHECK(pairing_digest_verify("GET", auth, "Circle Of Life"), "HTTP digest (RFC 2617)");
	CHECK(!pairing_digest_verify("GET", auth, "wrong"), "HTTP digest rejects a wrong password");

	char b64[64];
	unsigned char pk[32];
	for (int i = 0; i < 32; i++)
		pk[i] = (unsigned char)(i * 9);
	pk_to_base64(pk, 32, b64, (int)sizeof(b64));
	CHECK(!strcmp(b64, "AAkSGyQtNj9IUVpjbHV+h5CZoqu0vcbP2OHq8/wFDhc="), "base64: %s", b64);
#ifdef VCAST_HAVE_OPENSSL_REFERENCE
	char rb64[64];
	ref_pk_to_base64(pk, 32, rb64, (int)sizeof(rb64));
	CHECK(!strcmp(b64, rb64), "base64 matches OpenSSL");
#endif
}

static void test_x25519(void)
{
	unsigned char a_priv[32], b_priv[32], pub[32], s1[32], s2[32];
	unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_priv);
	unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b_priv);
	x25519_key_t *a = x25519_key_from_private_raw(a_priv), *b = x25519_key_from_private_raw(b_priv);
	x25519_key_get_raw(pub, a);
	CHECK(eq_hex(pub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), "X25519 Alice public");
	x25519_key_get_raw(pub, b);
	CHECK(eq_hex(pub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), "X25519 Bob public");
	x25519_key_t *bp = x25519_key_from_raw(pub);
	CHECK(x25519_derive_secret(s1, a, bp) == 0, "X25519 derive");
	CHECK(eq_hex(s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"), "X25519 shared secret");
	x25519_key_destroy(bp);

	/* random keys agree */
	x25519_key_t *c = x25519_key_generate(), *d = x25519_key_generate();
	unsigned char cp[32], dp[32];
	x25519_key_get_raw(cp, c);
	x25519_key_get_raw(dp, d);
	x25519_key_t *cpk = x25519_key_from_raw(cp), *dpk = x25519_key_from_raw(dp);
	x25519_derive_secret(s1, c, dpk);
	x25519_derive_secret(s2, d, cpk);
	CHECK(!memcmp(s1, s2, 32), "X25519 random agreement");
#ifdef VCAST_HAVE_OPENSSL_REFERENCE
	x25519_key_t *r = ref_x25519_key_generate();
	unsigned char rp[32], s3[32];
	ref_x25519_key_get_raw(rp, r);
	x25519_key_t *rpk = x25519_key_from_raw(rp);
	x25519_derive_secret(s1, c, rpk);
	x25519_key_t *rc = ref_x25519_key_from_raw(cp);
	ref_x25519_derive_secret(s3, r, rc);
	CHECK(!memcmp(s1, s3, 32), "X25519 agrees with OpenSSL");
	x25519_key_destroy(rpk);
	ref_x25519_key_destroy(rc);
	ref_x25519_key_destroy(r);
#endif
	x25519_key_destroy(cpk);
	x25519_key_destroy(dpk);
	x25519_key_destroy(c);
	x25519_key_destroy(d);
	x25519_key_destroy(a);
	x25519_key_destroy(b);
}

static void test_ed25519(void)
{
	unsigned char seed[32], pub[32], sig[64], msg[1];
	/* RFC 8032 7.1 tests 1 and 2 */
	unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
	ed25519_key_t *k = ed25519_key_from_seed(seed);
	ed25519_key_get_raw(pub, k);
	CHECK(eq_hex(pub, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"), "Ed25519 public 1");
	ed25519_sign(sig, 64, msg, 0, k);
	CHECK(eq_hex(sig, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9"
			  "b46bd25bf5f0595bbe24655141438e7a100b"),
	      "Ed25519 signature 1");
	ed25519_key_destroy(k);

	unhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", seed);
	k = ed25519_key_from_seed(seed);
	msg[0] = 0x72;
	ed25519_sign(sig, 64, msg, 1, k);
	CHECK(eq_hex(sig, "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f1"
			  "1d8c387b2eaeb4302aeeb00d291612bb0c00"),
	      "Ed25519 signature 2");
	ed25519_key_get_raw(pub, k);
	ed25519_key_t *v = ed25519_key_from_raw(pub);
	CHECK(ed25519_verify(sig, 64, msg, 1, v) == 1, "Ed25519 verify");
	sig[10] ^= 4;
	CHECK(ed25519_verify(sig, 64, msg, 1, v) == 0, "Ed25519 rejects a bad signature");
	ed25519_key_t *copy = ed25519_key_copy(k);
	ed25519_key_destroy(k);
	ed25519_sign(sig, 64, msg, 1, copy);
	CHECK(ed25519_verify(sig, 64, msg, 1, v) == 1, "Ed25519 copy signs");
	ed25519_key_destroy(copy);
	ed25519_key_destroy(v);

	/* persistent key derived from the device id */
	int result = 0;
	ed25519_key_t *dk = ed25519_key_generate("02:11:22:33:44:55", "", &result);
	ed25519_key_get_raw(pub, dk);
#ifdef VCAST_HAVE_OPENSSL_REFERENCE
	ed25519_key_t *rk = ref_ed25519_key_generate("02:11:22:33:44:55", "", &result);
	unsigned char rpub[32];
	ref_ed25519_key_get_raw(rpub, rk);
	CHECK(!memcmp(pub, rpub, 32), "device-id key matches OpenSSL");
	ed25519_sign(sig, 64, (const unsigned char *)"hello", 5, dk);
	ed25519_key_t *rv = ref_ed25519_key_from_raw(pub);
	CHECK(ref_ed25519_verify(sig, 64, (const unsigned char *)"hello", 5, rv) == 1, "OpenSSL verifies our signature");
	ref_ed25519_sign(sig, 64, (const unsigned char *)"hello", 5, rk);
	ed25519_key_t *ov = ed25519_key_from_raw(rpub);
	CHECK(ed25519_verify(sig, 64, (const unsigned char *)"hello", 5, ov) == 1, "we verify OpenSSL's signature");
	ed25519_key_destroy(ov);
	ref_ed25519_key_destroy(rv);
	ref_ed25519_key_destroy(rk);
#endif
	ed25519_key_destroy(dk);
}

/* derive the pair-verify AES key/IV the way an AirPlay client does */
static void client_derive(const unsigned char secret[32], const char *salt, unsigned char out[16])
{
	unsigned char h[64];
	mbedtls_sha512_context c;
	mbedtls_sha512_init(&c);
	mbedtls_sha512_starts(&c, 0);
	mbedtls_sha512_update(&c, (const unsigned char *)salt, strlen(salt));
	mbedtls_sha512_update(&c, secret, 32);
	mbedtls_sha512_finish(&c, h);
	mbedtls_sha512_free(&c);
	memcpy(out, h, 16);
}

static void test_pair_verify(void)
{
	/* the client side is the original OpenSSL code when available */
#ifdef VCAST_HAVE_OPENSSL_REFERENCE
#define C(fn) ref_##fn
	const char *who = "OpenSSL client";
#else
#define C(fn) fn
	const char *who = "mbedTLS client";
#endif
	int result = 0;
	pairing_t *pairing = pairing_init_generate("02:aa:bb:cc:dd:ee", "", &result);
	pairing_session_t *session = pairing_session_init(pairing);
	unsigned char server_ed_pub[32];
	pairing_get_public_key(pairing, server_ed_pub);
	pairing_session_set_setup_status(session);

	x25519_key_t *cx = C(x25519_key_generate)();
	ed25519_key_t *ced = C(ed25519_key_generate)("client-device", "", &result);
	unsigned char cx_pub[32], ced_pub[32], sx_pub[32], sig[64], secret[32], key[16], iv[16];
	C(x25519_key_get_raw)(cx_pub, cx);
	C(ed25519_key_get_raw)(ced_pub, ced);

	/* step 1 */
	CHECK(pairing_session_handshake(session, cx_pub, ced_pub) == 0, "handshake");
	CHECK(pairing_session_get_public_key(session, sx_pub) == 0, "server public key");
	CHECK(pairing_session_get_signature(session, sig) == 0, "server signature");

	x25519_key_t *sx = C(x25519_key_from_raw)(sx_pub);
	C(x25519_derive_secret)(secret, cx, sx);
	unsigned char server_secret[32];
	CHECK(pairing_get_ecdh_secret_key(session, server_secret) == 1 && !memcmp(secret, server_secret, 32),
	      "%s: same ECDH secret", who);
	client_derive(secret, "Pair-Verify-AES-Key", key);
	client_derive(secret, "Pair-Verify-AES-IV", iv);
	aes_ctx_t *ctr = C(aes_ctr_init)(key, iv);
	C(aes_ctr_encrypt)(ctr, sig, sig, 64);
	unsigned char msg[64];
	memcpy(msg, sx_pub, 32);
	memcpy(msg + 32, cx_pub, 32);
	ed25519_key_t *sed = C(ed25519_key_from_raw)(server_ed_pub);
	CHECK(C(ed25519_verify)(sig, 64, msg, 64, sed) == 1, "%s verifies the server's signature", who);

	/* step 2 */
	memcpy(msg, cx_pub, 32);
	memcpy(msg + 32, sx_pub, 32);
	C(ed25519_sign)(sig, 64, msg, 64, ced);
	C(aes_ctr_encrypt)(ctr, sig, sig, 64); /* continues the keystream */
	CHECK(pairing_session_finish(session, sig) == 0, "server accepts the %s signature", who);
	C(aes_ctr_destroy)(ctr);

	/* a tampered signature is refused */
	pairing_session_t *s2 = pairing_session_init(pairing);
	pairing_session_set_setup_status(s2);
	pairing_session_handshake(s2, cx_pub, ced_pub);
	sig[0] ^= 1;
	CHECK(pairing_session_finish(s2, sig) != 0, "server refuses a bad signature");
	pairing_session_destroy(s2);

	C(x25519_key_destroy)(sx);
	C(x25519_key_destroy)(cx);
	C(ed25519_key_destroy)(ced);
	C(ed25519_key_destroy)(sed);
	pairing_session_destroy(session);
	pairing_destroy(pairing);
#undef C
}

/* ------------------------------------------------------------------------- */
/* SRP-6a client (Apple variant, SHA-1, RFC 5054 padding), on mbedtls_mpi */

static const char *N_hex = "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
			   "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
			   "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
			   "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
			   "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
			   "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
			   "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";

typedef struct {
	mbedtls_mpi N, g, a, A, x;
	unsigned char Abin[256];
	size_t Alen;
	unsigned char K[40];
	unsigned char M[20];
	unsigned char HAMK[20];
} srp_client_t;

static void sha1_parts(unsigned char out[20], const void *p1, size_t l1, const void *p2, size_t l2)
{
	mbedtls_sha1_context c;
	mbedtls_sha1_init(&c);
	mbedtls_sha1_starts(&c);
	mbedtls_sha1_update(&c, p1, l1);
	if (p2)
		mbedtls_sha1_update(&c, p2, l2);
	mbedtls_sha1_finish(&c, out);
	mbedtls_sha1_free(&c);
}

static void mpi_hash_padded(unsigned char out[20], const mbedtls_mpi *a, const mbedtls_mpi *b)
{
	unsigned char buf[512];
	mbedtls_mpi_write_binary(a, buf, 256);
	mbedtls_mpi_write_binary(b, buf + 256, 256);
	sha1_parts(out, buf, 512, NULL, 0);
}

static void srp_client_init(srp_client_t *c, const char *user, const char *pin, const unsigned char *salt, size_t slen)
{
	unsigned char h[20], a[32];
	mbedtls_mpi_init(&c->N);
	mbedtls_mpi_init(&c->g);
	mbedtls_mpi_init(&c->a);
	mbedtls_mpi_init(&c->A);
	mbedtls_mpi_init(&c->x);
	mbedtls_mpi_read_string(&c->N, 16, N_hex);
	mbedtls_mpi_lset(&c->g, 2);
	/* x = H(s | H(I ":" P)) */
	mbedtls_sha1_context sc;
	mbedtls_sha1_init(&sc);
	mbedtls_sha1_starts(&sc);
	mbedtls_sha1_update(&sc, (const unsigned char *)user, strlen(user));
	mbedtls_sha1_update(&sc, (const unsigned char *)":", 1);
	mbedtls_sha1_update(&sc, (const unsigned char *)pin, strlen(pin));
	mbedtls_sha1_finish(&sc, h);
	mbedtls_sha1_free(&sc);
	unsigned char xh[20];
	sha1_parts(xh, salt, slen, h, 20);
	mbedtls_mpi_read_binary(&c->x, xh, 20);
	for (int i = 0; i < 32; i++)
		a[i] = (unsigned char)(i * 13 + 1);
	mbedtls_mpi_read_binary(&c->a, a, 32);
	mbedtls_mpi_exp_mod(&c->A, &c->g, &c->a, &c->N, NULL);
	c->Alen = mbedtls_mpi_size(&c->A);
	mbedtls_mpi_write_binary(&c->A, c->Abin, c->Alen);
}

/* v = g^x */
static void srp_client_verifier(srp_client_t *c, unsigned char v[256])
{
	mbedtls_mpi V;
	mbedtls_mpi_init(&V);
	mbedtls_mpi_exp_mod(&V, &c->g, &c->x, &c->N, NULL);
	mbedtls_mpi_write_binary(&V, v, 256);
	mbedtls_mpi_free(&V);
}

static void srp_client_process(srp_client_t *c, const char *user, const unsigned char *salt, size_t slen,
			       const unsigned char *Bbin, size_t Blen)
{
	mbedtls_mpi B, k, u, t, e, S, gx;
	unsigned char h[20], buf[256];
	mbedtls_mpi_init(&B);
	mbedtls_mpi_init(&k);
	mbedtls_mpi_init(&u);
	mbedtls_mpi_init(&t);
	mbedtls_mpi_init(&e);
	mbedtls_mpi_init(&S);
	mbedtls_mpi_init(&gx);
	mbedtls_mpi_read_binary(&B, Bbin, Blen);
	mpi_hash_padded(h, &c->N, &c->g);
	mbedtls_mpi_read_binary(&k, h, 20);
	mpi_hash_padded(h, &c->A, &B);
	mbedtls_mpi_read_binary(&u, h, 20);
	/* S = (B - k g^x) ^ (a + u x) mod N */
	mbedtls_mpi_exp_mod(&gx, &c->g, &c->x, &c->N, NULL);
	mbedtls_mpi_mul_mpi(&t, &k, &gx);
	mbedtls_mpi_mod_mpi(&t, &t, &c->N);
	mbedtls_mpi_sub_mpi(&t, &B, &t);
	mbedtls_mpi_mod_mpi(&t, &t, &c->N);
	mbedtls_mpi_mul_mpi(&e, &u, &c->x);
	mbedtls_mpi_add_mpi(&e, &e, &c->a);
	mbedtls_mpi_exp_mod(&S, &t, &e, &c->N, NULL);
	size_t Slen = mbedtls_mpi_size(&S);
	mbedtls_mpi_write_binary(&S, buf, Slen);
	unsigned char four[4] = {0, 0, 0, 0};
	sha1_parts(c->K, buf, Slen, four, 4);
	four[3] = 1;
	sha1_parts(c->K + 20, buf, Slen, four, 4);
	/* M = H(H(N) xor H(g) | H(I) | s | A | B | K) */
	unsigned char hn[20], hg[20], hi[20], nb[256], gb[1] = {2};
	size_t nlen = mbedtls_mpi_size(&c->N);
	mbedtls_mpi_write_binary(&c->N, nb, nlen);
	sha1_parts(hn, nb, nlen, NULL, 0);
	sha1_parts(hg, gb, 1, NULL, 0);
	for (int i = 0; i < 20; i++)
		hn[i] ^= hg[i];
	sha1_parts(hi, user, strlen(user), NULL, 0);
	mbedtls_sha1_context sc;
	mbedtls_sha1_init(&sc);
	mbedtls_sha1_starts(&sc);
	mbedtls_sha1_update(&sc, hn, 20);
	mbedtls_sha1_update(&sc, hi, 20);
	mbedtls_sha1_update(&sc, salt, slen);
	mbedtls_sha1_update(&sc, c->Abin, c->Alen);
	mbedtls_sha1_update(&sc, Bbin, Blen);
	mbedtls_sha1_update(&sc, c->K, 40);
	mbedtls_sha1_finish(&sc, c->M);
	/* H_AMK = H(A | M | K) */
	mbedtls_sha1_starts(&sc);
	mbedtls_sha1_update(&sc, c->Abin, c->Alen);
	mbedtls_sha1_update(&sc, c->M, 20);
	mbedtls_sha1_update(&sc, c->K, 40);
	mbedtls_sha1_finish(&sc, c->HAMK);
	mbedtls_sha1_free(&sc);
	mbedtls_mpi_free(&B);
	mbedtls_mpi_free(&k);
	mbedtls_mpi_free(&u);
	mbedtls_mpi_free(&t);
	mbedtls_mpi_free(&e);
	mbedtls_mpi_free(&S);
	mbedtls_mpi_free(&gx);
}

static void srp_client_free(srp_client_t *c)
{
	mbedtls_mpi_free(&c->N);
	mbedtls_mpi_free(&c->g);
	mbedtls_mpi_free(&c->a);
	mbedtls_mpi_free(&c->A);
	mbedtls_mpi_free(&c->x);
}

static void test_srp(void)
{
	const char *user = "AA:BB:CC:DD:EE:FF";
	unsigned char salt[16], v[256], b[32];
	for (int i = 0; i < 16; i++)
		salt[i] = (unsigned char)(0x80 + i);
	for (int i = 0; i < 32; i++)
		b[i] = (unsigned char)(255 - i);

	srp_client_t c;
	srp_client_init(&c, user, "1234", salt, 16);
	srp_client_verifier(&c, v);

	const unsigned char *B = NULL, *HAMK = NULL;
	int lenB = 0, lenK = 0;
	struct SRPVerifier *ver = srp_verifier_new(SRP_SHA1, SRP_NG_2048, user, salt, 16, v, 256, c.Abin,
						   (int)c.Alen, b, 32, &B, &lenB, NULL, NULL, 1);
	CHECK(ver != NULL, "SRP verifier");
	if (!ver)
		return;
	srp_client_process(&c, user, salt, 16, B, (size_t)lenB);
	srp_verifier_verify_session(ver, c.M, &HAMK);
	CHECK(srp_verifier_is_authenticated(ver) && HAMK && !memcmp(HAMK, c.HAMK, 20), "SRP client proof accepted");
	const unsigned char *K = srp_verifier_get_session_key(ver, &lenK);
	CHECK(lenK == 40 && !memcmp(K, c.K, 40), "SRP session keys agree");

	const unsigned char *B2 = NULL;
	int lenB2 = 0;
	srp_create_server_ephemeral_key(SRP_SHA1, SRP_NG_2048, v, 256, b, 32, &B2, &lenB2, NULL, NULL, 1);
	CHECK(lenB2 == lenB && !memcmp(B, B2, (size_t)lenB), "SRP ephemeral key consistent");

#ifdef VCAST_HAVE_OPENSSL_REFERENCE
	const unsigned char *rB = NULL, *rHAMK = NULL;
	int rlenB = 0, rlenK = 0;
	struct SRPVerifier *rver = ref_srp_verifier_new(SRP_SHA1, SRP_NG_2048, user, salt, 16, v, 256, c.Abin,
							(int)c.Alen, b, 32, &rB, &rlenB, NULL, NULL, 1);
	CHECK(rver && rlenB == lenB && !memcmp(rB, B, (size_t)lenB), "SRP B matches OpenSSL");
	if (rver) {
		ref_srp_verifier_verify_session(rver, c.M, &rHAMK);
		const unsigned char *rK = ref_srp_verifier_get_session_key(rver, &rlenK);
		CHECK(ref_srp_verifier_is_authenticated(rver) && rlenK == 40 && !memcmp(rK, K, 40) &&
			      !memcmp(rHAMK, HAMK, 20),
		      "SRP session key and proof match OpenSSL");
		ref_srp_verifier_delete(rver);
	}
	const unsigned char *rB2 = NULL;
	int rlenB2 = 0;
	ref_srp_create_server_ephemeral_key(SRP_SHA1, SRP_NG_2048, v, 256, b, 32, &rB2, &rlenB2, NULL, NULL, 1);
	CHECK(rlenB2 == lenB2 && !memcmp(rB2, B2, (size_t)lenB2), "SRP ephemeral key matches OpenSSL");
	free((void *)rB2);
#endif
	free((void *)B2);
	srp_verifier_delete(ver);
	srp_client_free(&c);

	/* full pair-setup-pin through pairing.c (random salt and b) */
	int result = 0;
	pairing_t *pairing = pairing_init_generate("02:aa:bb:cc:dd:ee", "", &result);
	pairing_session_t *session = pairing_session_init(pairing);
	const char *s_salt = NULL, *s_pk = NULL;
	int lsalt = 0, lpk = 0;
	CHECK(srp_new_user(session, pairing, user, "4321", &s_salt, &lsalt, &s_pk, &lpk) == 0 && lsalt == 16,
	      "srp_new_user");
	srp_client_t c2;
	srp_client_init(&c2, user, "4321", (const unsigned char *)s_salt, 16);
	srp_client_process(&c2, user, (const unsigned char *)s_salt, 16, (const unsigned char *)s_pk, (size_t)lpk);
	free((void *)s_pk);
	unsigned char proof[64] = {0};
	memcpy(proof, c2.M, 20);
	CHECK(srp_validate_proof(session, pairing, c2.Abin, (int)c2.Alen, proof, (int)sizeof(proof)) == 0 &&
		      !memcmp(proof, c2.HAMK, 20),
	      "srp_validate_proof");

	/* step 3: client sends its Ed25519 public key encrypted with AES-GCM */
	unsigned char h[64], aes_key[16], aes_iv[16], cpk[32], epk[32], tag[16];
	mbedtls_sha512_context sc;
	mbedtls_sha512_init(&sc);
	mbedtls_sha512_starts(&sc, 0);
	mbedtls_sha512_update(&sc, (const unsigned char *)"Pair-Setup-AES-Key", 18);
	mbedtls_sha512_update(&sc, c2.K, 40);
	mbedtls_sha512_finish(&sc, h);
	memcpy(aes_key, h, 16);
	mbedtls_sha512_starts(&sc, 0);
	mbedtls_sha512_update(&sc, (const unsigned char *)"Pair-Setup-AES-IV", 17);
	mbedtls_sha512_update(&sc, c2.K, 40);
	mbedtls_sha512_finish(&sc, h);
	mbedtls_sha512_free(&sc);
	memcpy(aes_iv, h, 16);
	aes_iv[15]++;
	for (int i = 0; i < 32; i++)
		cpk[i] = (unsigned char)(i + 100);
	gcm_encrypt(cpk, 32, epk, aes_key, aes_iv, tag);
	CHECK(srp_confirm_pair_setup(session, pairing, epk, tag) == 32, "srp_confirm_pair_setup");
	aes_iv[15]++;
	unsigned char spk[32], expect[32];
	CHECK(gcm_decrypt(epk, 32, spk, aes_key, aes_iv, tag) == 32, "client authenticates the server");
	pairing_get_public_key(pairing, expect);
	CHECK(!memcmp(spk, expect, 32), "server public key delivered");
	srp_client_free(&c2);
	pairing_session_destroy(session);
	pairing_destroy(pairing);
}

int main(void)
{
#ifdef VCAST_HAVE_OPENSSL_REFERENCE
	printf("crypto-test: comparing against the original OpenSSL implementation\n");
#else
	printf("crypto-test: test vectors only (built without OpenSSL)\n");
#endif
	test_aes();
	test_gcm();
	test_hashes();
	test_x25519();
	test_ed25519();
	test_pair_verify();
	test_srp();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
