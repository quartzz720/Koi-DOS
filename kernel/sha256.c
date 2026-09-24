#include "sha256.h"
#include "string.h"

/* SHA-256, HMAC and HKDF.
 *
 * Written from the standards rather than taken from anywhere: FIPS 180-4 for
 * the hash, RFC 2104 for the code, RFC 5869 for the derivation. All three are
 * short enough to write out and all three have published vectors, which is the
 * only reason it is reasonable to write one's own - `crypto` on the command
 * line checks this file against those vectors, and a build that fails them
 * says so before anything trusts it.
 */

static const boot_uint32_t round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static boot_uint32_t rotate(boot_uint32_t value, int by) {
    return (value >> by) | (value << (32 - by));
}

static void compress(SHA256* self, const boot_uint8_t* block) {
    boot_uint32_t schedule[64];
    boot_uint32_t a, b, c, d, e, f, g, h;

    for (int at = 0; at < 16; at++)
        schedule[at] = ((boot_uint32_t)block[at * 4] << 24) |
                       ((boot_uint32_t)block[at * 4 + 1] << 16) |
                       ((boot_uint32_t)block[at * 4 + 2] << 8) |
                       (boot_uint32_t)block[at * 4 + 3];

    for (int at = 16; at < 64; at++) {
        boot_uint32_t s0 = rotate(schedule[at - 15], 7) ^
                           rotate(schedule[at - 15], 18) ^
                           (schedule[at - 15] >> 3);
        boot_uint32_t s1 = rotate(schedule[at - 2], 17) ^
                           rotate(schedule[at - 2], 19) ^
                           (schedule[at - 2] >> 10);

        schedule[at] = schedule[at - 16] + s0 + schedule[at - 7] + s1;
    }

    a = self->state[0]; b = self->state[1];
    c = self->state[2]; d = self->state[3];
    e = self->state[4]; f = self->state[5];
    g = self->state[6]; h = self->state[7];

    for (int at = 0; at < 64; at++) {
        boot_uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
        boot_uint32_t choose = (e & f) ^ ((~e) & g);
        boot_uint32_t temp1 = h + s1 + choose + round_constants[at] +
                              schedule[at];
        boot_uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
        boot_uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        boot_uint32_t temp2 = s0 + majority;

        h = g; g = f; f = e;
        e = d + temp1;
        d = c; c = b; b = a;
        a = temp1 + temp2;
    }

    self->state[0] += a; self->state[1] += b;
    self->state[2] += c; self->state[3] += d;
    self->state[4] += e; self->state[5] += f;
    self->state[6] += g; self->state[7] += h;
}

void sha256_start(SHA256* self) {
    self->state[0] = 0x6a09e667; self->state[1] = 0xbb67ae85;
    self->state[2] = 0x3c6ef372; self->state[3] = 0xa54ff53a;
    self->state[4] = 0x510e527f; self->state[5] = 0x9b05688c;
    self->state[6] = 0x1f83d9ab; self->state[7] = 0x5be0cd19;
    self->length = 0;
    self->used = 0;
}

void sha256_add(SHA256* self, const void* data, boot_uint64_t length) {
    const boot_uint8_t* at = (const boot_uint8_t*)data;

    self->length += length;
    while (length) {
        boot_uint32_t room = SHA256_BLOCK - self->used;
        boot_uint32_t take = length < room ? (boot_uint32_t)length : room;

        memcpy(self->block + self->used, at, take);
        self->used += take;
        at += take;
        length -= take;
        if (self->used == SHA256_BLOCK) {
            compress(self, self->block);
            self->used = 0;
        }
    }
}

void sha256_finish(SHA256* self, boot_uint8_t out[SHA256_SIZE]) {
    boot_uint64_t bits = self->length * 8;
    boot_uint8_t padding[SHA256_BLOCK + 8];
    boot_uint32_t length = 0;

    /* A one bit, then zeroes, then the length in bits - so that two messages
       differing only in trailing zeroes cannot hash the same. */
    padding[length++] = 0x80;
    while ((self->used + length) % SHA256_BLOCK != 56) padding[length++] = 0;
    for (int at = 7; at >= 0; at--)
        padding[length++] = (boot_uint8_t)((bits >> (at * 8)) & 0xFF);

    {
        boot_uint64_t held = self->length;

        sha256_add(self, padding, length);
        self->length = held;           /* the padding is not the message */
    }

    for (int at = 0; at < 8; at++) {
        out[at * 4] = (boot_uint8_t)(self->state[at] >> 24);
        out[at * 4 + 1] = (boot_uint8_t)(self->state[at] >> 16);
        out[at * 4 + 2] = (boot_uint8_t)(self->state[at] >> 8);
        out[at * 4 + 3] = (boot_uint8_t)self->state[at];
    }
}

