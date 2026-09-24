#include "aead.h"
#include "string.h"

/* ChaCha20, Poly1305 and the two of them together, from RFC 8439.
 *
 * Both are small enough to read in one sitting, which is the point of choosing
 * them: this is code whose failure is silent and whose correctness cannot be
 * observed by using it. The only honest way to have it is to write it from the
 * standard and check it against the standard's own vectors - `crypto` on the
 * command line does exactly that, with the numbers from the RFC.
 */

/* ---- ChaCha20 ------------------------------------------------------------ */

static boot_uint32_t rotate_left(boot_uint32_t value, int by) {
    return (value << by) | (value >> (32 - by));
}

static boot_uint32_t read_le32(const boot_uint8_t* at) {
    return (boot_uint32_t)at[0] | ((boot_uint32_t)at[1] << 8) |
           ((boot_uint32_t)at[2] << 16) | ((boot_uint32_t)at[3] << 24);
}

static void write_le32(boot_uint8_t* at, boot_uint32_t value) {
    at[0] = (boot_uint8_t)value;
    at[1] = (boot_uint8_t)(value >> 8);
    at[2] = (boot_uint8_t)(value >> 16);
    at[3] = (boot_uint8_t)(value >> 24);
}

#define QUARTER_ROUND(a, b, c, d)                                        \
    do {                                                                 \
        a += b; d ^= a; d = rotate_left(d, 16);                          \
        c += d; b ^= c; b = rotate_left(b, 12);                          \
        a += b; d ^= a; d = rotate_left(d, 8);                           \
        c += d; b ^= c; b = rotate_left(b, 7);                           \
    } while (0)

static void chacha_block(const boot_uint32_t input[16], boot_uint8_t out[64]) {
    boot_uint32_t state[16];

    for (int at = 0; at < 16; at++) state[at] = input[at];

    /* Twenty rounds is ten of these: four columns, then four diagonals. */
    for (int round = 0; round < 10; round++) {
        QUARTER_ROUND(state[0], state[4], state[8], state[12]);
        QUARTER_ROUND(state[1], state[5], state[9], state[13]);
        QUARTER_ROUND(state[2], state[6], state[10], state[14]);
        QUARTER_ROUND(state[3], state[7], state[11], state[15]);
        QUARTER_ROUND(state[0], state[5], state[10], state[15]);
        QUARTER_ROUND(state[1], state[6], state[11], state[12]);
        QUARTER_ROUND(state[2], state[7], state[8], state[13]);
        QUARTER_ROUND(state[3], state[4], state[9], state[14]);
    }

    /* Added back to the input, which is what stops the whole thing being an
       invertible permutation anybody could run backwards. */
    for (int at = 0; at < 16; at++)
        write_le32(out + at * 4, state[at] + input[at]);
}

static void chacha_state(boot_uint32_t state[16],
                         const boot_uint8_t key[AEAD_KEY_SIZE],
                         const boot_uint8_t nonce[AEAD_NONCE_SIZE],
                         boot_uint32_t counter) {
    /* "expand 32-byte k", which is a constant and not a message. */
    state[0] = 0x61707865; state[1] = 0x3320646e;
    state[2] = 0x79622d32; state[3] = 0x6b206574;
    for (int at = 0; at < 8; at++) state[4 + at] = read_le32(key + at * 4);
    state[12] = counter;
    for (int at = 0; at < 3; at++) state[13 + at] = read_le32(nonce + at * 4);
}

void chacha20(const boot_uint8_t key[AEAD_KEY_SIZE],
              const boot_uint8_t nonce[AEAD_NONCE_SIZE],
              boot_uint32_t counter,
              const boot_uint8_t* input, boot_uint8_t* output,
              boot_uint32_t length) {
    boot_uint32_t state[16];
    boot_uint8_t stream[64];
    boot_uint32_t done = 0;

    chacha_state(state, key, nonce, counter);
    while (done < length) {
        boot_uint32_t take = length - done;

        if (take > 64) take = 64;
        chacha_block(state, stream);
        state[12]++;
        for (boot_uint32_t at = 0; at < take; at++)
            output[done + at] = (boot_uint8_t)(input[done + at] ^ stream[at]);
        done += take;
    }
}

