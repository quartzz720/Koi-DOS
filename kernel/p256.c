#include "p256.h"
#include "bignum.h"
#include "string.h"

/* ECDSA on P-256, verification only.
 *
 * ---- Why this exists ------------------------------------------------------
 *
 * Half the web signs with this curve rather than with RSA, and until it was
 * written this system had to *not offer* ECDSA in its handshake - which meant
 * every site holding both kinds of certificate was asked for the RSA one, and
 * every site holding only an ECDSA one was refused outright. Wikipedia was
 * refused. That is the honest behaviour when a check cannot be made, and it is
 * not a good place to stay.
 *
 * ---- What a signature is --------------------------------------------------
 *
 * The curve is the set of points where y^2 = x^3 - 3x + b, counted modulo a
 * prime. Points can be added, so a point can be multiplied by a number, and
 * that multiplication is easy forwards and believed impossible backwards -
 * which is the whole of it. A signature is two numbers, r and s. Verifying
 * means computing a point from the message, r, s and the signer's public key,
 * and checking that its x coordinate comes back as r.
 *
 * ---- How the arithmetic is arranged ---------------------------------------
 *
 * Points are held in Jacobian coordinates - (X, Y, Z) standing for
 * (X/Z^2, Y/Z^3) - because adding two points in plain coordinates needs a
 * division, and a division here means an inversion, and an inversion costs
 * more than the twelve multiplications that avoiding it costs. One inversion
 * at the very end converts back.
 *
 * Nothing here is constant time and nothing here needs to be: every number in
 * a verification is public. A *signature* would be another matter, and this
 * system does not make signatures.
 */

/* The curve, from FIPS 186-4. Written as bytes because that is how they are
   published and how they arrive on the wire. */
static const boot_uint8_t curve_p[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF
};
static const boot_uint8_t curve_n[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51
};
static const boot_uint8_t curve_b[32] = {
    0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,
    0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B
};
static const boot_uint8_t base_x[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96
};
static const boot_uint8_t base_y[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5
};

/* And P-384, which is the same curve shape with larger numbers.
 *
 * Needed because certificate chains mix the two: a leaf on P-256 signed by an
 * intermediate on P-384 is an ordinary arrangement, and a client that reads
 * only the smaller curve gets one link into such a chain and stops. Both
 * curves have a = -3, so every formula below serves both and the difference
 * is entirely in these numbers. */
static const boot_uint8_t curve384_p[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF
};
static const boot_uint8_t curve384_n[48] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,
    0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x73
};
static const boot_uint8_t curve384_b[48] = {
    0xB3,0x31,0x2F,0xA7,0xE2,0x3E,0xE7,0xE4,0x98,0x8E,0x05,0x6B,0xE3,0xF8,0x2D,0x19,
    0x18,0x1D,0x9C,0x6E,0xFE,0x81,0x41,0x12,0x03,0x14,0x08,0x8F,0x50,0x13,0x87,0x5A,
    0xC6,0x56,0x39,0x8D,0x8A,0x2E,0xD1,0x9D,0x2A,0x85,0xC8,0xED,0xD3,0xEC,0x2A,0xEF
};
static const boot_uint8_t base384_x[48] = {
    0xAA,0x87,0xCA,0x22,0xBE,0x8B,0x05,0x37,0x8E,0xB1,0xC7,0x1E,0xF3,0x20,0xAD,0x74,
    0x6E,0x1D,0x3B,0x62,0x8B,0xA7,0x9B,0x98,0x59,0xF7,0x41,0xE0,0x82,0x54,0x2A,0x38,
    0x55,0x02,0xF2,0x5D,0xBF,0x55,0x29,0x6C,0x3A,0x54,0x5E,0x38,0x72,0x76,0x0A,0xB7
};
static const boot_uint8_t base384_y[48] = {
    0x36,0x17,0xDE,0x4A,0x96,0x26,0x2C,0x6F,0x5D,0x9E,0x98,0xBF,0x92,0x92,0xDC,0x29,
    0xF8,0xF4,0x1D,0xBD,0x28,0x9A,0x14,0x7C,0xE9,0xDA,0x31,0x13,0xB5,0xF0,0xB8,0xC0,
    0x0A,0x60,0xB1,0xCE,0x1D,0x7E,0x81,0x9D,0x7A,0x43,0x1D,0x7C,0x90,0xEA,0x0E,0x5F
};

