#include "x509.h"
#include "bignum.h"
#include "sha256.h"
#include "p256.h"
#include "string.h"

/* X.509, walked rather than parsed.
 *
 * ---- DER in one paragraph -------------------------------------------------
 *
 * Everything is a tag byte, a length, and that many bytes. A length under 128
 * is the byte itself; otherwise the top bit is set and the low bits say how
 * many bytes of length follow. Structures are tags whose contents are more of
 * the same. That is the entire format, and it is why this file is a few
 * hundred lines rather than a few thousand.
 *
 * ---- The shape of a certificate -------------------------------------------
 *
 *   Certificate ::= SEQUENCE {
 *     tbsCertificate    SEQUENCE { ... everything that is signed ... }
 *     signatureAlgorithm SEQUENCE { OID }
 *     signature          BIT STRING
 *   }
 *
 * and inside tbsCertificate, in this order: an optional version, the serial
 * number, the algorithm again, the issuer, validity, the subject, the public
 * key, and then extensions - where the names it is valid for live.
 */

#define TAG_INTEGER 0x02
#define TAG_BIT_STRING 0x03
#define TAG_OCTET_STRING 0x04
#define TAG_NULL 0x05
#define TAG_OID 0x06
#define TAG_UTF8 0x0C
#define TAG_SEQUENCE 0x30
#define TAG_SET 0x31
#define TAG_PRINTABLE 0x13
#define TAG_IA5 0x16
#define TAG_UTC_TIME 0x17
#define TAG_GENERAL_TIME 0x18

static char trouble[96];

const char* x509_trouble(void) {
    return trouble[0] ? trouble : "nothing went wrong";
}

static void blame(const char* what) {
    boot_uint32_t at = 0;

    while (what[at] && at + 1 < sizeof(trouble)) { trouble[at] = what[at]; at++; }
    trouble[at] = 0;
}

typedef struct {
    const boot_uint8_t* data;
    boot_uint32_t length;
    boot_uint32_t at;
} WALK;

/* One element: its tag, where its contents start and how long they are. The
   walk moves past the whole element. */
static int step(WALK* walk, int* tag, const boot_uint8_t** body,
                boot_uint32_t* length) {
    boot_uint32_t size;

    if (walk->at + 2 > walk->length) return 0;
    *tag = walk->data[walk->at++];
    size = walk->data[walk->at++];

    if (size & 0x80) {
        int count = size & 0x7F;

        if (count > 4 || walk->at + count > walk->length) return 0;
        size = 0;
        while (count--) size = (size << 8) | walk->data[walk->at++];
    }
    if (walk->at + size > walk->length) return 0;
    *body = walk->data + walk->at;
    *length = size;
    walk->at += size;
    return 1;
}

/* A walk over the contents of the element just stepped into. */
static WALK inside(const boot_uint8_t* body, boot_uint32_t length) {
    WALK walk;

    walk.data = body;
    walk.length = length;
    walk.at = 0;
    return walk;
}

static int same_bytes(const boot_uint8_t* a, boot_uint32_t a_length,
                      const boot_uint8_t* b, boot_uint32_t b_length) {
    if (a_length != b_length) return 0;
    return !memcmp(a, b, a_length);
}

/* The object identifiers this needs, as the bytes they are on the wire.
   Written out rather than decoded into numbers: they are compared, never
   printed. */
static const boot_uint8_t oid_rsa_sha256[] =
    { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };
static const boot_uint8_t oid_rsa_sha384[] =
    { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C };
static const boot_uint8_t oid_rsa_pss[] =
    { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0A };