/* ---- Poly1305 ------------------------------------------------------------
 *
 * A message becomes a polynomial and is evaluated at a secret point, modulo
 * 2^130 - 5. The accumulator is five 26-bit limbs so that every product fits
 * in 64 bits without needing 128-bit arithmetic - which keeps this portable
 * and, on a kernel built without the vector registers, actually compilable.
 */

typedef struct {
    boot_uint32_t r[5];
    boot_uint32_t s[4];
    boot_uint32_t accumulator[5];
    boot_uint8_t leftover[16];
    boot_uint32_t used;
} POLY1305;

static void poly_start(POLY1305* self, const boot_uint8_t key[32]) {
    boot_uint32_t t0 = read_le32(key);
    boot_uint32_t t1 = read_le32(key + 4);
    boot_uint32_t t2 = read_le32(key + 8);
    boot_uint32_t t3 = read_le32(key + 12);

    /* r is "clamped": some bits are cleared so that the arithmetic below
       cannot overflow. The standard says which, and why is that the proof of
       the bound depends on it. */
    self->r[0] = t0 & 0x3ffffff;
    self->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    self->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    self->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    self->r[4] = (t3 >> 8) & 0x00fffff;

    self->s[0] = read_le32(key + 16);
    self->s[1] = read_le32(key + 20);
    self->s[2] = read_le32(key + 24);
    self->s[3] = read_le32(key + 28);

    for (int at = 0; at < 5; at++) self->accumulator[at] = 0;
    self->used = 0;
}

static void poly_blocks(POLY1305* self, const boot_uint8_t* data,
                        boot_uint32_t length, int final) {
    const boot_uint32_t high = final ? 0 : (1UL << 24);

    while (length >= 16) {
        boot_uint64_t d0, d1, d2, d3, d4;
        boot_uint32_t carry;
        boot_uint32_t h0, h1, h2, h3, h4;
        boot_uint32_t r0 = self->r[0], r1 = self->r[1], r2 = self->r[2];
        boot_uint32_t r3 = self->r[3], r4 = self->r[4];
        boot_uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

        h0 = self->accumulator[0] + (read_le32(data) & 0x3ffffff);
        h1 = self->accumulator[1] +
             ((((boot_uint64_t)read_le32(data + 3)) >> 2) & 0x3ffffff);
        h2 = self->accumulator[2] +
             ((((boot_uint64_t)read_le32(data + 6)) >> 4) & 0x3ffffff);
        h3 = self->accumulator[3] +
             ((((boot_uint64_t)read_le32(data + 9)) >> 6) & 0x3ffffff);
        h4 = self->accumulator[4] + ((read_le32(data + 12) >> 8) | high);

        d0 = (boot_uint64_t)h0 * r0 + (boot_uint64_t)h1 * s4 +
             (boot_uint64_t)h2 * s3 + (boot_uint64_t)h3 * s2 +
             (boot_uint64_t)h4 * s1;
        d1 = (boot_uint64_t)h0 * r1 + (boot_uint64_t)h1 * r0 +
             (boot_uint64_t)h2 * s4 + (boot_uint64_t)h3 * s3 +
             (boot_uint64_t)h4 * s2;
        d2 = (boot_uint64_t)h0 * r2 + (boot_uint64_t)h1 * r1 +
             (boot_uint64_t)h2 * r0 + (boot_uint64_t)h3 * s4 +
             (boot_uint64_t)h4 * s3;
        d3 = (boot_uint64_t)h0 * r3 + (boot_uint64_t)h1 * r2 +
             (boot_uint64_t)h2 * r1 + (boot_uint64_t)h3 * r0 +
             (boot_uint64_t)h4 * s4;
        d4 = (boot_uint64_t)h0 * r4 + (boot_uint64_t)h1 * r3 +
             (boot_uint64_t)h2 * r2 + (boot_uint64_t)h3 * r1 +
             (boot_uint64_t)h4 * r0;

        carry = (boot_uint32_t)(d0 >> 26); h0 = (boot_uint32_t)d0 & 0x3ffffff;
        d1 += carry;
        carry = (boot_uint32_t)(d1 >> 26); h1 = (boot_uint32_t)d1 & 0x3ffffff;
        d2 += carry;
        carry = (boot_uint32_t)(d2 >> 26); h2 = (boot_uint32_t)d2 & 0x3ffffff;
        d3 += carry;
        carry = (boot_uint32_t)(d3 >> 26); h3 = (boot_uint32_t)d3 & 0x3ffffff;
        d4 += carry;
        carry = (boot_uint32_t)(d4 >> 26); h4 = (boot_uint32_t)d4 & 0x3ffffff;
        /* 2^130 is 5 modulo the prime, so the bits that fell off the top come
           back in at the bottom multiplied by five. That is the whole reason
           this prime was chosen. */
        h0 += carry * 5;
        carry = h0 >> 26; h0 &= 0x3ffffff;
        h1 += carry;

        self->accumulator[0] = h0; self->accumulator[1] = h1;
        self->accumulator[2] = h2; self->accumulator[3] = h3;
        self->accumulator[4] = h4;

        data += 16;
        length -= 16;
    }
}