typedef struct {
    BIGNUM prime;
    BIGNUM order;
    BIGNUM b;
    const boot_uint8_t* base_x;
    const boot_uint8_t* base_y;
    boot_uint32_t size;             /* bytes in a coordinate */
    int bits;
} CURVE;

typedef struct {
    BIGNUM x, y, z;
    int infinity;
} POINT;

static CURVE curves[2];
static int prepared;

static void prepare(void) {
    if (prepared) return;
    bignum_from_bytes(&curves[0].prime, curve_p, 32);
    bignum_from_bytes(&curves[0].order, curve_n, 32);
    bignum_from_bytes(&curves[0].b, curve_b, 32);
    curves[0].base_x = base_x;
    curves[0].base_y = base_y;
    curves[0].size = 32;
    curves[0].bits = 256;

    bignum_from_bytes(&curves[1].prime, curve384_p, 48);
    bignum_from_bytes(&curves[1].order, curve384_n, 48);
    bignum_from_bytes(&curves[1].b, curve384_b, 48);
    curves[1].base_x = base384_x;
    curves[1].base_y = base384_y;
    curves[1].size = 48;
    curves[1].bits = 384;
    prepared = 1;
}

static const CURVE* curve_of(boot_uint32_t size) {
    prepare();
    if (size == 48) return &curves[1];
    return &curves[0];
}

static void double_point(const CURVE* curve, POINT* out, const POINT* in) {
    BIGNUM a, b, c, d, e, f, t;

    if (in->infinity || bignum_is_zero(&in->y)) {
        out->infinity = 1;
        return;
    }

    /* The standard doubling for a curve whose a is -3, which P-256's is: it
       lets the slope be computed with two multiplications instead of four. */
    bignum_mul_mod(&a, &in->z, &in->z, &curve->prime);          /* z^2 */
    bignum_sub_mod(&b, &in->x, &a, &curve->prime);              /* x - z^2 */
    bignum_add_mod(&c, &in->x, &a, &curve->prime);              /* x + z^2 */
    bignum_mul_mod(&d, &b, &c, &curve->prime);                  /* x^2 - z^4 */
    bignum_small(&t, 3);
    bignum_mul_mod(&d, &d, &t, &curve->prime);                  /* the slope's top */

    bignum_mul_mod(&e, &in->y, &in->y, &curve->prime);          /* y^2 */
    bignum_mul_mod(&f, &in->x, &e, &curve->prime);
    bignum_small(&t, 4);
    bignum_mul_mod(&f, &f, &t, &curve->prime);                  /* 4xy^2 */

    bignum_mul_mod(&a, &d, &d, &curve->prime);
    bignum_small(&t, 2);
    bignum_mul_mod(&b, &f, &t, &curve->prime);
    bignum_sub_mod(&a, &a, &b, &curve->prime);                  /* the new x */

    bignum_mul_mod(&c, &e, &e, &curve->prime);
    bignum_small(&t, 8);
    bignum_mul_mod(&c, &c, &t, &curve->prime);                  /* 8y^4 */

    bignum_sub_mod(&b, &f, &a, &curve->prime);
    bignum_mul_mod(&b, &d, &b, &curve->prime);
    bignum_sub_mod(&b, &b, &c, &curve->prime);                  /* the new y */

    bignum_mul_mod(&c, &in->y, &in->z, &curve->prime);
    bignum_small(&t, 2);
    bignum_mul_mod(&c, &c, &t, &curve->prime);                  /* the new z */

    out->x = a;
    out->y = b;
    out->z = c;
    out->infinity = 0;
}