void sha256(const void* data, boot_uint64_t length,
            boot_uint8_t out[SHA256_SIZE]) {
    SHA256 hash;

    sha256_start(&hash);
    sha256_add(&hash, data, length);
    sha256_finish(&hash, out);
}

void hmac_sha256(const void* key, boot_uint64_t key_length,
                 const void* data, boot_uint64_t length,
                 boot_uint8_t out[SHA256_SIZE]) {
    boot_uint8_t padded[SHA256_BLOCK];
    boot_uint8_t inner_pad[SHA256_BLOCK];
    boot_uint8_t outer_pad[SHA256_BLOCK];
    boot_uint8_t inner[SHA256_SIZE];
    SHA256 hash;

    /* A key longer than a block is hashed down to one; a shorter one is
       padded with zeroes. Both are the standard's, and both matter: a key
       used two different ways here is a code that verifies against nobody. */
    memset(padded, 0, sizeof(padded));
    if (key_length > SHA256_BLOCK) sha256(key, key_length, padded);
    else memcpy(padded, key, (boot_uint32_t)key_length);

    for (int at = 0; at < SHA256_BLOCK; at++) {
        inner_pad[at] = (boot_uint8_t)(padded[at] ^ 0x36);
        outer_pad[at] = (boot_uint8_t)(padded[at] ^ 0x5C);
    }

    sha256_start(&hash);
    sha256_add(&hash, inner_pad, SHA256_BLOCK);
    sha256_add(&hash, data, length);
    sha256_finish(&hash, inner);

    sha256_start(&hash);
    sha256_add(&hash, outer_pad, SHA256_BLOCK);
    sha256_add(&hash, inner, SHA256_SIZE);
    sha256_finish(&hash, out);
}

void hkdf_extract(const void* salt, boot_uint64_t salt_length,
                  const void* key, boot_uint64_t key_length,
                  boot_uint8_t out[SHA256_SIZE]) {
    boot_uint8_t zeroes[SHA256_SIZE];

    /* No salt means a string of zeroes the length of the hash, which is what
       TLS 1.3 hands in for the very first Extract. */
    if (!salt || !salt_length) {
        memset(zeroes, 0, sizeof(zeroes));
        salt = zeroes;
        salt_length = SHA256_SIZE;
    }
    hmac_sha256(salt, salt_length, key, key_length, out);
}

int hkdf_expand(const boot_uint8_t key[SHA256_SIZE],
                const void* info, boot_uint64_t info_length,
                boot_uint8_t* out, boot_uint64_t length) {
    boot_uint8_t block[SHA256_SIZE];
    boot_uint8_t input[SHA256_SIZE + 256 + 1];
    boot_uint32_t have = 0;
    boot_uint8_t counter = 1;

    /* 255 blocks is the standard's limit and 256 bytes of info is this
       implementation's: TLS asks for at most a few dozen of each. */
    if (info_length > 256) return 0;
    if (length > 255 * SHA256_SIZE) return 0;

    while (have < length) {
        boot_uint32_t at = 0;
        boot_uint32_t take;

        if (counter > 1) {
            memcpy(input, block, SHA256_SIZE);
            at = SHA256_SIZE;
        }
        if (info_length) {
            memcpy(input + at, info, (boot_uint32_t)info_length);
            at += (boot_uint32_t)info_length;
        }
        input[at++] = counter;

        hmac_sha256(key, SHA256_SIZE, input, at, block);
        take = (boot_uint32_t)(length - have);
        if (take > SHA256_SIZE) take = SHA256_SIZE;
        memcpy(out + have, block, take);
        have += take;
        counter++;
    }
    return 1;
}

int hkdf_expand_label(const boot_uint8_t key[SHA256_SIZE],
                      const char* label,
                      const boot_uint8_t* context, boot_uint32_t context_length,
                      boot_uint8_t* out, boot_uint32_t length) {
    boot_uint8_t info[512];
    boot_uint32_t at = 0;
    boot_uint32_t label_length = (boot_uint32_t)strlen(label);

    if (label_length + 6 > 255 || context_length > 255) return 0;
    if ((boot_uint64_t)label_length + context_length + 16 > sizeof(info))
        return 0;

    info[at++] = (boot_uint8_t)(length >> 8);
    info[at++] = (boot_uint8_t)length;
    info[at++] = (boot_uint8_t)(label_length + 6);
    memcpy(info + at, "tls13 ", 6);
    at += 6;
    memcpy(info + at, label, label_length);
    at += label_length;
    info[at++] = (boot_uint8_t)context_length;
    if (context_length) {
        memcpy(info + at, context, context_length);
        at += context_length;
    }
    return hkdf_expand(key, info, at, out, length);
}

/* ---- SHA-384 -------------------------------------------------------------
 *
 * The same shape as above in 64-bit words. The constants are the fractional
 * parts of the cube roots of the first eighty primes, and the starting state
 * for SHA-384 is the fractional parts of the square roots of the ninth to
 * sixteenth - different from SHA-512's on purpose, so that a truncated
 * SHA-512 and a SHA-384 of the same message are different numbers.
 */

