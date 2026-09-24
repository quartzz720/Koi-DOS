#ifndef KERNEL_SHA256_H
#define KERNEL_SHA256_H

#include "../include/bootinfo.h"

/* SHA-256, and the two things everything builds on top of it.
 *
 * The first stone of TLS. Nothing here talks to the network: a hash, a message
 * authentication code and a key derivation function are arithmetic, they have
 * published test vectors, and they can be finished and proved right before a
 * single byte of a handshake exists. That order is deliberate - a wrong hash
 * inside a handshake looks exactly like a wrong handshake, and the handshake
 * is the part that cannot be tested against a book.
 */

#define SHA256_SIZE 32
#define SHA256_BLOCK 64

typedef struct {
    boot_uint32_t state[8];
    boot_uint64_t length;              /* message bytes so far */
    boot_uint8_t block[SHA256_BLOCK];
    boot_uint32_t used;                /* bytes waiting in `block` */
} SHA256;

void sha256_start(SHA256* self);
void sha256_add(SHA256* self, const void* data, boot_uint64_t length);
void sha256_finish(SHA256* self, boot_uint8_t out[SHA256_SIZE]);

/* The whole of a message in one call, which is what most callers want. */
void sha256(const void* data, boot_uint64_t length,
            boot_uint8_t out[SHA256_SIZE]);

/* HMAC-SHA256 (RFC 2104): a hash with a key, and the only construction that
   makes a hash safe to use as one. */
void hmac_sha256(const void* key, boot_uint64_t key_length,
                 const void* data, boot_uint64_t length,
                 boot_uint8_t out[SHA256_SIZE]);

/* HKDF (RFC 5869), which is how TLS 1.3 turns one shared secret into the six
   keys it actually uses. Extract concentrates whatever entropy the input has;
   Expand stretches that into as many bytes as are asked for, with a label so
   that two keys derived from one secret are unrelated. */
void hkdf_extract(const void* salt, boot_uint64_t salt_length,
                  const void* key, boot_uint64_t key_length,
                  boot_uint8_t out[SHA256_SIZE]);
int hkdf_expand(const boot_uint8_t key[SHA256_SIZE],
                const void* info, boot_uint64_t info_length,
                boot_uint8_t* out, boot_uint64_t length);

/* TLS 1.3 spells its labels in one particular way (RFC 8446, 7.1): a length,
   the string "tls13 " in front of the label, and the transcript hash. Here
   rather than in the handshake because getting this wrong produces keys that
   are wrong in a way nothing can see until the first decryption fails. */
int hkdf_expand_label(const boot_uint8_t key[SHA256_SIZE],
                      const char* label,
                      const boot_uint8_t* context, boot_uint32_t context_length,
                      boot_uint8_t* out, boot_uint32_t length);

/* SHA-384, which certificate chains use as often as SHA-256.
 *
 * The same construction with wider words: 64 bits instead of 32, eighty rounds
 * instead of sixty-four, a different starting state, and the answer is the
 * first 48 bytes of what comes out. It is here rather than in a file of its
 * own because it is the same algorithm - SHA-512 with a different beginning
 * and a haircut - and separating them would be filing one idea in two
 * places. */
#define SHA384_SIZE 48
#define SHA512_BLOCK 128

typedef struct {
    boot_uint64_t state[8];
    boot_uint64_t length;
    boot_uint8_t block[SHA512_BLOCK];
    boot_uint32_t used;
} SHA384;

void sha384_start(SHA384* self);
void sha384_add(SHA384* self, const void* data, boot_uint64_t length);
void sha384_finish(SHA384* self, boot_uint8_t out[SHA384_SIZE]);
void sha384(const void* data, boot_uint64_t length,
            boot_uint8_t out[SHA384_SIZE]);

#endif
