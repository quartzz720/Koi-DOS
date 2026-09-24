#include "bignum.h"
#include "string.h"

/* Big numbers, in 32-bit limbs with 64-bit intermediates.
 *
 * Schoolbook multiplication and schoolbook division, because a 2048-bit
 * verification is seventeen squarings and a handful of multiplies - a
 * millisecond of work on this machine - and Montgomery arithmetic would be
 * three times the code to save a millisecond that nobody is waiting for.
 * Karatsuba would be the same trade.
 *
 * Limbs are least significant first, which is the order arithmetic wants; the
 * wire uses the opposite order, and the two conversions below are the only
 * place that matters.
 */

static void trim(BIGNUM* value) {
    while (value->count > 0 && !value->limb[value->count - 1]) value->count--;
}

int bignum_from_bytes(BIGNUM* out, const boot_uint8_t* data,
                      boot_uint32_t length) {
    boot_uint32_t at = 0;

    /* Leading zeroes carry no value and a signed DER integer has one on
       purpose; dropped here so that a 2048-bit modulus is 64 limbs and not
       65. */
    while (at < length && !data[at]) at++;
    if (length - at > BIGNUM_MAX_BYTES) return 0;

    memset(out, 0, sizeof(BIGNUM));
    {
        boot_uint32_t bytes = length - at;
        boot_uint32_t index = 0;

        while (index < bytes) {
            boot_uint32_t byte = data[length - 1 - index];

            out->limb[index / 4] |= byte << ((index % 4) * 8);
            index++;
        }
        out->count = (int)((bytes + 3) / 4);
    }
    trim(out);
    return 1;
}

int bignum_to_bytes(const BIGNUM* value, boot_uint8_t* out,
                    boot_uint32_t length) {
    for (boot_uint32_t at = 0; at < length; at++) {
        boot_uint32_t index = length - 1 - at;
        boot_uint32_t limb = (int)(at / 4) < value->count
                             ? value->limb[at / 4] : 0;

        out[index] = (boot_uint8_t)((limb >> ((at % 4) * 8)) & 0xFF);
    }
    return 1;
}

static int compare(const BIGNUM* left, const BIGNUM* right) {
    if (left->count != right->count)
        return left->count > right->count ? 1 : -1;
    for (int at = left->count - 1; at >= 0; at--)
        if (left->limb[at] != right->limb[at])
            return left->limb[at] > right->limb[at] ? 1 : -1;
    return 0;
}

static void subtract(BIGNUM* left, const BIGNUM* right) {
    boot_uint64_t borrow = 0;

    for (int at = 0; at < left->count; at++) {
        boot_uint64_t value = (boot_uint64_t)left->limb[at] - borrow -
                              (at < right->count ? right->limb[at] : 0);

        left->limb[at] = (boot_uint32_t)value;
        borrow = (value >> 32) & 1;
    }
    trim(left);
}

static void shift_left_one(BIGNUM* value) {
    boot_uint32_t carry = 0;

    for (int at = 0; at < value->count; at++) {
        boot_uint32_t limb = value->limb[at];

        value->limb[at] = (limb << 1) | carry;
        carry = limb >> 31;
    }
    if (carry && value->count < BIGNUM_MAX_LIMBS + 2)
        value->limb[value->count++] = carry;
}

static int bit_of(const BIGNUM* value, int bit) {
    if (bit >= value->count * 32) return 0;
    return (value->limb[bit / 32] >> (bit % 32)) & 1;
}

static int bit_length(const BIGNUM* value) {
    int at;

    if (!value->count) return 0;
    for (at = 31; at >= 0; at--)
        if (value->limb[value->count - 1] & (1U << at)) break;
    return (value->count - 1) * 32 + at + 1;
}

/* The remainder of a double-width product, by long division one bit at a
 * time.
 *
 * A thousand-odd iterations for a 2048-bit modulus, and there are about fifty
 * of these in a verification - which is to say it is fast enough, and it is
 * the shortest correct thing that can be written. */
static void reduce(BIGNUM* value, const BIGNUM* modulus) {
    BIGNUM remainder;
    int top = bit_length(value);

    if (compare(value, modulus) < 0) return;

    memset(&remainder, 0, sizeof(remainder));
    for (int at = top - 1; at >= 0; at--) {
        shift_left_one(&remainder);
        if (bit_of(value, at)) {
            if (!remainder.count) remainder.count = 1;
            remainder.limb[0] |= 1;
        }
        if (compare(&remainder, modulus) >= 0) subtract(&remainder, modulus);
    }
    *value = remainder;
}