static void add_point(const CURVE* curve, POINT* out, const POINT* left,
                      const POINT* right) {
    BIGNUM z1, z2, u1, u2, s1, s2, h, r, t, h2, h3;

    if (left->infinity) { *out = *right; return; }
    if (right->infinity) { *out = *left; return; }

    bignum_mul_mod(&z1, &left->z, &left->z, &curve->prime);
    bignum_mul_mod(&z2, &right->z, &right->z, &curve->prime);
    bignum_mul_mod(&u1, &left->x, &z2, &curve->prime);
    bignum_mul_mod(&u2, &right->x, &z1, &curve->prime);
    bignum_mul_mod(&s1, &z2, &right->z, &curve->prime);
    bignum_mul_mod(&s1, &left->y, &s1, &curve->prime);
    bignum_mul_mod(&s2, &z1, &left->z, &curve->prime);
    bignum_mul_mod(&s2, &right->y, &s2, &curve->prime);

    bignum_sub_mod(&h, &u2, &u1, &curve->prime);
    bignum_sub_mod(&r, &s2, &s1, &curve->prime);

    if (bignum_is_zero(&h)) {
        /* The same point, or two points that cancel. */
        if (bignum_is_zero(&r)) { double_point(curve, out, left); return; }
        out->infinity = 1;
        return;
    }

    bignum_mul_mod(&h2, &h, &h, &curve->prime);
    bignum_mul_mod(&h3, &h2, &h, &curve->prime);
    bignum_mul_mod(&t, &u1, &h2, &curve->prime);

    bignum_mul_mod(&u2, &r, &r, &curve->prime);
    bignum_sub_mod(&u2, &u2, &h3, &curve->prime);
    bignum_sub_mod(&u2, &u2, &t, &curve->prime);
    bignum_sub_mod(&u2, &u2, &t, &curve->prime);                /* the new x */

    bignum_sub_mod(&z1, &t, &u2, &curve->prime);
    bignum_mul_mod(&z1, &r, &z1, &curve->prime);
    bignum_mul_mod(&s1, &s1, &h3, &curve->prime);
    bignum_sub_mod(&z1, &z1, &s1, &curve->prime);               /* the new y */

    bignum_mul_mod(&z2, &left->z, &right->z, &curve->prime);
    bignum_mul_mod(&z2, &z2, &h, &curve->prime);                /* the new z */

    out->x = u2;
    out->y = z1;
    out->z = z2;
    out->infinity = 0;
}

/* Back to ordinary coordinates: one inversion, at the end, once. */
static int to_affine(const CURVE* curve, BIGNUM* x, const POINT* in) {
    BIGNUM inverse, square;

    if (in->infinity) return 0;
    if (!bignum_inverse(&inverse, &in->z, &curve->prime)) return 0;
    bignum_mul_mod(&square, &inverse, &inverse, &curve->prime);
    bignum_mul_mod(x, &in->x, &square, &curve->prime);
    return 1;
}

/* out = scalar * point, by doubling and adding from the top bit down. */
static void multiply_point(const CURVE* curve, POINT* out,
                           const BIGNUM* scalar, const POINT* point) {
    POINT result;
    int top = curve->bits;

    memset(&result, 0, sizeof(result));
    result.infinity = 1;

    while (top > 0) {
        int bit = top - 1;

        if ((bit / 32) < scalar->count &&
            ((scalar->limb[bit / 32] >> (bit % 32)) & 1)) break;
        top--;
    }

    for (int at = top - 1; at >= 0; at--) {
        POINT doubled;

        double_point(curve, &doubled, &result);
        if (result.infinity) doubled.infinity = 1;
        result = doubled;
        if ((at / 32) < scalar->count &&
            ((scalar->limb[at / 32] >> (at % 32)) & 1)) {
            POINT sum;

            add_point(curve, &sum, &result, point);
            result = sum;
        }
    }
    *out = result;
}