static const boot_uint64_t wide_constants[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static boot_uint64_t rotate64(boot_uint64_t value, int by) {
    return (value >> by) | (value << (64 - by));
}

static void compress_wide(SHA384* self, const boot_uint8_t* block) {
    boot_uint64_t schedule[80];
    boot_uint64_t a, b, c, d, e, f, g, h;

    for (int at = 0; at < 16; at++) {
        boot_uint64_t value = 0;

        for (int byte = 0; byte < 8; byte++)
            value = (value << 8) | block[at * 8 + byte];
        schedule[at] = value;
    }
    for (int at = 16; at < 80; at++) {
        boot_uint64_t s0 = rotate64(schedule[at - 15], 1) ^
                           rotate64(schedule[at - 15], 8) ^
                           (schedule[at - 15] >> 7);
        boot_uint64_t s1 = rotate64(schedule[at - 2], 19) ^
                           rotate64(schedule[at - 2], 61) ^
                           (schedule[at - 2] >> 6);

        schedule[at] = schedule[at - 16] + s0 + schedule[at - 7] + s1;
    }

    a = self->state[0]; b = self->state[1];
    c = self->state[2]; d = self->state[3];
    e = self->state[4]; f = self->state[5];
    g = self->state[6]; h = self->state[7];

    for (int at = 0; at < 80; at++) {
        boot_uint64_t s1 = rotate64(e, 14) ^ rotate64(e, 18) ^ rotate64(e, 41);
        boot_uint64_t choose = (e & f) ^ ((~e) & g);
        boot_uint64_t temp1 = h + s1 + choose + wide_constants[at] + schedule[at];
        boot_uint64_t s0 = rotate64(a, 28) ^ rotate64(a, 34) ^ rotate64(a, 39);
        boot_uint64_t majority = (a & b) ^ (a & c) ^ (b & c);
        boot_uint64_t temp2 = s0 + majority;

        h = g; g = f; f = e;
        e = d + temp1;
        d = c; c = b; b = a;
        a = temp1 + temp2;
    }

    self->state[0] += a; self->state[1] += b;
    self->state[2] += c; self->state[3] += d;
    self->state[4] += e; self->state[5] += f;
    self->state[6] += g; self->state[7] += h;
}

void sha384_start(SHA384* self) {
    self->state[0] = 0xcbbb9d5dc1059ed8ULL;
    self->state[1] = 0x629a292a367cd507ULL;
    self->state[2] = 0x9159015a3070dd17ULL;
    self->state[3] = 0x152fecd8f70e5939ULL;
    self->state[4] = 0x67332667ffc00b31ULL;
    self->state[5] = 0x8eb44a8768581511ULL;
    self->state[6] = 0xdb0c2e0d64f98fa7ULL;
    self->state[7] = 0x47b5481dbefa4fa4ULL;
    self->length = 0;
    self->used = 0;
}

void sha384_add(SHA384* self, const void* data, boot_uint64_t length) {
    const boot_uint8_t* at = (const boot_uint8_t*)data;

    self->length += length;
    while (length) {
        boot_uint32_t room = SHA512_BLOCK - self->used;
        boot_uint32_t take = length < room ? (boot_uint32_t)length : room;

        memcpy(self->block + self->used, at, take);
        self->used += take;
        at += take;
        length -= take;
        if (self->used == SHA512_BLOCK) {
            compress_wide(self, self->block);
            self->used = 0;
        }
    }
}

void sha384_finish(SHA384* self, boot_uint8_t out[SHA384_SIZE]) {
    boot_uint64_t bits = self->length * 8;
    boot_uint8_t padding[SHA512_BLOCK + 16];
    boot_uint32_t length = 0;

    padding[length++] = 0x80;
    while ((self->used + length) % SHA512_BLOCK != 112) padding[length++] = 0;
    /* The length is 128 bits here; the top half is zero for any message this
       system will ever hash. */
    for (int at = 0; at < 8; at++) padding[length++] = 0;
    for (int at = 7; at >= 0; at--)
        padding[length++] = (boot_uint8_t)((bits >> (at * 8)) & 0xFF);

    {
        boot_uint64_t held = self->length;

        sha384_add(self, padding, length);
        self->length = held;
    }

    for (int at = 0; at < 6; at++)
        for (int byte = 0; byte < 8; byte++)
            out[at * 8 + byte] =
                (boot_uint8_t)((self->state[at] >> (56 - byte * 8)) & 0xFF);
}

void sha384(const void* data, boot_uint64_t length,
            boot_uint8_t out[SHA384_SIZE]) {
    SHA384 hash;

    sha384_start(&hash);
    sha384_add(&hash, data, length);
    sha384_finish(&hash, out);
}