/* left * right, reduced.
 *
 * The product is built at full width in an array of its own and then divided
 * by the modulus, one bit at a time, keeping only the remainder. That division
 * is the slow part - eight thousand iterations for a 4096-bit key - and it is
 * what makes the rest of this file short: no Montgomery form, no precomputed
 * inverse, nothing to get subtly wrong.
 *
 * The first version tried to avoid the wide array by splitting the operands
 * and recursing. It recursed forever on the first 4096-bit key it met, which
 * is the sort of thing that only shows up against a real server: every test
 * key was 2048 bits and every one of them worked.
 */
static void reduce_wide(const boot_uint32_t* value, int count,
                        const BIGNUM* modulus, BIGNUM* out) {
    BIGNUM remainder;
    int top = count * 32;

    memset(&remainder, 0, sizeof(remainder));
    while (top > 0 && !((value[(top - 1) / 32] >> ((top - 1) % 32)) & 1)) top--;

    for (int at = top - 1; at >= 0; at--) {
        shift_left_one(&remainder);
        if ((value[at / 32] >> (at % 32)) & 1) {
            if (!remainder.count) remainder.count = 1;
            remainder.limb[0] |= 1;
        }
        if (compare(&remainder, modulus) >= 0) subtract(&remainder, modulus);
    }
    *out = remainder;
}

/* ---- Montgomery multiplication -------------------------------------------
 *
 * The bit-by-bit division below is correct and slow: one iteration per bit of
 * the product, so a single field multiplication on P-256 costs five hundred
 * passes over the number. A curve verification does six thousand of those, and
 * that measured at 154 ms - which on a desktop with no threads is a machine
 * that stops dead for a second every time a certificate is checked. Koi's own
 * site froze the machine for long enough that she thought it had crashed.
 *
 * Montgomery's method replaces the division with a multiplication. Numbers are
 * kept multiplied by R = 2^(32k), and in that form the reduction after a
 * product is a multiply-and-shift instead of a long division. The conversion
 * in and out costs one extra pass each, which is why this is used as
 * `mont(mont(a, b), R^2)` - two passes, still forty times cheaper than five
 * hundred.
 *
 * It needs an odd modulus, which every prime and every curve order is. An even
 * one falls back to the slow path rather than being refused: correctness first,
 * speed where it is available.
 *
 * The two constants per modulus - the inverse of its lowest limb, and R^2 -
 * are worked out once and cached, because the same handful of moduli are used
 * over and over: a curve's prime, its order, a key's modulus.
 */

#define MONTGOMERY_CACHE 4

typedef struct {
    BIGNUM modulus;
    BIGNUM r_squared;
    boot_uint32_t inverse;          /* -modulus^-1 mod 2^32 */
    int used;
} MONTGOMERY;

static MONTGOMERY cache[MONTGOMERY_CACHE];
static int cache_next;

/* The inverse of an odd number modulo 2^32, by Newton's iteration: each step
   doubles the number of correct bits, so five steps cover thirty-two. */
static boot_uint32_t inverse_of_limb(boot_uint32_t value) {
    boot_uint32_t x = value;

    for (int at = 0; at < 5; at++) x *= 2u - value * x;
    return (boot_uint32_t)(0u - x);
}

static const MONTGOMERY* prepare_montgomery(const BIGNUM* modulus) {
    MONTGOMERY* entry;

    if (!modulus->count || !(modulus->limb[0] & 1)) return 0;

    for (int at = 0; at < MONTGOMERY_CACHE; at++)
        if (cache[at].used && compare(&cache[at].modulus, modulus) == 0)
            return &cache[at];

    entry = &cache[cache_next];
    cache_next = (cache_next + 1) % MONTGOMERY_CACHE;
    entry->modulus = *modulus;
    entry->inverse = inverse_of_limb(modulus->limb[0]);

    /* R^2 mod n, by doubling 64k times. Done with the slow reduction, once
       per modulus, which is the whole of what the slow path is still for. */
    {
        BIGNUM value;

        memset(&value, 0, sizeof(value));
        value.limb[0] = 1;
        value.count = 1;
        for (int at = 0; at < modulus->count * 64; at++) {
            shift_left_one(&value);
            if (compare(&value, modulus) >= 0) subtract(&value, modulus);
        }
        entry->r_squared = value;
    }
    entry->used = 1;
    return entry;
}

/* out = left * right * R^-1 mod modulus, the interleaved form: one limb of
   the product and one step of the reduction at a time, so nothing ever grows
   wider than the modulus plus two limbs. */
