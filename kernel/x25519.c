#include "x25519.h"

/* X25519, on the Montgomery ladder.
 *
 * ---- Where this arrangement comes from -----------------------------------
 *
 * The field arithmetic and the ladder follow TweetNaCl's, which is in the
 * public domain and was written to be small enough to audit by reading. That
 * is exactly the property wanted here, and the alternative - inventing a
 * different arrangement of the same arithmetic - would produce something with
 * the same behaviour, no better, and unreviewed by anybody. What is here is
 * rewritten in this project's style with the reasoning spelled out; the design
 * is not claimed as new.
 *
 * ---- What the numbers are -------------------------------------------------
 *
 * Arithmetic modulo 2^255 - 19, on numbers held as sixteen limbs of sixteen
 * bits each in signed 64-bit words. Sixteen bits leaves 48 bits of headroom in
 * every word, so a whole 16x16 multiply can be accumulated before anything
 * needs carrying - which is why this needs no 128-bit type and compiles the
 * same on any 64-bit compiler.
 *
 * 2^256 is 38 modulo the prime, so the limbs that overflow the top come back
 * in at the bottom multiplied by 38. Every reduction below is that one fact.
 *
 * ---- Constant time --------------------------------------------------------
 *
 * Nothing here branches on a secret and nothing indexes memory by one. The
 * ladder does the same work for a zero bit as for a one, and the swap that
 * decides which is which is arithmetic on a mask rather than an `if`. A
 * conditional that depends on a key bit is a key bit that can be measured from
 * outside - this has been how real implementations were broken, repeatedly.
 */

typedef long long field[16];

static const field constant_121665 = { 0xDB41, 1 };

static void carry(field out) {
    for (int at = 0; at < 16; at++) {
        long long over;

        out[at] += 1LL << 16;
        over = out[at] >> 16;
        /* The top limb wraps around into the bottom one times 38; every other
           one carries into its neighbour. Written without a branch so the
           loop is one shape. */
        out[(at + 1) * (at < 15)] += over - 1 + 37 * (over - 1) * (at == 15);
        out[at] -= over << 16;
    }
}

/* Swap two numbers, or do not, without the processor knowing which. */
static void swap_if(field a, field b, int condition) {
    long long mask = ~((long long)condition - 1);

    for (int at = 0; at < 16; at++) {
        long long difference = mask & (a[at] ^ b[at]);

        a[at] ^= difference;
        b[at] ^= difference;
    }
}

static void pack(boot_uint8_t* out, const field number) {
    field reduced;
    field minus_prime;

    for (int at = 0; at < 16; at++) reduced[at] = number[at];
    carry(reduced);
    carry(reduced);
    carry(reduced);

    /* The value may still be one or two whole primes too big. Subtract the
       prime twice, conditionally and without branching, and what is left is
       the one representation in range. */
    for (int round = 0; round < 2; round++) {
        int borrow;

        minus_prime[0] = reduced[0] - 0xFFED;
        for (int at = 1; at < 15; at++) {
            minus_prime[at] = reduced[at] - 0xFFFF -
                              ((minus_prime[at - 1] >> 16) & 1);
            minus_prime[at - 1] &= 0xFFFF;
        }
        minus_prime[15] = reduced[15] - 0x7FFF -
                          ((minus_prime[14] >> 16) & 1);
        borrow = (int)((minus_prime[15] >> 16) & 1);
        minus_prime[14] &= 0xFFFF;
        swap_if(reduced, minus_prime, 1 - borrow);
    }

    for (int at = 0; at < 16; at++) {
        out[2 * at] = (boot_uint8_t)(reduced[at] & 0xFF);
        out[2 * at + 1] = (boot_uint8_t)(reduced[at] >> 8);
    }
}

static void unpack(field out, const boot_uint8_t* in) {
    for (int at = 0; at < 16; at++)
        out[at] = (long long)in[2 * at] + ((long long)in[2 * at + 1] << 8);
    /* The top bit of the last byte is ignored, as the standard requires: it
       is not part of the number, and a peer that sets it must not get a
       different answer from one that does not. */
    out[15] &= 0x7FFF;
}