static void poly_finish(POLY1305* self, boot_uint8_t tag[AEAD_TAG_SIZE]) {
    boot_uint32_t h0, h1, h2, h3, h4;
    boot_uint32_t g0, g1, g2, g3, g4;
    boot_uint32_t mask;
    boot_uint64_t f;

    if (self->used) {
        /* The last, short block: a one bit after the message, then zeroes. */
        self->leftover[self->used++] = 1;
        while (self->used < 16) self->leftover[self->used++] = 0;
        poly_blocks(self, self->leftover, 16, 1);
    }

    h0 = self->accumulator[0]; h1 = self->accumulator[1];
    h2 = self->accumulator[2]; h3 = self->accumulator[3];
    h4 = self->accumulator[4];

    {
        boot_uint32_t carry = h1 >> 26; h1 &= 0x3ffffff;
        h2 += carry; carry = h2 >> 26; h2 &= 0x3ffffff;
        h3 += carry; carry = h3 >> 26; h3 &= 0x3ffffff;
        h4 += carry; carry = h4 >> 26; h4 &= 0x3ffffff;
        h0 += carry * 5; carry = h0 >> 26; h0 &= 0x3ffffff;
        h1 += carry;
    }

    /* Subtract the prime once if the accumulator is at least that big -
       chosen without a branch, so the timing says nothing about the value. */
    g0 = h0 + 5; mask = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + mask; mask = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + mask; mask = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + mask; mask = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + mask - (1UL << 26);

    mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;

    f = (boot_uint64_t)h0 + self->s[0]; h0 = (boot_uint32_t)f;
    f = (boot_uint64_t)h1 + self->s[1] + (f >> 32); h1 = (boot_uint32_t)f;
    f = (boot_uint64_t)h2 + self->s[2] + (f >> 32); h2 = (boot_uint32_t)f;
    f = (boot_uint64_t)h3 + self->s[3] + (f >> 32); h3 = (boot_uint32_t)f;

    write_le32(tag, h0);
    write_le32(tag + 4, h1);
    write_le32(tag + 8, h2);
    write_le32(tag + 12, h3);
}

static void poly_add(POLY1305* self, const boot_uint8_t* data,
                     boot_uint32_t length) {
    if (self->used) {
        boot_uint32_t take = 16 - self->used;

        if (take > length) take = length;
        memcpy(self->leftover + self->used, data, take);
        self->used += take;
        data += take;
        length -= take;
        if (self->used < 16) return;
        poly_blocks(self, self->leftover, 16, 0);
        self->used = 0;
    }
    if (length >= 16) {
        boot_uint32_t whole = length & ~(boot_uint32_t)15;

        poly_blocks(self, data, whole, 0);
        data += whole;
        length -= whole;
    }
    if (length) {
        memcpy(self->leftover, data, length);
        self->used = length;
    }
}

