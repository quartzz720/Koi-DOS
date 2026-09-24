#include "crypto_check.h"
#include "sha256.h"
#include "aead.h"
#include "x25519.h"
#include "random.h"
#include "x509.h"
#include "bignum.h"
#include "p256.h"
#include "string.h"


/* The machine checking its own cryptography, against the numbers in the
 * standards.
 *
 * These are run on the host while the code is being written, which proves the
 * arithmetic. This runs the same vectors on the machine that will use them,
 * which proves something different and just as necessary: that this compiler,
 * these flags and this processor produce the same answers. A kernel built with
 * one wrong optimisation is a kernel whose handshake fails in a way nobody can
 * read.
 *
 * It is a command rather than a startup check because it costs a second and
 * because the useful moment to run it is on somebody else's machine, when
 * something has gone wrong.
 */

static int checks;
static int failures;
/* Where the report goes. The command layer prints to the screen and to the
   kernel's log at once; writing to the console directly would put this on the
   screen only - and a self-check whose output cannot be captured with `log`
   is a self-check that is useless on the machine that failed, which is the
   only machine it exists for. */
static void (*say)(const char*);

static int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static boot_uint32_t unhex(const char* text, boot_uint8_t* out) {
    boot_uint32_t length = 0;

    while (text[0] && text[1]) {
        int high = digit(text[0]);
        int low = digit(text[1]);

        if (high < 0 || low < 0) break;
        out[length++] = (boot_uint8_t)((high << 4) | low);
        text += 2;
    }
    return length;
}

static void expect(const char* what, const boot_uint8_t* got,
                   boot_uint32_t length, const char* wanted) {
    boot_uint8_t expected[256];
    boot_uint32_t expected_length = unhex(wanted, expected);
    int same = expected_length == length &&
               !memcmp(got, expected, length);

    checks++;
    say(same ? "  [pass] " : "  [FAIL] ");
    say(what);
    say("\n");
    if (same) return;

    failures++;
    /* What it produced, because "FAIL" on its own has never once been enough
       to find anything. */
    say("         got  ");
    for (boot_uint32_t at = 0; at < length; at++) {
        static const char* hex = "0123456789abcdef";
        char pair[3];

        pair[0] = hex[got[at] >> 4];
        pair[1] = hex[got[at] & 15];
        pair[2] = 0;
        say(pair);
    }
    say("\n         want ");
    say(wanted);
    say("\n");
}

static void claim(const char* what, int condition) {
    checks++;
    if (!condition) failures++;
    say(condition ? "  [pass] " : "  [FAIL] ");
    say(what);
    say("\n");
}

