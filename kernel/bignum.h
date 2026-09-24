#ifndef KERNEL_BIGNUM_H
#define KERNEL_BIGNUM_H

#include "../include/bootinfo.h"

/* Arithmetic on numbers of a few hundred bytes, for RSA.
 *
 * One operation is needed and one only: `signature^e mod n`, with e small and
 * n about 2048 bits. That is what verifying an RSA signature is, and it is the
 * whole of why this file exists - there is no division here, no modular
 * inverse, no primality testing, because verification needs none of them.
 *
 * Not constant time, and that is correct here rather than a shortcut: every
 * number in a signature verification is public. The signature is public, the
 * modulus is public, the exponent is 65537. There is no secret to leak, and
 * pretending otherwise would be cargo cult.
 */

#define BIGNUM_MAX_BYTES 512        /* 4096-bit keys, which is the largest
                                       anybody issues */
#define BIGNUM_MAX_LIMBS (BIGNUM_MAX_BYTES / 4)

/* Two limbs of headroom above the largest modulus.
 *
 * The long division shifts its remainder left before comparing, and with a
 * 4096-bit modulus that remainder already fills every limb - so the bit
 * shifted out at the top was dropped, and the answer came back wrong. Only
 * wrong for the largest keys, which is why every test with a 2048-bit key
 * passed and the first 4096-bit root in a real chain did not. */
typedef struct {
    boot_uint32_t limb[BIGNUM_MAX_LIMBS + 2];
    int count;                      /* limbs in use, least significant first */
} BIGNUM;

/* From and to the big-endian bytes every format on the wire uses. */
int bignum_from_bytes(BIGNUM* out, const boot_uint8_t* data, boot_uint32_t length);
int bignum_to_bytes(const BIGNUM* value, boot_uint8_t* out, boot_uint32_t length);

/* The pieces elliptic curves need, over the same numbers.
 *
 * A curve does its arithmetic in a field - add, subtract and multiply, all
 * modulo a prime - and needs an inverse, which comes from the exponent rule
 * rather than from a separate algorithm: x^(p-2) is x^-1 when p is prime, and
 * a modexp is already here. Slower than the extended Euclidean algorithm and
 * about forty lines shorter, on an operation that happens twice per
 * signature. */
void bignum_zero(BIGNUM* out);
void bignum_small(BIGNUM* out, boot_uint32_t value);
int bignum_is_zero(const BIGNUM* value);
int bignum_compare_to(const BIGNUM* left, const BIGNUM* right);
void bignum_add_mod(BIGNUM* out, const BIGNUM* left, const BIGNUM* right,
                    const BIGNUM* modulus);
void bignum_sub_mod(BIGNUM* out, const BIGNUM* left, const BIGNUM* right,
                    const BIGNUM* modulus);
int bignum_mul_mod(BIGNUM* out, const BIGNUM* left, const BIGNUM* right,
                   const BIGNUM* modulus);
/* out = value^(modulus-2) mod modulus, which is the inverse for a prime. */
int bignum_inverse(BIGNUM* out, const BIGNUM* value, const BIGNUM* modulus);
void bignum_reduce(BIGNUM* value, const BIGNUM* modulus);

/* out = base^exponent mod modulus. The exponent is an ordinary number
   because RSA's public exponent always is - 65537, or occasionally 3. */
int bignum_modexp(BIGNUM* out, const BIGNUM* base, boot_uint32_t exponent,
                  const BIGNUM* modulus);

#endif