/* Both coordinates, which key agreement needs and verification does not. */
static int to_affine_pair(const CURVE* curve, BIGNUM* x, BIGNUM* y,
                          const POINT* in) {
    BIGNUM inverse, square, cube;

    if (in->infinity) return 0;
    if (!bignum_inverse(&inverse, &in->z, &curve->prime)) return 0;
    bignum_mul_mod(&square, &inverse, &inverse, &curve->prime);
    bignum_mul_mod(&cube, &square, &inverse, &curve->prime);
    bignum_mul_mod(x, &in->x, &square, &curve->prime);
    bignum_mul_mod(y, &in->y, &cube, &curve->prime);
    return 1;
}

/* ---- Key agreement -------------------------------------------------------
 *
 * The rest of this file only ever touches public numbers, and says so. These
 * two do not: the scalar is a secret that exists for one connection. The
 * multiplication above is the plain double-and-add, and its timing depends on
 * the bits of that scalar - so a party who can measure this machine finely
 * enough could learn them.
 *
 * Kept anyway, and written down rather than hidden: the alternative is a
 * constant-time ladder, which is a real piece of work, and the exposure here
 * is a single-user machine on a home network agreeing a key that lives for
 * one page load. It is the sort of thing to fix when the rest is finished,
 * not a reason to have no TLS 1.2 at all.
 */
int p256_agree_base(const boot_uint8_t scalar[32], boot_uint8_t out_x[32],
                    boot_uint8_t out_y[32]) {
    const CURVE* curve;
    BIGNUM k, x, y;
    POINT generator, result;

    prepare();
    curve = curve_of(32);
    bignum_from_bytes(&k, scalar, 32);
    if (bignum_is_zero(&k)) return 0;
    memset(&generator, 0, sizeof(generator));
    bignum_from_bytes(&generator.x, base_x, 32);
    bignum_from_bytes(&generator.y, base_y, 32);
    bignum_small(&generator.z, 1);
    multiply_point(curve, &result, &k, &generator);
    if (!to_affine_pair(curve, &x, &y, &result)) return 0;
    return bignum_to_bytes(&x, out_x, 32) && bignum_to_bytes(&y, out_y, 32);
}

int p256_agree(const boot_uint8_t scalar[32], const boot_uint8_t* peer_x,
               const boot_uint8_t* peer_y, boot_uint8_t out_x[32]) {
    const CURVE* curve;
    BIGNUM k, x;
    POINT peer, result;

    /* Their point is checked before it is used. A point that is not on the
       curve is a point somebody chose, and multiplying a secret by it is one
       of the older ways to hand that secret over. */
    if (!p256_public_key_valid(peer_x, peer_y)) return 0;

    prepare();
    curve = curve_of(32);
    bignum_from_bytes(&k, scalar, 32);
    if (bignum_is_zero(&k)) return 0;
    memset(&peer, 0, sizeof(peer));
    bignum_from_bytes(&peer.x, peer_x, 32);
    bignum_from_bytes(&peer.y, peer_y, 32);
    bignum_small(&peer.z, 1);
    multiply_point(curve, &result, &k, &peer);
    if (!to_affine(curve, &x, &result)) return 0;   /* x alone is the secret */
    return bignum_to_bytes(&x, out_x, 32);
}

int p256_public_key_valid_on(const boot_uint8_t* x, const boot_uint8_t* y,
                             boot_uint32_t size) {
    const CURVE* curve = curve_of(size);
    BIGNUM px, py, left, right, three, t;

    bignum_from_bytes(&px, x, size);
    bignum_from_bytes(&py, y, size);

    /* On the curve, and not the point at infinity. A key that is neither is a
       key an attacker chose, and using it is how small-subgroup attacks
       start. */
    if (bignum_is_zero(&px) && bignum_is_zero(&py)) return 0;
    if (bignum_compare_to(&px, &curve->prime) >= 0) return 0;
    if (bignum_compare_to(&py, &curve->prime) >= 0) return 0;

    bignum_mul_mod(&left, &py, &py, &curve->prime);
    bignum_mul_mod(&right, &px, &px, &curve->prime);
    bignum_mul_mod(&right, &right, &px, &curve->prime);
    bignum_small(&three, 3);
    bignum_mul_mod(&t, &three, &px, &curve->prime);
    bignum_sub_mod(&right, &right, &t, &curve->prime);
    bignum_add_mod(&right, &right, &curve->b, &curve->prime);
    return bignum_compare_to(&left, &right) == 0;
}