static const boot_uint8_t oid_ecdsa_sha256[] =
    { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const boot_uint8_t oid_ecdsa_sha384[] =
    { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03 };
static const boot_uint8_t oid_ec_key[] =
    { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const boot_uint8_t oid_p256[] =
    { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const boot_uint8_t oid_p384[] = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
static const boot_uint8_t oid_rsa_key[] =
    { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const boot_uint8_t oid_common_name[] = { 0x55, 0x04, 0x03 };
static const boot_uint8_t oid_basic_constraints[] = { 0x55, 0x1D, 0x13 };
static const boot_uint8_t oid_subject_alt_name[] = { 0x55, 0x1D, 0x11 };

static int algorithm_of(const boot_uint8_t* oid, boot_uint32_t length) {
    if (same_bytes(oid, length, oid_rsa_sha256, sizeof(oid_rsa_sha256)))
        return X509_RSA_PKCS1_SHA256;
    if (same_bytes(oid, length, oid_rsa_sha384, sizeof(oid_rsa_sha384)))
        return X509_RSA_PKCS1_SHA384;
    if (same_bytes(oid, length, oid_rsa_pss, sizeof(oid_rsa_pss)))
        return X509_RSA_PSS_SHA256;
    if (same_bytes(oid, length, oid_ecdsa_sha256, sizeof(oid_ecdsa_sha256)))
        return X509_ECDSA_SHA256;
    if (same_bytes(oid, length, oid_ecdsa_sha384, sizeof(oid_ecdsa_sha384)))
        return X509_ECDSA_SHA384;
    return X509_UNKNOWN;
}

/* Times come as either two or four digits of year. Both are turned into
   YYYYMMDDHHMMSS so that comparing two of them is comparing two strings -
   which is the whole reason for the format. */
static void read_time(int tag, const boot_uint8_t* body, boot_uint32_t length,
                      char* out) {
    boot_uint32_t at = 0;
    int written = 0;

    if (tag == TAG_UTC_TIME && length >= 12) {
        int year = (body[0] - '0') * 10 + (body[1] - '0');

        /* Two digits, and the standard's rule: 50 and above is the twentieth
           century. Nothing issued now is, but a root from 1998 is. */
        out[written++] = year >= 50 ? '1' : '2';
        out[written++] = year >= 50 ? '9' : '0';
        out[written++] = body[0];
        out[written++] = body[1];
        at = 2;
    } else if (length >= 14) {
        for (; at < 4; at++) out[written++] = (char)body[at];
    }
    while (at < length && written < 14 && body[at] >= '0' && body[at] <= '9')
        out[written++] = (char)body[at++];
    while (written < 14) out[written++] = '0';
    out[14] = 0;
}

int x509_parse(const boot_uint8_t* der, boot_uint32_t length,
               CERTIFICATE* out) {
    WALK top = inside(der, length);
    WALK certificate;
    WALK fields;
    int tag;
    const boot_uint8_t* body;
    boot_uint32_t size;

    memset(out, 0, sizeof(CERTIFICATE));
    trouble[0] = 0;

    if (!step(&top, &tag, &body, &size) || tag != TAG_SEQUENCE) {
        blame("that is not a certificate");
        return 0;
    }
    certificate = inside(body, size);

    /* tbsCertificate: kept whole, because it is what the signature covers -
       every byte of it, exactly as it arrived. */
    {
        boot_uint32_t before = certificate.at;

        if (!step(&certificate, &tag, &body, &size) || tag != TAG_SEQUENCE) {
            blame("the certificate has no body");
            return 0;
        }
        out->tbs = certificate.data + before;
        out->tbs_length = certificate.at - before;
        fields = inside(body, size);
    }

    /* The algorithm, then the signature. */
    if (!step(&certificate, &tag, &body, &size) || tag != TAG_SEQUENCE) {
        blame("the certificate has no signature algorithm");
        return 0;
    }
    {
        WALK algorithm = inside(body, size);
        const boot_uint8_t* oid;
        boot_uint32_t oid_length;

        if (step(&algorithm, &tag, &oid, &oid_length) && tag == TAG_OID)
            out->algorithm = algorithm_of(oid, oid_length);
    }
    if (!step(&certificate, &tag, &body, &size) || tag != TAG_BIT_STRING) {
        blame("the certificate has no signature");
        return 0;
    }
    /* A bit string starts with how many bits of the last byte are unused, and
       for a signature it is always zero. */
    out->signature = body + 1;
    out->signature_length = size - 1;

    /* ---- inside the body ---- */
    if (!step(&fields, &tag, &body, &size)) { blame("the certificate body is empty"); return 0; }
    if (tag == 0xA0) {                          /* the version, when present */
        if (!step(&fields, &tag, &body, &size)) { blame("the certificate body is short"); return 0; }
    }
    /* body is now the serial number; skip the algorithm that follows. */
    if (!step(&fields, &tag, &body, &size)) { blame("the certificate body is short"); return 0; }

    /* issuer */
    {
        boot_uint32_t before = fields.at;

        if (!step(&fields, &tag, &body, &size) || tag != TAG_SEQUENCE) {
            blame("the certificate has no issuer");
            return 0;
        }
        out->issuer = fields.data + before;
        out->issuer_length = fields.at - before;
    }

    /* validity */
    if (!step(&fields, &tag, &body, &size) || tag != TAG_SEQUENCE) {
        blame("the certificate has no validity");
        return 0;
    }
    {
        WALK validity = inside(body, size);
        const boot_uint8_t* when;
        boot_uint32_t when_length;

        if (step(&validity, &tag, &when, &when_length))
            read_time(tag, when, when_length, out->not_before);
        if (step(&validity, &tag, &when, &when_length))
            read_time(tag, when, when_length, out->not_after);
    }

    /* subject, kept raw so a child's issuer can be compared against it */
    {
        boot_uint32_t before = fields.at;

        if (!step(&fields, &tag, &body, &size) || tag != TAG_SEQUENCE) {
            blame("the certificate has no subject");
            return 0;
        }
        out->subject = fields.data + before;
        out->subject_length = fields.at - before;

        /* The common name, for the case of a certificate with no
           subjectAltName - which the rules stopped allowing years ago and
           which old servers still send. */
        {
            WALK names = inside(body, size);
            int name_tag;
            const boot_uint8_t* set_body;
            boot_uint32_t set_length;

            while (step(&names, &name_tag, &set_body, &set_length)) {
                WALK pair = inside(set_body, set_length);
                const boot_uint8_t* item;
                boot_uint32_t item_length;

                if (!step(&pair, &name_tag, &item, &item_length)) continue;
                {
                    WALK attribute = inside(item, item_length);
                    const boot_uint8_t* oid;
                    boot_uint32_t oid_length;
                    const boot_uint8_t* value;
                    boot_uint32_t value_length;
                    int value_tag;

                    if (!step(&attribute, &name_tag, &oid, &oid_length)) continue;
                    if (!same_bytes(oid, oid_length, oid_common_name,
                                    sizeof(oid_common_name))) continue;
                    if (!step(&attribute, &value_tag, &value, &value_length))
                        continue;
                    {
                        boot_uint32_t take = value_length < X509_NAME_MAX - 1
                                             ? value_length : X509_NAME_MAX - 1;

                        memcpy(out->common_name, value, take);
                        out->common_name[take] = 0;
                    }
                }
            }
        }
    }

    /* subjectPublicKeyInfo */
    if (!step(&fields, &tag, &body, &size) || tag != TAG_SEQUENCE) {
        blame("the certificate has no public key");
        return 0;
    }
    {
        WALK key = inside(body, size);
        const boot_uint8_t* algorithm_body;
        boot_uint32_t algorithm_length;
        int is_rsa = 0;
        int is_p256 = 0;

        if (step(&key, &tag, &algorithm_body, &algorithm_length) &&
            tag == TAG_SEQUENCE) {
            WALK algorithm = inside(algorithm_body, algorithm_length);
            const boot_uint8_t* oid;
            boot_uint32_t oid_length;

            if (step(&algorithm, &tag, &oid, &oid_length) && tag == TAG_OID) {
                is_rsa = same_bytes(oid, oid_length, oid_rsa_key,
                                    sizeof(oid_rsa_key));
                if (same_bytes(oid, oid_length, oid_ec_key,
                               sizeof(oid_ec_key))) {
                    /* Which curve is the next thing in the algorithm. Both
                       the curves this reads are here, because chains mix
                       them: a leaf on P-256 under an intermediate on P-384 is
                       an ordinary arrangement. */
                    if (step(&algorithm, &tag, &oid, &oid_length) &&
                        tag == TAG_OID) {
                        if (same_bytes(oid, oid_length, oid_p256,
                                       sizeof(oid_p256))) is_p256 = 32;
                        else if (same_bytes(oid, oid_length, oid_p384,
                                            sizeof(oid_p384))) is_p256 = 48;
                    }
                }
            }
        }
        if (is_p256 && step(&key, &tag, &body, &size) &&
            tag == TAG_BIT_STRING) {
            /* An uncompressed point: 0x04, then the two coordinates. The
               compressed forms are legal and nobody issues them. */
            if (size >= 2 + (boot_uint32_t)is_p256 * 2 && body[1] == 0x04) {
                out->curve_x = body + 2;
                out->curve_y = body + 2 + is_p256;
                out->curve_size = (boot_uint32_t)is_p256;
            }
        }
        if (is_rsa && step(&key, &tag, &body, &size) && tag == TAG_BIT_STRING) {
            WALK numbers = inside(body + 1, size - 1);

            if (step(&numbers, &tag, &body, &size) && tag == TAG_SEQUENCE) {
                WALK pair = inside(body, size);
                const boot_uint8_t* value;
                boot_uint32_t value_length;

                if (step(&pair, &tag, &value, &value_length) &&
                    tag == TAG_INTEGER) {
                    out->modulus = value;
                    out->modulus_length = value_length;
                }
                if (step(&pair, &tag, &value, &value_length) &&
                    tag == TAG_INTEGER) {
                    boot_uint32_t exponent = 0;

                    for (boot_uint32_t at = 0; at < value_length && at < 4; at++)
                        exponent = (exponent << 8) | value[at];
                    out->exponent = exponent;
                }
            }
        }
    }

    /* extensions, for what it is allowed to be and what names it covers */
    while (step(&fields, &tag, &body, &size)) {
        WALK extensions;

        if (tag != 0xA3) continue;
        extensions = inside(body, size);
        if (!step(&extensions, &tag, &body, &size) || tag != TAG_SEQUENCE)
            break;
        extensions = inside(body, size);
        while (step(&extensions, &tag, &body, &size)) {
            WALK extension = inside(body, size);
            const boot_uint8_t* oid;
            boot_uint32_t oid_length;
            const boot_uint8_t* value;
            boot_uint32_t value_length;
            int inner_tag;

            if (!step(&extension, &inner_tag, &oid, &oid_length)) continue;
            if (!step(&extension, &inner_tag, &value, &value_length)) continue;
            if (inner_tag == 0x01)      /* the "critical" flag, when present */
                if (!step(&extension, &inner_tag, &value, &value_length))
                    continue;

            if (same_bytes(oid, oid_length, oid_basic_constraints,
                           sizeof(oid_basic_constraints))) {
                WALK constraints = inside(value, value_length);

                if (step(&constraints, &inner_tag, &value, &value_length) &&
                    inner_tag == TAG_SEQUENCE) {
                    WALK body_walk = inside(value, value_length);

                    if (step(&body_walk, &inner_tag, &value, &value_length) &&
                        inner_tag == 0x01 && value_length && value[0])
                        out->is_authority = 1;
                }
            } else if (same_bytes(oid, oid_length, oid_subject_alt_name,
                                  sizeof(oid_subject_alt_name))) {
                WALK names = inside(value, value_length);

                if (step(&names, &inner_tag, &value, &value_length) &&
                    inner_tag == TAG_SEQUENCE) {
                    out->names = value;
                    out->names_length = value_length;
                }
            }
        }
    }

    if (!out->tbs_length || !out->signature_length) {
        blame("the certificate is missing the parts that make it one");
        return 0;
    }
    return 1;
}

/* ---- Signatures ---------------------------------------------------------- */

/* PKCS#1 v1.5: the hash with a fixed prefix, padded with 0xFF up to the size
   of the modulus. Checked by rebuilding what it should be and comparing - the
   only safe way, because taking it apart invites accepting a signature with
   the padding wrong. */
static int check_pkcs1(const boot_uint8_t* recovered, boot_uint32_t length,
                       const boot_uint8_t* hash, boot_uint32_t hash_length) {
    static const boot_uint8_t prefix_256[] = {
        0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
        0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    static const boot_uint8_t prefix_384[] = {
        0x30, 0x41, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
        0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30
    };
    const boot_uint8_t* prefix = hash_length == 48 ? prefix_384 : prefix_256;
    boot_uint32_t prefix_length = 19;
    boot_uint8_t expected[512];
    boot_uint32_t at = 0;
    boot_uint32_t padding;

    if (length < prefix_length + hash_length + 11) return 0;
    padding = length - prefix_length - hash_length - 3;

    expected[at++] = 0x00;
    expected[at++] = 0x01;
    while (padding--) expected[at++] = 0xFF;
    expected[at++] = 0x00;
    memcpy(expected + at, prefix, prefix_length);
    at += prefix_length;
    memcpy(expected + at, hash, hash_length);
    at += hash_length;

    if (at != length) return 0;
    return !memcmp(expected, recovered, length);
}

/* PSS, which is what TLS 1.3 asks for: the recovered block holds a masked
 * salt and a hash of (eight zeroes, the message hash, the salt). The mask is
 * generated from that hash by MGF1.
 *
 * Longer than v1.5 and not harder: every step below is written in the order
 * RFC 8017 gives it. */
static void mgf1(const boot_uint8_t* seed, boot_uint32_t seed_length,
                 boot_uint8_t* out, boot_uint32_t length) {
    boot_uint32_t done = 0;
    boot_uint32_t counter = 0;

    while (done < length) {
        SHA256 hash;
        boot_uint8_t block[32];
        boot_uint8_t number[4];
        boot_uint32_t take = length - done < 32 ? length - done : 32;

        number[0] = (boot_uint8_t)(counter >> 24);
        number[1] = (boot_uint8_t)(counter >> 16);
        number[2] = (boot_uint8_t)(counter >> 8);
        number[3] = (boot_uint8_t)counter;

        sha256_start(&hash);
        sha256_add(&hash, seed, seed_length);
        sha256_add(&hash, number, 4);
        sha256_finish(&hash, block);
        memcpy(out + done, block, take);
        done += take;
        counter++;
    }
}

static int check_pss(const boot_uint8_t* recovered, boot_uint32_t length,
                     const boot_uint8_t hash[32]) {
    boot_uint8_t masked[512];
    boot_uint8_t mask[512];
    const boot_uint8_t* signed_hash;
    boot_uint32_t db_length;
    boot_uint32_t at;
    boot_uint32_t salt_length;
    const boot_uint8_t* salt;

    if (length < 32 + 2 || length > sizeof(masked)) return 0;
    if (recovered[length - 1] != 0xBC) return 0;

    db_length = length - 32 - 1;
    signed_hash = recovered + db_length;

    mgf1(signed_hash, 32, mask, db_length);
    for (at = 0; at < db_length; at++) masked[at] = recovered[at] ^ mask[at];
    /* The leftmost bits are cleared because the number must be smaller than
       the modulus; the standard says to clear the same ones here. */
    masked[0] &= 0x7F;

    at = 0;
    while (at < db_length && !masked[at]) at++;
    if (at >= db_length || masked[at] != 0x01) return 0;
    at++;
    salt = masked + at;
    salt_length = db_length - at;

    {
        SHA256 rebuild;
        boot_uint8_t zeroes[8];
        boot_uint8_t expected[32];

        memset(zeroes, 0, sizeof(zeroes));
        sha256_start(&rebuild);
        sha256_add(&rebuild, zeroes, sizeof(zeroes));
        sha256_add(&rebuild, hash, 32);
        sha256_add(&rebuild, salt, salt_length);
        sha256_finish(&rebuild, expected);
        return !memcmp(expected, signed_hash, 32);
    }
}

int x509_rsa_verify_wide(const boot_uint8_t* modulus,
                         boot_uint32_t modulus_length,
                         boot_uint32_t exponent, int algorithm,
                         const boot_uint8_t* hash, boot_uint32_t hash_length,
                         const boot_uint8_t* signature,
                         boot_uint32_t signature_length) {
    BIGNUM number;
    BIGNUM key;
    BIGNUM result;
    boot_uint8_t recovered[BIGNUM_MAX_BYTES];
    boot_uint32_t size;

    trouble[0] = 0;
    if (!modulus || !modulus_length) { blame("that certificate has no RSA key"); return 0; }
    if (!bignum_from_bytes(&key, modulus, modulus_length)) { blame("the key is larger than this can hold"); return 0; }
    if (!bignum_from_bytes(&number, signature, signature_length)) { blame("the signature is larger than this can hold"); return 0; }
    if (!bignum_modexp(&result, &number, exponent ? exponent : 65537, &key)) {
        blame("the signature could not be worked out");
        return 0;
    }

    /* The recovered block is as wide as the modulus, leading zero and all. */
    size = modulus_length;
    while (size > 1 && !modulus[modulus_length - size]) size--;
    if (size > sizeof(recovered)) { blame("the key is larger than this can hold"); return 0; }
    bignum_to_bytes(&result, recovered, size);

    if (algorithm == X509_RSA_PSS_SHA256)
        return check_pss(recovered, size, hash) ? 1
               : (blame("the signature does not match"), 0);
    if (algorithm == X509_RSA_PKCS1_SHA256 ||
        algorithm == X509_RSA_PKCS1_SHA384)
        return check_pkcs1(recovered, size, hash, hash_length) ? 1
               : (blame("the signature does not match"), 0);

    blame("that signature is of a kind this cannot check yet");
    return 0;
}

int x509_rsa_verify(const boot_uint8_t* modulus, boot_uint32_t modulus_length,
                    boot_uint32_t exponent, int algorithm,
                    const boot_uint8_t hash[32],
                    const boot_uint8_t* signature,
                    boot_uint32_t signature_length) {
    return x509_rsa_verify_wide(modulus, modulus_length, exponent, algorithm,
                                hash, 32, signature, signature_length);
}

/* An ECDSA signature is a DER pair of integers rather than one number, so it
   has to be taken apart before the curve sees it. */
static int ecdsa_check(const CERTIFICATE* parent, const boot_uint8_t* hash,
                       boot_uint32_t hash_length,
                       const boot_uint8_t* signature,
                       boot_uint32_t signature_length) {
    WALK outer = inside(signature, signature_length);
    WALK pair;
    int tag;
    const boot_uint8_t* body;
    boot_uint32_t size;
    const boot_uint8_t* r;
    boot_uint32_t r_length;

    if (!parent->curve_x || !parent->curve_y) {
        blame("that certificate has no P-256 key");
        return 0;
    }
    if (!step(&outer, &tag, &body, &size) || tag != TAG_SEQUENCE) {
        blame("the signature is not a pair of numbers");
        return 0;
    }
    pair = inside(body, size);
    if (!step(&pair, &tag, &r, &r_length) || tag != TAG_INTEGER) {
        blame("the signature is not a pair of numbers");
        return 0;
    }
    if (!step(&pair, &tag, &body, &size) || tag != TAG_INTEGER) {
        blame("the signature is not a pair of numbers");
        return 0;
    }
    if (!p256_verify_on(hash, hash_length, r, r_length, body, size,
                        parent->curve_x, parent->curve_y,
                        parent->curve_size)) {
        blame("the signature does not match");
        return 0;
    }
    return 1;
}

int x509_signed_by(const CERTIFICATE* child, const CERTIFICATE* parent) {
    boot_uint8_t hash[48];
    boot_uint32_t hash_length = 32;

    if (child->algorithm == X509_ECDSA_SHA256 ||
        child->algorithm == X509_ECDSA_SHA384) {
        /* P-256 takes a 32-byte number: a longer hash is used from its left,
           which is what the standard says and what everybody does. */
        if (child->algorithm == X509_ECDSA_SHA384) {
            sha384(child->tbs, child->tbs_length, hash);
            hash_length = 48;
        } else {
            sha256(child->tbs, child->tbs_length, hash);
        }
        return ecdsa_check(parent, hash, hash_length, child->signature,
                           child->signature_length);
    }
    if (child->algorithm == X509_UNKNOWN) {
        blame("this chain is signed in a way this does not know");
        return 0;
    }
    if (child->algorithm == X509_RSA_PKCS1_SHA384) {
        sha384(child->tbs, child->tbs_length, hash);
        hash_length = 48;
    } else {
        sha256(child->tbs, child->tbs_length, hash);
    }
    return x509_rsa_verify_wide(parent->modulus, parent->modulus_length,
                                parent->exponent, child->algorithm, hash,
                                hash_length, child->signature,
                                child->signature_length);
}

/* ---- Names --------------------------------------------------------------- */

static int same_name(const char* a, boot_uint32_t a_length, const char* b) {
    boot_uint32_t at = 0;

    while (at < a_length && b[at]) {
        char left = a[at];
        char right = b[at];

        if (left >= 'A' && left <= 'Z') left = (char)(left + 32);
        if (right >= 'A' && right <= 'Z') right = (char)(right + 32);
        if (left != right) return 0;
        at++;
    }
    return at == a_length && !b[at];
}

/* `*.example.com` matches one label and not a dot inside it: a wildcard that
   matched `a.b.example.com` would let a certificate for one subdomain answer
   for a different one. */
static int wildcard_matches(const char* pattern, boot_uint32_t pattern_length,
                            const char* host) {
    boot_uint32_t at = 0;

    if (pattern_length < 2 || pattern[0] != '*' || pattern[1] != '.') return 0;
    while (host[at] && host[at] != '.') at++;
    if (!host[at]) return 0;
    return same_name(pattern + 1, pattern_length - 1, host + at);
}

int x509_name_matches(const CERTIFICATE* self, const char* host) {
    if (self->names && self->names_length) {
        WALK names = inside(self->names, self->names_length);
        int tag;
        const boot_uint8_t* body;
        boot_uint32_t size;

        while (step(&names, &tag, &body, &size)) {
            /* Context tag 2 is a DNS name; the others are addresses and
               electronic mail, which a browser does not use. */
            if (tag != 0x82) continue;
            if (same_name((const char*)body, size, host)) return 1;
            if (wildcard_matches((const char*)body, size, host)) return 1;
        }
        /* A certificate with names listed is answered by that list alone -
           the common name is not consulted, which is the rule since 2011 and
           the reason a wildcard in the wrong field cannot be smuggled in. */
        return 0;
    }

    {
        boot_uint32_t length = 0;

        while (self->common_name[length]) length++;
        if (same_name(self->common_name, length, host)) return 1;
        return wildcard_matches(self->common_name, length, host);
    }
}

int x509_valid_at(const CERTIFICATE* self, const char* when) {
    if (!self->not_before[0] || !self->not_after[0]) return 0;
    for (int at = 0; at < 14; at++) {
        if (when[at] < self->not_before[at]) return 0;
        if (when[at] > self->not_before[at]) break;
    }
    for (int at = 0; at < 14; at++) {
        if (when[at] > self->not_after[at]) return 0;
        if (when[at] < self->not_after[at]) break;
    }
    return 1;
}

/* The same check with the curve and the hash length said out loud, for TLS
   1.2 - where the server signs its key exchange, and may do it on P-384. */
int x509_ecdsa_verify_key(const CERTIFICATE* key, const boot_uint8_t* hash,
                          boot_uint32_t hash_length,
                          const boot_uint8_t* signature,
                          boot_uint32_t signature_length) {
    return ecdsa_check(key, hash, hash_length, signature, signature_length);
}

int x509_ecdsa_verify(const boot_uint8_t* key_x, const boot_uint8_t* key_y,
                      const boot_uint8_t hash[32],
                      const boot_uint8_t* signature,
                      boot_uint32_t signature_length) {
    CERTIFICATE holder;

    memset(&holder, 0, sizeof(holder));
    holder.curve_x = key_x;
    holder.curve_y = key_y;
    holder.curve_size = 32;      /* TLS 1.3 signs with P-256 here */
    return ecdsa_check(&holder, hash, 32, signature, signature_length);
}