static void montgomery_multiply(BIGNUM* out, const BIGNUM* left,
                                const BIGNUM* right, const BIGNUM* modulus,
                                boot_uint32_t inverse) {
    boot_uint32_t t[BIGNUM_MAX_LIMBS + 3];
    int count = modulus->count;

    for (int at = 0; at < count + 3; at++) t[at] = 0;

    for (int i = 0; i < count; i++) {
        boot_uint64_t carry = 0;
        boot_uint32_t a = i < left->count ? left->limb[i] : 0;
        boot_uint32_t m;
        boot_uint64_t sum;

        for (int j = 0; j < count; j++) {
            boot_uint64_t value = (boot_uint64_t)a *
                                  (j < right->count ? right->limb[j] : 0) +
                                  t[j] + carry;

            t[j] = (boot_uint32_t)value;
            carry = value >> 32;
        }
        sum = (boot_uint64_t)t[count] + carry;
        t[count] = (boot_uint32_t)sum;
        t[count + 1] += (boot_uint32_t)(sum >> 32);

        m = (boot_uint32_t)(t[0] * inverse);
        carry = 0;
        for (int j = 0; j < count; j++) {
            boot_uint64_t value = (boot_uint64_t)m * modulus->limb[j] + t[j] +
                                  carry;

            t[j] = (boot_uint32_t)value;
            carry = value >> 32;
        }
        sum = (boot_uint64_t)t[count] + carry;
        t[count] = (boot_uint32_t)sum;
        t[count + 1] += (boot_uint32_t)(sum >> 32);

        /* The lowest limb is now zero by construction; shifting it off is the
           division by 2^32 that gives this method its name. */
        for (int j = 0; j <= count + 1; j++) t[j] = t[j + 1];
        t[count + 2] = 0;
    }

    /* Only the limbs in play are cleared. A BIGNUM is half a kilobyte and
       this runs six thousand times in one signature check: clearing all of it
       cost more than the arithmetic did. */
    for (int at = 0; at <= count; at++) out->limb[at] = t[at];
    for (int at = count + 1; at < BIGNUM_MAX_LIMBS + 2; at++) out->limb[at] = 0;
    out->count = count + 1;
    trim(out);
    if (compare(out, modulus) >= 0) subtract(out, modulus);
}

static int multiply_mod_slow(BIGNUM* out, const BIGNUM* left,
                             const BIGNUM* right, const BIGNUM* modulus);

static int multiply_mod(BIGNUM* out, const BIGNUM* left, const BIGNUM* right,
                        const BIGNUM* modulus) {
    const MONTGOMERY* entry = prepare_montgomery(modulus);
    const BIGNUM* a = left;
    const BIGNUM* b = right;
    BIGNUM reduced_left;
    BIGNUM reduced_right;
    BIGNUM product;

    if (!entry) return multiply_mod_slow(out, left, right, modulus);

    /* Both operands must be inside the modulus - Montgomery's reduction is
       only correct there. Copied only when that is not already true, which in
       the curve code it always is: half a kilobyte moved for nothing, six
       thousand times a signature, is most of what this function used to
       cost. */
    if (compare(a, modulus) >= 0) {
        reduced_left = *left;
        reduce(&reduced_left, modulus);
        a = &reduced_left;
    }
    if (compare(b, modulus) >= 0) {
        reduced_right = *right;
        reduce(&reduced_right, modulus);
        b = &reduced_right;
    }

    montgomery_multiply(&product, a, b, modulus, entry->inverse);
    montgomery_multiply(out, &product, &entry->r_squared, modulus,
                        entry->inverse);
    return 1;
}

static int multiply_mod_slow(BIGNUM* out, const BIGNUM* left,
                             const BIGNUM* right, const BIGNUM* modulus) {
    boot_uint32_t product[BIGNUM_MAX_LIMBS * 2];
    int count = left->count + right->count;

    if (count > BIGNUM_MAX_LIMBS * 2) return 0;
    for (int at = 0; at < count; at++) product[at] = 0;

    for (int i = 0; i < left->count; i++) {
        boot_uint64_t carry = 0;

        for (int j = 0; j < right->count; j++) {
            boot_uint64_t value = (boot_uint64_t)left->limb[i] * right->limb[j] +
                                  product[i + j] + carry;

            product[i + j] = (boot_uint32_t)value;
            carry = value >> 32;
        }
        {
            int at = i + right->count;

            while (carry && at < count) {
                boot_uint64_t value = product[at] + carry;

                product[at] = (boot_uint32_t)value;
                carry = value >> 32;
                at++;
            }
        }
    }

    reduce_wide(product, count, modulus, out);
    return 1;
}