static void add(field out, const field a, const field b) {
    for (int at = 0; at < 16; at++) out[at] = a[at] + b[at];
}

static void subtract(field out, const field a, const field b) {
    for (int at = 0; at < 16; at++) out[at] = a[at] - b[at];
}

static void multiply(field out, const field a, const field b) {
    long long product[31];

    for (int at = 0; at < 31; at++) product[at] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            product[i + j] += a[i] * b[j];
    for (int at = 0; at < 15; at++) product[at] += 38 * product[at + 16];
    for (int at = 0; at < 16; at++) out[at] = product[at];
    carry(out);
    carry(out);
}

static void square(field out, const field a) {
    multiply(out, a, a);
}

/* The inverse, by raising to the power p - 2. There is no division here and
   no shortcut: Fermat's little theorem is the whole method, and 254 squarings
   is what it costs. */
static void invert(field out, const field in) {
    field working;

    for (int at = 0; at < 16; at++) working[at] = in[at];
    for (int at = 253; at >= 0; at--) {
        square(working, working);
        if (at != 2 && at != 4) multiply(working, working, in);
    }
    for (int at = 0; at < 16; at++) out[at] = working[at];
}

static int scalar_multiply(boot_uint8_t out[X25519_SIZE],
                           const boot_uint8_t scalar[X25519_SIZE],
                           const boot_uint8_t point[X25519_SIZE]) {
    boot_uint8_t clamped[32];
    field x, a, b, c, d, e, f;
    boot_uint8_t empty = 0;

    for (int at = 0; at < 31; at++) clamped[at] = scalar[at];
    /* Clamping, which the standard requires: the bottom three bits cleared so
       the scalar is a multiple of the cofactor, the top bit set so the ladder
       always runs the same number of steps, and the bit above it cleared so
       the value stays in range. */
    clamped[31] = (boot_uint8_t)((scalar[31] & 127) | 64);
    clamped[0] = (boot_uint8_t)(scalar[0] & 248);

    unpack(x, point);
    for (int at = 0; at < 16; at++) {
        b[at] = x[at];
        a[at] = c[at] = d[at] = 0;
    }
    a[0] = d[0] = 1;

    /* One step per bit, from the top down. Each step does the same doubling
       and adding whichever the bit is; the two candidate points are swapped
       into place before and after by the mask. */
    for (int at = 254; at >= 0; at--) {
        int bit = (clamped[at >> 3] >> (at & 7)) & 1;

        swap_if(a, b, bit);
        swap_if(c, d, bit);
        add(e, a, c);
        subtract(a, a, c);
        add(c, b, d);
        subtract(b, b, d);
        square(d, e);
        square(f, a);
        multiply(a, c, a);
        multiply(c, b, e);
        add(e, a, c);
        subtract(a, a, c);
        square(b, a);
        subtract(c, d, f);
        multiply(a, c, constant_121665);
        add(a, a, d);
        multiply(c, c, a);
        multiply(a, d, f);
        multiply(d, b, x);
        square(b, e);
        swap_if(a, b, bit);
        swap_if(c, d, bit);
    }

    invert(c, c);
    multiply(a, a, c);
    pack(out, a);

    for (int at = 0; at < X25519_SIZE; at++) empty |= out[at];
    return empty != 0;
}

void x25519_public(boot_uint8_t out[X25519_SIZE],
                   const boot_uint8_t secret[X25519_SIZE]) {
    static const boot_uint8_t base_point[X25519_SIZE] = { 9 };

    (void)scalar_multiply(out, secret, base_point);
}

int x25519(boot_uint8_t out[X25519_SIZE],
           const boot_uint8_t secret[X25519_SIZE],
           const boot_uint8_t their_public[X25519_SIZE]) {
    return scalar_multiply(out, secret, their_public);
}