int crypto_check(void (*write)(const char*)) {
    boot_uint8_t out[128];
    boot_uint8_t key[64];
    boot_uint8_t nonce[12];
    boot_uint8_t tag[16];
    boot_uint8_t data[128];

    checks = 0;
    failures = 0;
    say = write;

    say("SHA-256\n");
    sha256("abc", 3, out);
    expect("abc", out, 32,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        SHA256 hash;
        char block[100];

        memset(block, 'a', sizeof(block));
        sha256_start(&hash);
        for (int at = 0; at < 10000; at++)
            sha256_add(&hash, block, sizeof(block));
        sha256_finish(&hash, out);
        expect("a million letters", out, 32,
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    say("HMAC-SHA256\n");
    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, out);
    expect("RFC 4231 case 1", out, 32,
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    expect("RFC 4231 case 2", out, 32,
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    say("HKDF\n");
    {
        boot_uint8_t salt[16];
        boot_uint8_t info[16];
        boot_uint8_t secret[32];
        boot_uint32_t length = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
                                     key);

        unhex("000102030405060708090a0b0c", salt);
        unhex("f0f1f2f3f4f5f6f7f8f9", info);
        hkdf_extract(salt, 13, key, length, secret);
        expect("RFC 5869 extract", secret, 32,
               "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
        hkdf_expand(secret, info, 10, out, 42);
        expect("RFC 5869 expand", out, 42,
               "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
               "34007208d5b887185865");
    }

    say("ChaCha20-Poly1305\n");
    {
        static const char text[] =
            "Ladies and Gentlemen of the class of '99: If I could offer you "
            "only one tip for the future, sunscreen would be it.";
        boot_uint8_t extra[12];
        boot_uint32_t length = (boot_uint32_t)strlen(text);

        unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
              key);
        unhex("070000004041424344454647", nonce);
        unhex("50515253c0c1c2c3c4c5c6c7", extra);
        memcpy(data, text, length);
        aead_seal(key, nonce, extra, 12, data, length, tag);
        expect("RFC 8439 tag", tag, 16, "1ae10b594f09e26a7e902ecbd0600691");
        claim("what was sealed opens again",
              aead_open(key, nonce, extra, 12, data, length, tag) &&
              !memcmp(data, text, length));
        aead_seal(key, nonce, extra, 12, data, length, tag);
        data[7] ^= 0x40;
        claim("one altered byte is refused",
              !aead_open(key, nonce, extra, 12, data, length, tag));
    }

    say("X25519\n");
    {
        boot_uint8_t mine[32], theirs[32], shared[32], other[32];

        unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
              mine);
        unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
              theirs);
        x25519(out, mine, theirs);
        expect("RFC 7748 vector", out, 32,
               "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

        unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
              mine);
        unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",
              theirs);
        x25519_public(out, mine);
        expect("a public key", out, 32,
               "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
        x25519_public(other, theirs);
        x25519(shared, mine, other);
        expect("the shared secret", shared, 32,
               "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
        x25519(out, theirs, out);
        claim("both ends agree", !memcmp(out, shared, 32));
    }

    say("TLS 1.3 key schedule\n");
    {
        boot_uint8_t zeroes[32];
        boot_uint8_t early[32];
        boot_uint8_t empty_hash[32];

        memset(zeroes, 0, sizeof(zeroes));
        hkdf_extract(0, 0, zeroes, 32, early);
        expect("the early secret", early, 32,
               "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
        sha256("", 0, empty_hash);
        hkdf_expand_label(early, "derived", empty_hash, 32, out, 32);
        expect("derived from it (RFC 8448)", out, 32,
               "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");
    }

    say("SHA-384\n");
    {
        boot_uint8_t wide[SHA384_SIZE];

        sha384("abc", 3, wide);
        expect("abc", wide, SHA384_SIZE,
               "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
               "8086072ba1e7cc2358baeca134c825a7");
    }

    say("ECDSA on P-256\n");
    {
        boot_uint8_t hash[32], r[32], s[32], qx[32], qy[32];

        unhex("84537ca4fcb1ed40b6de164f28c294f8c9b05e53f25a9c16475568e862d96d53", hash);
        unhex("6d2721ffffa3d7a489944b455753af22cd3c1bfdb4c5851a8b90ada6bca969e6", r);
        unhex("72b46aa9f8014012b4c2f45d8d108858fd7192b078fb6e3f563de2c892940d5b", s);
        unhex("a8c7357fefa197e46d4483a78452cd5c74c99edcb9f7017acffc9ea53a15e99c", qx);
        unhex("7ae869fa4bd2daf0adc819fc3c3361645beeb23eca1ed9b33df921e546a6c545", qy);
        claim("a signature verifies",
              p256_verify(hash, r, 32, s, 32, qx, qy));
        hash[5] ^= 0x10;
        claim("an altered message is refused",
              !p256_verify(hash, r, 32, s, 32, qx, qy));
        hash[5] ^= 0x10;
        r[9] ^= 0x01;
        claim("an altered signature is refused",
              !p256_verify(hash, r, 32, s, 32, qx, qy));
    }

    say("Certificates\n");
    {
        /* How many roots this machine carries, said out loud: a store that
           quietly emptied itself would otherwise look exactly like a web where
           nothing verifies. */
        say("  ");
        {
            int count = x509_root_count();
            char number[8];
            int at = 0;

            if (!count) { say("[FAIL] no roots are carried at all\n"); failures++; }
            while (count && at < 6) { number[at++] = (char)('0' + count % 10); count /= 10; }
            number[at] = 0;
            for (int index = at - 1; index >= 0; index--) {
                char one[2];

                one[0] = number[index];
                one[1] = 0;
                say(one);
            }
            say(" certificate authorities are carried\n");
        }
        checks++;
    }

    say("Randomness\n");
    {
        boot_uint8_t first[32], second[32];
        int ones = 0;

        random_bytes(first, sizeof(first));
        random_bytes(second, sizeof(second));
        claim("two draws differ", memcmp(first, second, 32) != 0);
        for (int at = 0; at < 32; at++)
            for (int bit = 0; bit < 8; bit++)
                if (first[at] & (1 << bit)) ones++;
        /* Not a test of randomness - nothing this short can be - but it does
           catch a generator that returns all zeroes or all ones, which is the
           failure that actually happens. */
        claim("the bits are not all the same way up", ones > 64 && ones < 192);
        say(random_is_strong()
                      ? "  the processor has RDRAND; keys come from it\n"
                      : "  no RDRAND on this processor; keys come from timing,"
                        " which is weaker\n");
    }

    return failures;
}