int p256_public_key_valid(const boot_uint8_t* x, const boot_uint8_t* y) {
    return p256_public_key_valid_on(x, y, 32);
}

int p256_verify_on(const boot_uint8_t* hash, boot_uint32_t hash_length,
                   const boot_uint8_t* signature_r, boot_uint32_t r_length,
                   const boot_uint8_t* signature_s, boot_uint32_t s_length,
                   const boot_uint8_t* key_x, const boot_uint8_t* key_y,
                   boot_uint32_t size) {
    const CURVE* curve = curve_of(size);
    BIGNUM r, s, e, w, u1, u2, x;
    /* Three points, not five. Each is a kilobyte and a half of stack, this is
       the deepest frame in the kernel, and the kernel's stack is a fixed size
       - so the two that are only ever used once each are reused rather than
       kept side by side. */
    POINT point, first, sum;

    bignum_from_bytes(&r, signature_r, r_length);
    bignum_from_bytes(&s, signature_s, s_length);
    /* A hash longer than the curve is used from its left, which is what the
       standard says: the leftmost bits, not the rightmost, and not a hash of
       the hash. */
    if (hash_length > size) hash_length = size;
    bignum_from_bytes(&e, hash, hash_length);

    /* Both halves must be inside the order and neither may be zero: a
       signature of (0, 0) verifies against anything if this is skipped, which
       is a real bug real libraries have shipped. */
    if (bignum_is_zero(&r) || bignum_is_zero(&s)) return 0;
    if (bignum_compare_to(&r, &curve->order) >= 0) return 0;
    if (bignum_compare_to(&s, &curve->order) >= 0) return 0;
    if (!p256_public_key_valid_on(key_x, key_y, size)) return 0;

    bignum_reduce(&e, &curve->order);
    if (!bignum_inverse(&w, &s, &curve->order)) return 0;
    if (!bignum_mul_mod(&u1, &e, &w, &curve->order)) return 0;
    if (!bignum_mul_mod(&u2, &r, &w, &curve->order)) return 0;

    memset(&point, 0, sizeof(point));
    bignum_from_bytes(&point.x, curve->base_x, size);
    bignum_from_bytes(&point.y, curve->base_y, size);
    bignum_small(&point.z, 1);
    multiply_point(curve, &first, &u1, &point);

    memset(&point, 0, sizeof(point));
    bignum_from_bytes(&point.x, key_x, size);
    bignum_from_bytes(&point.y, key_y, size);
    bignum_small(&point.z, 1);
    multiply_point(curve, &sum, &u2, &point);

    add_point(curve, &point, &first, &sum);
    sum = point;

    if (!to_affine(curve, &x, &sum)) return 0;
    bignum_reduce(&x, &curve->order);
    return bignum_compare_to(&x, &r) == 0;
}

int p256_verify(const boot_uint8_t hash[32],
                const boot_uint8_t* signature_r, boot_uint32_t r_length,
                const boot_uint8_t* signature_s, boot_uint32_t s_length,
                const boot_uint8_t* key_x, const boot_uint8_t* key_y) {
    return p256_verify_on(hash, 32, signature_r, r_length, signature_s,
                          s_length, key_x, key_y, 32);
}

/* For the bench: k times the base point, as bytes. Not in the header - it
   exists so the arithmetic can be checked against published points. */
int p256_debug_multiply(const boot_uint8_t* scalar, boot_uint8_t* out_x) {
    BIGNUM k, x;
    POINT generator, result;

    prepare();
    bignum_from_bytes(&k, scalar, 32);
    memset(&generator, 0, sizeof(generator));
    bignum_from_bytes(&generator.x, base_x, 32);
    bignum_from_bytes(&generator.y, base_y, 32);
    bignum_small(&generator.z, 1);
    multiply_point(curve_of(32), &result, &k, &generator);
    if (!to_affine(curve_of(32), &x, &result)) return 0;
    bignum_to_bytes(&x, out_x, 32);
    return 1;
}