int bignum_modexp(BIGNUM* out, const BIGNUM* base, boot_uint32_t exponent,
                  const BIGNUM* modulus) {
    BIGNUM result;
    BIGNUM square = *base;
    int highest = 31;

    if (!modulus->count) return 0;

    memset(&result, 0, sizeof(result));
    result.limb[0] = 1;
    result.count = 1;

    reduce(&square, modulus);
    while (highest > 0 && !((exponent >> highest) & 1)) highest--;

    /* Square and multiply, from the top bit down. The exponent is public -
       65537 for every certificate anybody issues - so there is nothing to
       hide in the shape of this loop. */
    for (int at = highest; at >= 0; at--) {
        if (at != highest)
            if (!multiply_mod(&result, &result, &result, modulus)) return 0;
        if ((exponent >> at) & 1)
            if (!multiply_mod(&result, &result, &square, modulus)) return 0;
    }

    *out = result;
    return 1;
}

/* ---- What a curve needs -------------------------------------------------- */

void bignum_zero(BIGNUM* out) {
    memset(out, 0, sizeof(BIGNUM));
}

void bignum_small(BIGNUM* out, boot_uint32_t value) {
    memset(out, 0, sizeof(BIGNUM));
    if (value) { out->limb[0] = value; out->count = 1; }
}

int bignum_is_zero(const BIGNUM* value) {
    return value->count == 0;
}

int bignum_compare_to(const BIGNUM* left, const BIGNUM* right) {
    return compare(left, right);
}

void bignum_reduce(BIGNUM* value, const BIGNUM* modulus) {
    reduce(value, modulus);
}

void bignum_add_mod(BIGNUM* out, const BIGNUM* left, const BIGNUM* right,
                    const BIGNUM* modulus) {
    BIGNUM sum;
    boot_uint64_t carry = 0;
    int count = left->count > right->count ? left->count : right->count;

    memset(&sum, 0, sizeof(sum));
    for (int at = 0; at < count; at++) {
        boot_uint64_t value = (boot_uint64_t)(at < left->count ? left->limb[at] : 0) +
                              (at < right->count ? right->limb[at] : 0) + carry;

        sum.limb[at] = (boot_uint32_t)value;
        carry = value >> 32;
    }
    sum.count = count;
    if (carry && sum.count < BIGNUM_MAX_LIMBS + 2)
        sum.limb[sum.count++] = (boot_uint32_t)carry;
    trim(&sum);
    reduce(&sum, modulus);
    *out = sum;
}

void bignum_sub_mod(BIGNUM* out, const BIGNUM* left, const BIGNUM* right,
                    const BIGNUM* modulus) {
    BIGNUM value = *left;

    /* Borrowing is avoided by adding the modulus first when it would be
       needed: the numbers here are always already reduced. */
    if (compare(&value, right) < 0) {
        BIGNUM raised;
        boot_uint64_t carry = 0;
        int count = modulus->count > value.count ? modulus->count : value.count;

        memset(&raised, 0, sizeof(raised));
        for (int at = 0; at < count; at++) {
            boot_uint64_t sum = (boot_uint64_t)(at < value.count ? value.limb[at] : 0) +
                                (at < modulus->count ? modulus->limb[at] : 0) + carry;

            raised.limb[at] = (boot_uint32_t)sum;
            carry = sum >> 32;
        }
        raised.count = count;
        if (carry && raised.count < BIGNUM_MAX_LIMBS + 2)
            raised.limb[raised.count++] = (boot_uint32_t)carry;
        trim(&raised);
        value = raised;
    }
    subtract(&value, right);
    reduce(&value, modulus);
    *out = value;
}

int bignum_mul_mod(BIGNUM* out, const BIGNUM* left, const BIGNUM* right,
                   const BIGNUM* modulus) {
    return multiply_mod(out, left, right, modulus);
}

int bignum_inverse(BIGNUM* out, const BIGNUM* value, const BIGNUM* modulus) {
    BIGNUM exponent = *modulus;
    BIGNUM result;
    BIGNUM square = *value;
    int top;

    /* modulus - 2, done by hand because the only subtraction here is by two
       and the modulus of a curve is always odd. */
    if (!exponent.count) return 0;
    if (exponent.limb[0] < 2) return 0;
    exponent.limb[0] -= 2;

    bignum_small(&result, 1);
    reduce(&square, modulus);
    top = bit_length(&exponent);

    for (int at = top - 1; at >= 0; at--) {
        if (!multiply_mod(&result, &result, &result, modulus)) return 0;
        if (bit_of(&exponent, at))
            if (!multiply_mod(&result, &result, &square, modulus)) return 0;
    }
    *out = result;
    return 1;
}
