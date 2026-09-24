#ifndef KERNEL_X25519_H
#define KERNEL_X25519_H

#include "../include/bootinfo.h"

/* X25519 (RFC 7748): the key agreement TLS 1.3 opens with.
 *
 * Two machines that have never met each end up holding the same 32 bytes, and
 * anybody who watched every packet between them cannot work out what those
 * bytes are. That is the whole trick on which the rest rests: the keys the
 * record layer uses are derived from this, and without it encryption would
 * only ever be possible between two machines that had already met somewhere
 * else to agree on a secret.
 *
 * Curve25519 rather than the older Diffie-Hellman groups because it is the one
 * with no parameter choices to get wrong: every 32-byte string is a valid
 * public key, there is no point validation to forget and no small-subgroup
 * check to skip. A design where the dangerous mistakes are unavailable is the
 * right kind for code like this.
 */

#define X25519_SIZE 32

/* The public key belonging to a secret: the base point multiplied by it. */
void x25519_public(boot_uint8_t out[X25519_SIZE],
                   const boot_uint8_t secret[X25519_SIZE]);

/* The shared secret: their public key multiplied by our secret. Both ends
 * compute the same value from opposite halves.
 *
 * Returns 0 when the result is all zeroes, which is what a peer sending one of
 * the handful of degenerate points produces. Such a peer is either broken or
 * trying something, and either way there is no shared secret to have. */
int x25519(boot_uint8_t out[X25519_SIZE],
           const boot_uint8_t secret[X25519_SIZE],
           const boot_uint8_t their_public[X25519_SIZE]);

#endif