void poly1305(const boot_uint8_t key[32], const boot_uint8_t* data,
              boot_uint32_t length, boot_uint8_t tag[AEAD_TAG_SIZE]) {
    POLY1305 state;

    poly_start(&state, key);
    poly_add(&state, data, length);
    poly_finish(&state, tag);
}

/* ---- The two together ---------------------------------------------------- */

/* The authenticator's key is the first block of the cipher's own keystream,
   which is how one key serves both and how the authenticator's key is
   guaranteed never to repeat. */
static void one_time_key(const boot_uint8_t key[AEAD_KEY_SIZE],
                         const boot_uint8_t nonce[AEAD_NONCE_SIZE],
                         boot_uint8_t out[32]) {
    boot_uint32_t state[16];
    boot_uint8_t block[64];

    chacha_state(state, key, nonce, 0);
    chacha_block(state, block);
    memcpy(out, block, 32);
}

static void authenticate(POLY1305* state, const boot_uint8_t* extra,
                         boot_uint32_t extra_length,
                         const boot_uint8_t* data, boot_uint32_t length) {
    static const boot_uint8_t zeroes[16] = { 0 };
    boot_uint8_t lengths[16];

    /* Both parts are padded to a multiple of sixteen and both lengths go in
       at the end. Without the lengths, a byte moved from the header into the
       message would authenticate just as well - the classic way to break an
       otherwise correct construction. */
    poly_add(state, extra, extra_length);
    if (extra_length % 16) poly_add(state, zeroes, 16 - (extra_length % 16));
    poly_add(state, data, length);
    if (length % 16) poly_add(state, zeroes, 16 - (length % 16));

    memset(lengths, 0, sizeof(lengths));
    for (int at = 0; at < 8; at++) {
        lengths[at] = (boot_uint8_t)(((boot_uint64_t)extra_length >> (at * 8)) & 0xFF);
        lengths[8 + at] = (boot_uint8_t)(((boot_uint64_t)length >> (at * 8)) & 0xFF);
    }
    poly_add(state, lengths, 16);
}

void aead_seal(const boot_uint8_t key[AEAD_KEY_SIZE],
               const boot_uint8_t nonce[AEAD_NONCE_SIZE],
               const boot_uint8_t* extra, boot_uint32_t extra_length,
               boot_uint8_t* data, boot_uint32_t length,
               boot_uint8_t tag[AEAD_TAG_SIZE]) {
    boot_uint8_t authenticator_key[32];
    POLY1305 state;

    one_time_key(key, nonce, authenticator_key);
    /* Counter 1, because block 0 was spent on the authenticator's key. */
    chacha20(key, nonce, 1, data, data, length);

    poly_start(&state, authenticator_key);
    authenticate(&state, extra, extra_length, data, length);
    poly_finish(&state, tag);
}

int aead_open(const boot_uint8_t key[AEAD_KEY_SIZE],
              const boot_uint8_t nonce[AEAD_NONCE_SIZE],
              const boot_uint8_t* extra, boot_uint32_t extra_length,
              boot_uint8_t* data, boot_uint32_t length,
              const boot_uint8_t tag[AEAD_TAG_SIZE]) {
    boot_uint8_t authenticator_key[32];
    boot_uint8_t expected[AEAD_TAG_SIZE];
    POLY1305 state;
    boot_uint8_t difference = 0;

    one_time_key(key, nonce, authenticator_key);
    poly_start(&state, authenticator_key);
    authenticate(&state, extra, extra_length, data, length);
    poly_finish(&state, expected);

    for (int at = 0; at < AEAD_TAG_SIZE; at++)
        difference |= (boot_uint8_t)(expected[at] ^ tag[at]);
    /* Every byte is compared before anything is decided, and the message is
       not touched until the answer is known. */
    if (difference) return 0;

    chacha20(key, nonce, 1, data, data, length);
    return 1;
}
