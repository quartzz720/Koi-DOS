#ifndef KERNEL_P256_H
#define KERNEL_P256_H

#include "../include/bootinfo.h"

/* ECDSA on NIST P-256, verification only.
 *
 * Half of the certificates on the web are signed with this rather than with
 * RSA, and a browser that cannot check them has to refuse them - which is
 * where this system was until this file existed.
 *
 * Verification only, deliberately. Making a signature needs a secret and a
 * source of randomness good enough that a repeat leaks the key; checking one
 * needs neither, works entirely on public numbers, and is the only half a
 * client has any business doing.
 */

/* r and s arrive as they were written in DER - any length up to 32 bytes,
   possibly with a leading zero. The key is the two 32-byte coordinates. */
int p256_verify(const boot_uint8_t hash[32],
                const boot_uint8_t* signature_r, boot_uint32_t r_length,
                const boot_uint8_t* signature_s, boot_uint32_t s_length,
                const boot_uint8_t* key_x, const boot_uint8_t* key_y);

/* Whether a key is a point on the curve at all. Checked before it is used:
   a point that is not on the curve is a point somebody chose. */
int p256_public_key_valid(const boot_uint8_t* x, const boot_uint8_t* y);

/* The same on either curve: `size` is 32 for P-256 and 48 for P-384, which is
   how a certificate chain that mixes them is followed to its end. */
int p256_verify_on(const boot_uint8_t* hash, boot_uint32_t hash_length,
                   const boot_uint8_t* signature_r, boot_uint32_t r_length,
                   const boot_uint8_t* signature_s, boot_uint32_t s_length,
                   const boot_uint8_t* key_x, const boot_uint8_t* key_y,
                   boot_uint32_t size);
int p256_public_key_valid_on(const boot_uint8_t* x, const boot_uint8_t* y,
                             boot_uint32_t size);

/* Key agreement on P-256, for TLS 1.2 - where a good many servers will do
 * ECDHE on this curve and on no other, x25519 included. The secret scalar is
 * the caller's; see the note in the source about what this does not promise
 * about timing.
 *
 * `p256_agree_base` is our own public point, `p256_agree` is the shared one,
 * of which only the x coordinate is used - which is what TLS asks for.
 */
int p256_agree_base(const boot_uint8_t scalar[32], boot_uint8_t out_x[32],
                    boot_uint8_t out_y[32]);
int p256_agree(const boot_uint8_t scalar[32], const boot_uint8_t* peer_x,
               const boot_uint8_t* peer_y, boot_uint8_t out_x[32]);

#endif
