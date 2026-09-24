#include "tls.h"
#include "tcp.h"
#include "sha256.h"
#include "aead.h"
#include "x25519.h"
#include "p256.h"
#include "random.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "task.h"
#include "x509.h"
#include "rtc.h"

/* TLS 1.3.
 *
 * ---- The shape of a handshake, because the code follows it ---------------
 *
 *   we send    ClientHello           a random, a key share, what we support
 *   they send  ServerHello           their key share, the suite they chose
 *   ---- from here everything is encrypted ----
 *   they send  EncryptedExtensions
 *              Certificate           who they claim to be
 *              CertificateVerify     a signature proving they hold its key
 *              Finished              a code over everything said so far
 *   we send    Finished              the same, from our side
 *   ---- from here the keys change again, and it is a pipe ----
 *
 * Two key exchanges' worth of secrets come out of one shared secret, through
 * a schedule of HKDF steps: handshake keys to read the messages above,
 * application keys for everything after. The transcript - a running SHA-256
 * of every handshake message in order - is mixed into each step, which is what
 * ties the keys to *this* conversation and makes a replayed or altered
 * message produce keys that do not work.
 *
 * ---- Why only one cipher suite -------------------------------------------
 *
 * TLS_CHACHA20_POLY1305_SHA256. It is the one whose parts were already
 * written and checked against the RFC's vectors, it needs no AES
 * instructions and no cache-timing-safe tables, and every server that speaks
 * TLS 1.3 supports it - it is one of the three the standard requires. A
 * second suite is a second set of bugs and buys nothing here.
 *
 * ---- What is deliberately not here yet -----------------------------------
 *
 * The certificate is received, its length checked and its bytes skipped. It is
 * not parsed and nothing is verified, so this connection is private and
 * unauthenticated: encrypted against somebody watching the wire, worth nothing
 * against somebody who can answer in the server's place. Everything above is
 * told that through `tls_identity_checked`, and Nami says it in words.
 */

#define RECORD_HEADER 5
#define RECORD_MAX 16640            /* 2^14 plus the room a tag needs */
#define HASH_SIZE SHA256_SIZE

#define RECORD_CHANGE_CIPHER 20
#define RECORD_ALERT 21
#define RECORD_HANDSHAKE 22
#define RECORD_APPLICATION 23

#define HANDSHAKE_CLIENT_HELLO 1
#define HANDSHAKE_SERVER_HELLO 2
#define HANDSHAKE_NEW_TICKET 4
#define HANDSHAKE_ENCRYPTED_EXTENSIONS 8
#define HANDSHAKE_CERTIFICATE 11
#define HANDSHAKE_SERVER_KEY_EXCHANGE 12        /* 1.2 only */
#define HANDSHAKE_CERTIFICATE_REQUEST 13
#define HANDSHAKE_SERVER_HELLO_DONE 14          /* 1.2 only */
#define HANDSHAKE_CLIENT_KEY_EXCHANGE 16        /* 1.2 only */
#define HANDSHAKE_CERTIFICATE_VERIFY 15
#define HANDSHAKE_FINISHED 20
#define HANDSHAKE_KEY_UPDATE 24

typedef struct {
    int used;
    int tcp;
    int open;
    int identity_checked;

    /* Which protocol this connection turned out to be. 1.3 and 1.2 share the
       connection, the transcript and the cipher, and differ in nearly
       everything else - so the version is carried rather than assumed. */
    int version;                    /* 0x0304 or 0x0303 */
    boot_uint8_t client_random[32];
    boot_uint8_t server_random[32];
    boot_uint8_t master[48];

    SHA256 transcript;

    boot_uint8_t client_secret[HASH_SIZE];
    boot_uint8_t server_secret[HASH_SIZE];
    boot_uint8_t client_key[AEAD_KEY_SIZE];
    boot_uint8_t server_key[AEAD_KEY_SIZE];
    boot_uint8_t client_iv[AEAD_NONCE_SIZE];
    boot_uint8_t server_iv[AEAD_NONCE_SIZE];
    boot_uint64_t client_sequence;
    boot_uint64_t server_sequence;
    int encrypted;                  /* whether what we send is protected yet */
    int reading_encrypted;          /* and whether what arrives is.

       Two flags rather than one because TLS 1.2 turns the two directions on
       separately: our ChangeCipherSpec protects everything after it that we
       send, and theirs arrives later and protects theirs. In 1.3 both happen
       at the same instant and both are set together. */

    /* Plain application bytes already decrypted and not yet taken. */
    boot_uint8_t plain[RECORD_MAX];
    boot_uint32_t plain_length;
    boot_uint32_t plain_taken;

    /* One record being collected off the wire. */
    boot_uint8_t record[RECORD_MAX + RECORD_HEADER];
    boot_uint32_t record_have;

    /* Handshake messages, joined.
     *
     * A record is not a message. One record may hold several messages, and one
     * message - a certificate chain, always - is routinely spread across
     * several records. Parsing messages inside a record works perfectly on a
     * small site with a short chain and fails on every large one, which is the
     * worst possible distribution of a bug. So handshake bytes are collected
     * here and taken out a whole message at a time. */
    boot_uint8_t pending[RECORD_MAX * 2];
    boot_uint32_t pending_length;

    /* The chain as it arrived, and the leaf taken apart. Kept because the
       signature in CertificateVerify is made with the leaf's key and arrives
       one message later. */
    boot_uint8_t chain[RECORD_MAX];
    boot_uint32_t chain_length;
    CERTIFICATE leaf;
    int have_leaf;
    boot_uint8_t before_verify[HASH_SIZE];   /* transcript up to Certificate */
} SESSION;

static SESSION sessions[TLS_MAX_SESSIONS];
static char trouble[128];

const char* tls_trouble(void) {
    return trouble[0] ? trouble : "nothing went wrong";
}

static void blame(const char* what) {
    boot_uint32_t at = 0;

    while (what[at] && at + 1 < sizeof(trouble)) { trouble[at] = what[at]; at++; }
    trouble[at] = 0;
}

static void put_be16(boot_uint8_t* at, boot_uint32_t value) {
    at[0] = (boot_uint8_t)(value >> 8);
    at[1] = (boot_uint8_t)value;
}

static boot_uint32_t get_be16(const boot_uint8_t* at) {
    return ((boot_uint32_t)at[0] << 8) | at[1];
}

static boot_uint32_t get_be24(const boot_uint8_t* at) {
    return ((boot_uint32_t)at[0] << 16) | ((boot_uint32_t)at[1] << 8) | at[2];
}

/* ---- The older way of making keys ----------------------------------------
 *
 * TLS 1.2 has no HKDF. It has this: a secret and a seed stretched to any
 * length by chaining HMAC, defined in RFC 5246 and called the PRF. Everything
 * in a 1.2 connection comes out of it - the master secret, the keys, the proof
 * at the end of the handshake - so it is the first thing that has to be right,
 * and the last thing whose mistakes are visible.
 *
 * A(0) is the seed; A(i) is HMAC of A(i-1). The output is HMAC of A(i) with
 * the seed appended, taken as many times as needed. That is the whole of it.
 */
static void tls12_prf(const boot_uint8_t* secret, boot_uint32_t secret_length,
                      const char* label,
                      const boot_uint8_t* seed, boot_uint32_t seed_length,
                      boot_uint8_t* out, boot_uint32_t length) {
    boot_uint8_t a[HASH_SIZE];
    boot_uint8_t input[HASH_SIZE + 128];
    boot_uint32_t label_length = 0;
    boot_uint32_t written = 0;
    boot_uint32_t tail;

    while (label[label_length]) label_length++;
    if (label_length + seed_length > 128) return;

    /* A(1) = HMAC(secret, label + seed), and the label is part of the seed
       everywhere below. */
    memcpy(input, label, label_length);
    memcpy(input + label_length, seed, seed_length);
    tail = label_length + seed_length;
    hmac_sha256(secret, secret_length, input, tail, a);

    while (written < length) {
        boot_uint8_t block[HASH_SIZE];
        boot_uint8_t joined[HASH_SIZE + 128];
        boot_uint32_t take;

        memcpy(joined, a, HASH_SIZE);
        memcpy(joined + HASH_SIZE, input, tail);
        hmac_sha256(secret, secret_length, joined, HASH_SIZE + tail, block);

        take = length - written < HASH_SIZE ? length - written : HASH_SIZE;
        memcpy(out + written, block, take);
        written += take;

        hmac_sha256(secret, secret_length, a, HASH_SIZE, a);
    }
}

/* ---- The key schedule ----------------------------------------------------
 *
 * RFC 8446 section 7.1, written out in the order it happens. Every secret is
 * derived from the one before it and from the transcript so far, which is
 * what makes the keys belong to this conversation and no other.
 */

static void transcript_hash(SESSION* self, boot_uint8_t out[HASH_SIZE]) {
    SHA256 copy = self->transcript;   /* the running hash, without ending it */

    sha256_finish(&copy, out);
}

static int derive_secret(const boot_uint8_t secret[HASH_SIZE],
                         const char* label,
                         const boot_uint8_t context[HASH_SIZE],
                         boot_uint8_t out[HASH_SIZE]) {
    return hkdf_expand_label(secret, label, context, HASH_SIZE, out, HASH_SIZE);
}

/* A traffic secret becomes a key and a nonce base. */
static int traffic_keys(const boot_uint8_t secret[HASH_SIZE],
                        boot_uint8_t key[AEAD_KEY_SIZE],
                        boot_uint8_t iv[AEAD_NONCE_SIZE]) {
    if (!hkdf_expand_label(secret, "key", (const boot_uint8_t*)0, 0, key,
                           AEAD_KEY_SIZE)) return 0;
    return hkdf_expand_label(secret, "iv", (const boot_uint8_t*)0, 0, iv,
                             AEAD_NONCE_SIZE);
}

/* The nonce for a record: the base with the sequence number counted into its
   last eight bytes. Not a counter of its own - the standard says exclusive-or
   with the base, so that two connections with the same sequence numbers do
   not share a nonce. */
static void record_nonce(const boot_uint8_t base[AEAD_NONCE_SIZE],
                         boot_uint64_t sequence,
                         boot_uint8_t out[AEAD_NONCE_SIZE]) {
    for (int at = 0; at < AEAD_NONCE_SIZE; at++) out[at] = base[at];
    for (int at = 0; at < 8; at++)
        out[AEAD_NONCE_SIZE - 1 - at] ^=
            (boot_uint8_t)((sequence >> (at * 8)) & 0xFF);
}

/* ---- Records ------------------------------------------------------------- */

static int send_all(SESSION* self, const boot_uint8_t* data,
                    boot_uint32_t length, boot_uint32_t timeout) {
    boot_uint32_t sent = 0;

    while (sent < length) {
        int step = tcp_send(self->tcp, data + sent, length - sent, timeout);

        if (step <= 0) return 0;
        sent += (boot_uint32_t)step;
    }
    return 1;
}

/* One record out. Before the keys exist it goes as it is; after, the whole
   thing including its real content type is encrypted and the outside says
   "application data" - which is what TLS 1.3 does so that a watcher cannot
   tell a handshake message from a page. */
static int send_record(SESSION* self, int type, const boot_uint8_t* body,
                       boot_uint32_t length, boot_uint32_t timeout) {
    boot_uint8_t out[RECORD_MAX + RECORD_HEADER];

    if (length + AEAD_TAG_SIZE + 1 > RECORD_MAX) {
        blame("a record longer than this can send");
        return 0;
    }

    if (!self->encrypted) {
        out[0] = (boot_uint8_t)type;
        out[1] = 3;
        out[2] = 3;                 /* "TLS 1.2" on the outside, always */
        put_be16(out + 3, length);
        memcpy(out + RECORD_HEADER, body, length);
        return send_all(self, out, RECORD_HEADER + length, timeout);
    }

    if (self->version == 0x0303) {
        /* TLS 1.2 keeps the real type in the header and authenticates it
           alongside the sequence number, rather than hiding it inside the
           encrypted part the way 1.3 does. */
        boot_uint8_t nonce[AEAD_NONCE_SIZE];
        boot_uint8_t tag[AEAD_TAG_SIZE];
        boot_uint8_t extra[13];

        memcpy(out + RECORD_HEADER, body, length);
        out[0] = (boot_uint8_t)type;
        out[1] = 3;
        out[2] = 3;
        put_be16(out + 3, length + AEAD_TAG_SIZE);

        for (int at = 0; at < 8; at++)
            extra[at] = (boot_uint8_t)((self->client_sequence >>
                                        ((7 - at) * 8)) & 0xFF);
        extra[8] = (boot_uint8_t)type;
        extra[9] = 3;
        extra[10] = 3;
        put_be16(extra + 11, length);

        record_nonce(self->client_iv, self->client_sequence, nonce);
        aead_seal(self->client_key, nonce, extra, sizeof(extra),
                  out + RECORD_HEADER, length, tag);
        memcpy(out + RECORD_HEADER + length, tag, AEAD_TAG_SIZE);
        self->client_sequence++;
        return send_all(self, out, RECORD_HEADER + length + AEAD_TAG_SIZE,
                        timeout);
    }

    {
        boot_uint8_t nonce[AEAD_NONCE_SIZE];
        boot_uint8_t tag[AEAD_TAG_SIZE];
        boot_uint32_t inner = length + 1;

        memcpy(out + RECORD_HEADER, body, length);
        out[RECORD_HEADER + length] = (boot_uint8_t)type;   /* the real type */

        out[0] = RECORD_APPLICATION;
        out[1] = 3;
        out[2] = 3;
        put_be16(out + 3, inner + AEAD_TAG_SIZE);

        record_nonce(self->client_iv, self->client_sequence, nonce);
        aead_seal(self->client_key, nonce, out, RECORD_HEADER,
                  out + RECORD_HEADER, inner, tag);
        memcpy(out + RECORD_HEADER + inner, tag, AEAD_TAG_SIZE);
        self->client_sequence++;
        return send_all(self, out, RECORD_HEADER + inner + AEAD_TAG_SIZE,
                        timeout);
    }
}

/* One record in, decrypted if it needs to be. `type` comes back as the real
   content type. Returns the body length, 0 when the connection ended, -1 on
   failure. */
static int read_record(SESSION* self, int* type, boot_uint8_t** body,
                       boot_uint32_t timeout) {
    boot_uint32_t length;

    self->record_have = 0;
    while (self->record_have < RECORD_HEADER) {
        int got = tcp_receive(self->tcp, self->record + self->record_have,
                              RECORD_HEADER - self->record_have, timeout);

        if (got == 0) return 0;
        if (got < 0) { blame("the connection ended in the middle of a record"); return -1; }
        self->record_have += (boot_uint32_t)got;
    }

    length = get_be16(self->record + 3);
    if (length > RECORD_MAX) { blame("the server sent a record larger than TLS allows"); return -1; }

    while (self->record_have < RECORD_HEADER + length) {
        int got = tcp_receive(self->tcp, self->record + self->record_have,
                              RECORD_HEADER + length - self->record_have,
                              timeout);

        if (got <= 0) { blame("the connection ended in the middle of a record"); return -1; }
        self->record_have += (boot_uint32_t)got;
    }

    /* A record of "change cipher spec" means nothing in TLS 1.3 and is sent
       only so that middleboxes written for 1.2 see what they expect. Dropped
       here rather than confusing everything above. */
    if (self->record[0] == RECORD_CHANGE_CIPHER) {
        *type = RECORD_CHANGE_CIPHER;
        *body = self->record + RECORD_HEADER;
        return (int)length;
    }

    /* An alert may arrive in the clear - a server that refuses the handshake
       sends one before any keys exist. Once TLS 1.2 has switched on
       encryption its alerts are encrypted like everything else, though, and
       reading those as plain text turns a polite goodbye into nonsense. */
    if (!self->reading_encrypted ||
        (self->record[0] == RECORD_ALERT && self->version != 0x0303)) {
        *type = self->record[0];
        *body = self->record + RECORD_HEADER;
        return (int)length;
    }

    if (self->version == 0x0303) {
        boot_uint8_t nonce[AEAD_NONCE_SIZE];
        boot_uint8_t extra[13];
        boot_uint32_t inner;

        if (length < AEAD_TAG_SIZE) { blame("the server sent a record too short to be encrypted"); return -1; }
        inner = length - AEAD_TAG_SIZE;

        for (int at = 0; at < 8; at++)
            extra[at] = (boot_uint8_t)((self->server_sequence >>
                                        ((7 - at) * 8)) & 0xFF);
        extra[8] = self->record[0];
        extra[9] = 3;
        extra[10] = 3;
        put_be16(extra + 11, inner);

        record_nonce(self->server_iv, self->server_sequence, nonce);
        if (!aead_open(self->server_key, nonce, extra, sizeof(extra),
                       self->record + RECORD_HEADER, inner,
                       self->record + RECORD_HEADER + inner)) {
            blame("a record did not survive the journey - it was altered, or "
                  "the keys do not match");
            return -1;
        }
        self->server_sequence++;
        *type = self->record[0];
        *body = self->record + RECORD_HEADER;
        return (int)inner;
    }

    {
        boot_uint8_t nonce[AEAD_NONCE_SIZE];
        boot_uint32_t inner;

        if (length < AEAD_TAG_SIZE + 1) { blame("the server sent a record too short to be encrypted"); return -1; }
        inner = length - AEAD_TAG_SIZE;

        record_nonce(self->server_iv, self->server_sequence, nonce);
        if (!aead_open(self->server_key, nonce, self->record, RECORD_HEADER,
                       self->record + RECORD_HEADER, inner,
                       self->record + RECORD_HEADER + inner)) {
            blame("a record did not survive the journey - it was altered, or "
                  "the keys do not match");
            return -1;
        }
        self->server_sequence++;

        /* The real content type is the last byte that is not padding. */
        while (inner && !self->record[RECORD_HEADER + inner - 1]) inner--;
        if (!inner) { blame("the server sent a record with nothing in it"); return -1; }
        *type = self->record[RECORD_HEADER + inner - 1];
        *body = self->record + RECORD_HEADER;
        return (int)(inner - 1);
    }
}

/* ---- The handshake -------------------------------------------------------- */

static boot_uint32_t add_extension(boot_uint8_t* at, boot_uint32_t kind,
                                   const boot_uint8_t* body,
                                   boot_uint32_t length) {
    put_be16(at, kind);
    put_be16(at + 2, length);
    memcpy(at + 4, body, length);
    return 4 + length;
}

static int send_client_hello(SESSION* self, const char* host,
                             const boot_uint8_t public_key[X25519_SIZE],
                             boot_uint32_t timeout) {
    boot_uint8_t message[512];
    boot_uint8_t extensions[384];
    boot_uint32_t at = 0;
    boot_uint32_t extension_at = 0;
    boot_uint32_t host_length = 0;

    while (host && host[host_length]) host_length++;
    if (host_length > 200) { blame("that name is too long for a handshake"); return 0; }

    /* ---- the body of the hello ---- */
    message[at++] = HANDSHAKE_CLIENT_HELLO;
    at += 3;                                   /* length, filled in below */
    message[at++] = 3;
    message[at++] = 3;                         /* "TLS 1.2", as 1.3 requires */
    random_bytes(message + at, 32);            /* the client random */
    memcpy(self->client_random, message + at, 32);
    at += 32;
    message[at++] = 32;                        /* a session id, for looks */
    random_bytes(message + at, 32);
    at += 32;
    /* Three suites: the one TLS 1.3 uses, and the two that are the same
     * cipher under TLS 1.2 - with an RSA certificate and with an ECDSA one.
     *
     * All three are ChaCha20-Poly1305, which is the whole reason speaking the
     * older protocol is affordable at all: the record layer differs, the key
     * schedule differs, and the arithmetic underneath does not. */
    put_be16(message + at, 6);
    at += 2;
    put_be16(message + at, 0x1303);            /* TLS 1.3 */
    at += 2;
    put_be16(message + at, 0xCCA8);            /* 1.2, ECDHE with RSA */
    at += 2;
    put_be16(message + at, 0xCCA9);            /* 1.2, ECDHE with ECDSA */
    at += 2;
    message[at++] = 1;                         /* one compression method */
    message[at++] = 0;                         /* which is none */

    /* ---- extensions ---- */
    {
        boot_uint8_t body[300];
        boot_uint32_t length;

        /* The name being asked for, so a server with several sites answers as
           the right one. Without this most of the web answers with whatever
           certificate is first. */
        if (host_length) {
            put_be16(body, host_length + 3);
            body[2] = 0;                       /* a host name */
            put_be16(body + 3, host_length);
            memcpy(body + 5, host, host_length);
            extension_at += add_extension(extensions + extension_at, 0x0000,
                                          body, host_length + 5);
        }

        /* Which versions we speak, said in the only place that counts - and
           both of them, best first. A hello that names 1.3 alone is answered
           by an older server with alert 70 and nothing else, which is exactly
           what the badge host did. */
        body[0] = 4;
        put_be16(body + 1, 0x0304);
        put_be16(body + 3, 0x0303);
        extension_at += add_extension(extensions + extension_at, 0x002B, body, 5);

        /* The curves, and then the share on the first of them. TLS 1.2 uses
           this same list to decide what to put in its key exchange message,
           and a good many 1.2 servers will do P-256 and nothing else - which
           is why the older curve is here at all. */
        put_be16(body, 4);
        put_be16(body + 2, 0x001D);            /* x25519 */
        put_be16(body + 4, 0x0017);            /* secp256r1, for TLS 1.2 */
        extension_at += add_extension(extensions + extension_at, 0x000A, body, 6);

        /* Which shapes of point are understood - only the plain one, which is
           what everybody sends. TLS 1.2 servers expect to be told. */
        body[0] = 1;
        body[1] = 0;
        extension_at += add_extension(extensions + extension_at, 0x000B, body, 2);

        put_be16(body, X25519_SIZE + 4);
        put_be16(body + 2, 0x001D);
        put_be16(body + 4, X25519_SIZE);
        memcpy(body + 6, public_key, X25519_SIZE);
        extension_at += add_extension(extensions + extension_at, 0x0033, body,
                                      X25519_SIZE + 6);

        /* What signatures we could check, if we checked them. Offered
           honestly: a server picks from this list, and asking for algorithms
           nobody here can verify would be asking for a promise we do not
           intend to test. */
        length = 0;
        put_be16(body + 2 + length, 0x0804); length += 2;   /* rsa_pss_rsae_sha256 */
        put_be16(body + 2 + length, 0x0805); length += 2;   /* rsa_pss_rsae_sha384 */
        put_be16(body + 2 + length, 0x0401); length += 2;   /* rsa_pkcs1_sha256 */
        /* And ECDSA on P-256, which arrived once the curve could be checked.
         *
         * The rule this list follows: offer exactly what can be verified, and
         * nothing else. A server picks from it, so an algorithm offered and
         * not checkable turns a verifiable connection into an unverifiable
         * one - which is how this list looked while the curve was missing. */
        put_be16(body + 2 + length, 0x0403); length += 2;   /* ecdsa_p256_sha256 */
        put_be16(body, length);
        extension_at += add_extension(extensions + extension_at, 0x000D, body,
                                      length + 2);
    }

    put_be16(message + at, extension_at);
    at += 2;
    memcpy(message + at, extensions, extension_at);
    at += extension_at;

    /* The length of the handshake message, now that it is known. */
    message[1] = (boot_uint8_t)((at - 4) >> 16);
    message[2] = (boot_uint8_t)((at - 4) >> 8);
    message[3] = (boot_uint8_t)(at - 4);

    sha256_add(&self->transcript, message, at);
    return send_record(self, RECORD_HANDSHAKE, message, at, timeout);
}

/* Their half of the key exchange, out of a ServerHello. */
static int read_server_hello(SESSION* self, const boot_uint8_t* body,
                             boot_uint32_t length,
                             boot_uint8_t their_share[X25519_SIZE]) {
    boot_uint32_t at = 4;                      /* past type and length */
    boot_uint32_t extensions_end;
    int found = 0;
    int suite;

    /* Which version, and it is not the number in the header: TLS 1.3 leaves
       that saying 1.2 for the benefit of middleboxes and says the truth in an
       extension. So the default is 1.2 and the extension overrides it. */
    self->version = 0x0303;

    if (length < 44) { blame("the server's hello is too short to be one"); return 0; }
    at += 2;                                   /* the legacy version */
    memcpy(self->server_random, body + at, 32);
    at += 32;                                  /* their random */
    at += 1 + body[at];                        /* the session id echoed back */
    suite = (int)get_be16(body + at);
    if (suite != 0x1303 && suite != 0xCCA8 && suite != 0xCCA9) {
        blame("the server chose a cipher this does not have");
        return 0;
    }
    at += 2;
    at += 1;                                   /* compression, always none */

    if (at + 2 > length) { blame("the server's hello has no extensions"); return 0; }
    extensions_end = at + 2 + get_be16(body + at);
    at += 2;
    if (extensions_end > length) { blame("the server's hello is damaged"); return 0; }

    while (at + 4 <= extensions_end) {
        boot_uint32_t kind = get_be16(body + at);
        boot_uint32_t size = get_be16(body + at + 2);

        at += 4;
        if (at + size > extensions_end) break;
        if (kind == 0x0033) {                  /* their key share */
            if (size >= 4 + X25519_SIZE && get_be16(body + at) == 0x001D) {
                memcpy(their_share, body + at + 4, X25519_SIZE);
                found = 1;
            }
        } else if (kind == 0x002B) {
            if (size >= 2) self->version = (int)get_be16(body + at);
        }
        at += size;
    }

    if (self->version == 0x0304) {
        if (!found) {
            blame("the server did not send a key of the kind that was "
                  "offered");
            return 0;
        }
        return 1;
    }
    if (self->version != 0x0303) {
        blame("the server answered with a version of TLS this does not speak");
        return 0;
    }
    /* In 1.2 the key exchange has not happened yet - it arrives in a message
       of its own, a few messages later. Nothing more to read here. */
    (void)their_share;
    return 1;
}

static void log_alert(const boot_uint8_t* body, boot_uint32_t length) {
    /* An alert is two bytes: how bad, and which. Worth the log line - a
       handshake refused by the far end says why, and that is usually the
       fastest answer available. */
    serial_write("TLS: the server sent an alert, code ");
    if (length >= 2) serial_write_dec(body[1]);
    serial_write("\n");
}

static SESSION* session_of(int handle) {
    if (handle < 0 || handle >= TLS_MAX_SESSIONS) return (SESSION*)0;
    if (!sessions[handle].used) return (SESSION*)0;
    return &sessions[handle];
}

/* Today, as the string x509_valid_at compares against. */
static void now_as_text(char* out) {
    RTC_TIME clock;
    int at = 0;

    rtc_read(&clock);
    out[at++] = (char)('0' + (clock.year / 1000) % 10);
    out[at++] = (char)('0' + (clock.year / 100) % 10);
    out[at++] = (char)('0' + (clock.year / 10) % 10);
    out[at++] = (char)('0' + clock.year % 10);
    out[at++] = (char)('0' + clock.month / 10);
    out[at++] = (char)('0' + clock.month % 10);
    out[at++] = (char)('0' + clock.day / 10);
    out[at++] = (char)('0' + clock.day % 10);
    out[at++] = (char)('0' + clock.hour / 10);
    out[at++] = (char)('0' + clock.hour % 10);
    out[at++] = (char)('0' + clock.minute / 10);
    out[at++] = (char)('0' + clock.minute % 10);
    out[at++] = (char)('0' + clock.second / 10);
    out[at++] = (char)('0' + clock.second % 10);
    out[at] = 0;
}

/* The chain, walked from the leaf upwards.
 *
 * Each certificate must be signed by the next, the last must be signed by a
 * root this machine carries, every one must be inside its validity, and the
 * name typed must be one the leaf is for. All four, or nothing: a chain that
 * fails any of them is a chain that proves nothing, and reporting it as
 * "mostly verified" would be the worst answer available. */
static int check_chain(SESSION* self, const char* host) {
    boot_uint8_t* at = self->chain;
    boot_uint32_t left = self->chain_length;
    CERTIFICATE child;
    char today[16];
    int depth = 0;
    int have_child = 0;

    now_as_text(today);

    while (left >= 3 && depth < 10) {
        boot_uint32_t size = ((boot_uint32_t)at[0] << 16) |
                             ((boot_uint32_t)at[1] << 8) | at[2];
        CERTIFICATE current;

        if (size + 3 > left) { blame("the certificate chain is damaged"); return 0; }
        if (!x509_parse(at + 3, size, &current)) {
            blame(x509_trouble());
            return 0;
        }
        if (!x509_valid_at(&current, today)) {
            blame(depth ? "a certificate in the chain has expired"
                        : "this site's certificate is not valid today - check "
                          "the machine's clock as well");
            return 0;
        }

        if (!have_child) {
            child = current;
            self->leaf = current;
            self->have_leaf = 1;
            have_child = 1;
            if (!x509_name_matches(&child, host)) {
                blame("the certificate is for a different name than the one "
                      "asked for");
                return 0;
            }
        } else {
            /* Verifying one link is tens of milliseconds of arithmetic with
               no waiting in it, and a chain is several. Between links nothing
               is half done, so the processor goes round once - which is the
               difference between music that stutters while a page loads and
               music that does not. */
            task_yield();
            if (!x509_signed_by(&child, &current)) {
                serial_write("TLS: the link at depth ");
                serial_write_dec((boot_uint64_t)depth);
                serial_write(" did not verify\n");
                blame(x509_trouble());
                return 0;
            }
            child = current;
        }

        /* Anchored here, if this one was issued by somebody carried.
         *
         * Tried at every step rather than only at the end, because a server
         * may send more of the chain than is needed - its own root, or a
         * cross-signed pair kept for old clients. Stopping at the first
         * certificate that reaches a carried root is what every other client
         * does, and without it a perfectly good chain with one extra
         * certificate on the end is refused. */
        {
            const CERTIFICATE* root = x509_root_for(child.issuer,
                                                    child.issuer_length);

            task_yield();
            if (root && x509_signed_by(&child, root)) return 1;
        }

        at += 3 + size;
        left -= 3 + size;
        /* In TLS 1.3 every certificate carries two bytes of extensions after
           it, almost always empty. In 1.2 it does not, and skipping them
           there walks straight into the next certificate's length - which
           looks exactly like a damaged chain. */
        if (self->version == 0x0304 && left >= 2) {
            boot_uint32_t extensions = ((boot_uint32_t)at[0] << 8) | at[1];

            if (extensions + 2 > left) break;
            at += 2 + extensions;
            left -= 2 + extensions;
        }
        depth++;
    }

    if (!have_child) { blame("the server sent no certificate"); return 0; }
    blame("this chain ends at an authority this machine does not carry");
    return 0;
}


/* ---- The whole of TLS 1.2 -------------------------------------------------
 *
 * A different protocol wearing the same name. In 1.3 the server's very first
 * message carries its key share and everything after it is encrypted; in 1.2
 * the server sends its certificate and its key in the clear, one message at a
 * time, and says ServerHelloDone when it has finished. Only then do we send
 * our share, switch on encryption with a record of its own, and prove the
 * conversation with Finished.
 *
 * It is here because it is still what a good deal of the web speaks - the
 * badge host `cyber.dabamos.de` answers a 1.3-only hello with alert 70 and
 * nothing else. The suites offered are the ChaCha20-Poly1305 pair, so all of
 * this reuses the cipher that was already written and tested; nothing about
 * AES had to arrive with it.
 *
 * The signature is the part worth reading twice. In 1.3 the server signs the
 * transcript; here it signs its key share along with both randoms, and that
 * is what stops somebody from replaying an old ServerKeyExchange - the
 * randoms are new every time.
 */
static int handshake_12(SESSION* self, const char* host,
                        const boot_uint8_t secret[X25519_SIZE],
                        const boot_uint8_t our_share[X25519_SIZE],
                        boot_uint32_t timeout) {
    boot_uint8_t their_share[65];       /* x25519, or a P-256 point */
    boot_uint8_t our_point[65];
    boot_uint8_t p256_secret[32];
    boot_uint8_t shared[32];
    boot_uint32_t share_length = 0;
    boot_uint32_t our_length = 0;
    int curve = 0;
    boot_uint8_t randoms[64];
    boot_uint8_t key_block[2 * AEAD_KEY_SIZE + 2 * AEAD_NONCE_SIZE];
    boot_uint8_t digest[SHA384_SIZE];
    boot_uint8_t params[4 + 65];
    boot_uint32_t params_length = 0;
    boot_uint8_t signature[512];        /* room for a 4096-bit RSA one */
    boot_uint32_t signature_length = 0;
    boot_uint32_t digest_length = HASH_SIZE;
    int algorithm = X509_UNKNOWN;
    int have_key = 0;
    int done = 0;
    int wants_certificate = 0;

    self->pending_length = 0;

    /* ---- everything the server says before it stops talking ---- */
    while (!done) {
        int type = 0;
        boot_uint8_t* body = (boot_uint8_t*)0;
        int length;

        while (self->pending_length >= 4) {
            boot_uint32_t size = get_be24(self->pending + 1);
            int kind = self->pending[0];

            if (self->pending_length < 4 + size) break;

            if (kind == HANDSHAKE_CERTIFICATE) {
                /* No request context here - that is a 1.3 addition. The
                   chain begins straight after its own three-byte length. */
                if (size > 3) {
                    boot_uint32_t chain = size - 3;

                    if (chain <= sizeof(self->chain)) {
                        memcpy(self->chain, self->pending + 7, chain);
                        self->chain_length = chain;
                    } else {
                        blame("the certificate chain is larger than this can "
                              "hold");
                        return 0;
                    }
                }
            } else if (kind == HANDSHAKE_SERVER_KEY_EXCHANGE) {
                /* curve type, the curve, their length-prefixed share, then the
                   signature over both randoms and all of the above. */
                boot_uint8_t* at = self->pending + 4;

                if (size < 8 || at[0] != 3) {
                    blame("the server offered a key exchange this does not "
                          "know");
                    return 0;
                }
                curve = (int)get_be16(at + 1);
                share_length = at[3];
                if (!((curve == 0x001D && share_length == X25519_SIZE) ||
                      (curve == 0x0017 && share_length == 65 &&
                       at[4] == 4)) || size < 4 + share_length + 4) {
                    blame("the server chose a curve this does not have");
                    return 0;
                }
                params_length = 4 + share_length;
                memcpy(params, at, params_length);
                memcpy(their_share, at + 4, share_length);
                have_key = 1;

                {
                    boot_uint32_t offered = get_be16(at + params_length);

                    signature_length = get_be16(at + params_length + 2);
                    algorithm = offered == 0x0401 ? X509_RSA_PKCS1_SHA256
                                : offered == 0x0403 ? X509_ECDSA_SHA256
                                : offered == 0x0804 ? X509_RSA_PSS_SHA256
                                : offered == 0x0501 ? X509_RSA_PKCS1_SHA384
                                : offered == 0x0503 ? X509_ECDSA_SHA384
                                : X509_UNKNOWN;
                    digest_length = (offered & 0xFF00) == 0x0500
                                    ? SHA384_SIZE : HASH_SIZE;
                    if (signature_length > sizeof(signature) ||
                        params_length + 4 + signature_length > size) {
                        blame("the server's signature is not a size this "
                              "expects");
                        return 0;
                    }
                    memcpy(signature, at + params_length + 4,
                           signature_length);
                }
            } else if (kind == HANDSHAKE_CERTIFICATE_REQUEST) {
                /* Asked for a certificate of ours. There is none, and the
                   answer to that is an empty Certificate message rather than
                   silence - silence is a handshake failure. */
                wants_certificate = 1;
            } else if (kind == HANDSHAKE_SERVER_HELLO_DONE) {
                done = 1;
            }

            sha256_add(&self->transcript, self->pending, 4 + size);
            {
                boot_uint32_t used = 4 + size;

                for (boot_uint32_t index = used; index < self->pending_length;
                     index++)
                    self->pending[index - used] = self->pending[index];
                self->pending_length -= used;
            }
            if (done) break;
        }
        if (done) break;

        length = read_record(self, &type, &body, timeout);
        if (length < 0) return 0;
        if (length == 0) {
            blame("the connection ended in the middle of the handshake");
            return 0;
        }
        if (type == RECORD_ALERT) {
            log_alert(body, (boot_uint32_t)length);
            blame("the server gave up during the handshake");
            return 0;
        }
        if (type != RECORD_HANDSHAKE) {
            blame("the server sent something that is not a handshake message");
            return 0;
        }
        if (self->pending_length + (boot_uint32_t)length >
            sizeof(self->pending)) {
            blame("the server's certificate chain is larger than this can "
                  "hold");
            return 0;
        }
        memcpy(self->pending + self->pending_length, body,
               (boot_uint32_t)length);
        self->pending_length += (boot_uint32_t)length;
    }

    if (!have_key) {
        blame("the server never sent a key to agree with");
        return 0;
    }

    /* ---- who they are, and that the key is theirs ---- */
    if (!self->chain_length || !check_chain(self, host)) {
        if (!trouble[0]) blame("the server sent no certificate");
        return 0;
    }
    if (!self->have_leaf) {
        blame("the server's certificate could not be read");
        return 0;
    }

    memcpy(randoms, self->client_random, 32);
    memcpy(randoms + 32, self->server_random, 32);
    if (digest_length == SHA384_SIZE) {
        SHA384 hash;

        sha384_start(&hash);
        sha384_add(&hash, randoms, sizeof(randoms));
        sha384_add(&hash, params, params_length);
        sha384_finish(&hash, digest);
    } else {
        SHA256 hash;

        sha256_start(&hash);
        sha256_add(&hash, randoms, sizeof(randoms));
        sha256_add(&hash, params, params_length);
        sha256_finish(&hash, digest);
    }

    task_yield();
    if (algorithm == X509_UNKNOWN) {
        blame("the server signed with an algorithm this cannot check yet");
        return 0;
    }
    if (!(algorithm == X509_ECDSA_SHA256 || algorithm == X509_ECDSA_SHA384
          ? x509_ecdsa_verify_key(&self->leaf, digest, digest_length,
                                  signature, signature_length)
          : x509_rsa_verify_wide(self->leaf.modulus, self->leaf.modulus_length,
                                 self->leaf.exponent, algorithm, digest,
                                 digest_length, signature,
                                 signature_length))) {
        blame(x509_trouble());
        return 0;
    }
    self->identity_checked = 1;

    /* ---- our share, and the keys both sides now hold ---- */
    if (curve == 0x001D) {
        if (!x25519(shared, secret, their_share)) {
            blame("the server's key is one this cannot agree with");
            return 0;
        }
        memcpy(our_point, our_share, X25519_SIZE);
        our_length = X25519_SIZE;
    } else {
        /* P-256, which needs a secret of its own - the x25519 one offered in
           the hello is on a different curve and means nothing here. */
        random_bytes(p256_secret, sizeof(p256_secret));
        our_point[0] = 4;                       /* an uncompressed point */
        if (!p256_agree_base(p256_secret, our_point + 1, our_point + 33) ||
            !p256_agree(p256_secret, their_share + 1, their_share + 33,
                        shared)) {
            blame("the server's key is one this cannot agree with");
            return 0;
        }
        our_length = 65;
    }

    if (wants_certificate) {
        boot_uint8_t empty[7] = { HANDSHAKE_CERTIFICATE, 0, 0, 3, 0, 0, 0 };

        if (!send_record(self, RECORD_HANDSHAKE, empty, sizeof(empty),
                         timeout)) return 0;
        sha256_add(&self->transcript, empty, sizeof(empty));
    }

    {
        boot_uint8_t message[5 + 65];

        message[0] = HANDSHAKE_CLIENT_KEY_EXCHANGE;
        message[1] = 0;
        put_be16(message + 2, 1 + our_length);
        message[4] = (boot_uint8_t)our_length;
        memcpy(message + 5, our_point, our_length);
        if (!send_record(self, RECORD_HANDSHAKE, message, 5 + our_length,
                         timeout)) {
            blame("our half of the handshake could not be sent");
            return 0;
        }
        sha256_add(&self->transcript, message, 5 + our_length);
    }

    /* The master secret, and from it the four keys - ours and theirs, each a
       key and a nonce base. The order in the block is fixed by the standard
       and is not the order anybody would guess. */
    tls12_prf(shared, sizeof(shared), "master secret", randoms, sizeof(randoms),
              self->master, sizeof(self->master));
    {
        boot_uint8_t swapped[64];

        memcpy(swapped, self->server_random, 32);
        memcpy(swapped + 32, self->client_random, 32);
        tls12_prf(self->master, sizeof(self->master), "key expansion",
                  swapped, sizeof(swapped), key_block, sizeof(key_block));
    }
    memcpy(self->client_key, key_block, AEAD_KEY_SIZE);
    memcpy(self->server_key, key_block + AEAD_KEY_SIZE, AEAD_KEY_SIZE);
    memcpy(self->client_iv, key_block + 2 * AEAD_KEY_SIZE, AEAD_NONCE_SIZE);
    memcpy(self->server_iv, key_block + 2 * AEAD_KEY_SIZE + AEAD_NONCE_SIZE,
           AEAD_NONCE_SIZE);
    self->client_sequence = 0;
    self->server_sequence = 0;

    /* ChangeCipherSpec is a record of its own kind, is sent in the clear, and
       is not part of the transcript - it is not a handshake message at all,
       which is exactly the sort of detail 1.3 removed. */
    {
        boot_uint8_t one = 1;

        if (!send_record(self, RECORD_CHANGE_CIPHER, &one, 1, timeout)) {
            blame("our half of the handshake could not be sent");
            return 0;
        }
    }
    self->encrypted = 1;

    {
        boot_uint8_t message[4 + 12];
        boot_uint8_t so_far[HASH_SIZE];

        transcript_hash(self, so_far);
        message[0] = HANDSHAKE_FINISHED;
        message[1] = 0;
        message[2] = 0;
        message[3] = 12;
        tls12_prf(self->master, sizeof(self->master), "client finished",
                  so_far, HASH_SIZE, message + 4, 12);
        if (!send_record(self, RECORD_HANDSHAKE, message, sizeof(message),
                         timeout)) {
            blame("our half of the handshake could not be sent");
            return 0;
        }
        sha256_add(&self->transcript, message, sizeof(message));
    }

    /* ---- their turn: the same two records back ---- */
    {
        int type = 0;
        boot_uint8_t* body = (boot_uint8_t*)0;
        int length = read_record(self, &type, &body, timeout);

        if (length < 0) return 0;
        if (type == RECORD_ALERT) {
            log_alert(body, (boot_uint32_t)length);
            blame("the server refused our half of the handshake");
            return 0;
        }
        if (type != RECORD_CHANGE_CIPHER) {
            blame("the server did not switch on encryption when it should");
            return 0;
        }
    }
    self->reading_encrypted = 1;

    {
        int type = 0;
        boot_uint8_t* body = (boot_uint8_t*)0;
        int length = read_record(self, &type, &body, timeout);
        boot_uint8_t expected[12];
        boot_uint8_t so_far[HASH_SIZE];
        int same = 1;

        if (length < 0) return 0;
        if (type == RECORD_ALERT) {
            log_alert(body, (boot_uint32_t)length);
            blame("the server refused our half of the handshake");
            return 0;
        }
        if (type != RECORD_HANDSHAKE || length != 4 + 12 ||
            body[0] != HANDSHAKE_FINISHED) {
            blame("the server did not finish the handshake");
            return 0;
        }
        transcript_hash(self, so_far);
        tls12_prf(self->master, sizeof(self->master), "server finished",
                  so_far, HASH_SIZE, expected, sizeof(expected));
        for (int at = 0; at < 12; at++)
            if (expected[at] != body[4 + at]) same = 0;
        if (!same) {
            blame("the server's proof of the conversation did not match - "
                  "something altered it");
            return 0;
        }
    }
    return 1;
}

int tls_connect(boot_uint32_t address, boot_uint16_t port, const char* host,
                boot_uint32_t timeout_ms) {
    SESSION* self = (SESSION*)0;
    int handle = -1;
    boot_uint8_t secret[X25519_SIZE];
    boot_uint8_t our_share[X25519_SIZE];
    boot_uint8_t their_share[X25519_SIZE];
    boot_uint8_t shared[X25519_SIZE];
    boot_uint8_t early[HASH_SIZE];
    boot_uint8_t derived[HASH_SIZE];
    boot_uint8_t handshake[HASH_SIZE];
    boot_uint8_t master[HASH_SIZE];
    boot_uint8_t context[HASH_SIZE];
    boot_uint8_t empty_hash[HASH_SIZE];
    boot_uint8_t zeroes[HASH_SIZE];
    int finished_seen = 0;
    int verify_failed = 0;

    trouble[0] = 0;
    for (int at = 0; at < TLS_MAX_SESSIONS; at++)
        if (!sessions[at].used) { handle = at; break; }
    if (handle < 0) { blame("too many secure connections at once"); return -1; }

    self = &sessions[handle];
    memset(self, 0, sizeof(SESSION));
    self->used = 1;
    sha256_start(&self->transcript);

    /* Our half of the exchange. The secret exists only in this function and
       in the session's memory; nothing writes it anywhere. */
    random_bytes(secret, sizeof(secret));
    x25519_public(our_share, secret);

    self->tcp = tcp_connect(address, port, timeout_ms);
    if (self->tcp < 0) {
        blame("nothing answered on that port");
        self->used = 0;
        return -1;
    }

    if (!send_client_hello(self, host, our_share, timeout_ms)) {
        if (!trouble[0]) blame("the hello could not be sent");
        tcp_close(self->tcp, 500);
        self->used = 0;
        return -1;
    }

    /* ---- their hello, and the keys that follow from it ---- */
    {
        int type = 0;
        boot_uint8_t* body = (boot_uint8_t*)0;
        int length = read_record(self, &type, &body, timeout_ms);

        if (length <= 0 || type != RECORD_HANDSHAKE ||
            body[0] != HANDSHAKE_SERVER_HELLO) {
            if (length > 0 && type == RECORD_ALERT) {
                log_alert(body, (boot_uint32_t)length);
                blame("the server refused the handshake");
            } else if (!trouble[0]) {
                blame("the server did not answer with a hello");
            }
            tcp_close(self->tcp, 500);
            self->used = 0;
            return -1;
        }
        if (!read_server_hello(self, body, (boot_uint32_t)length,
                               their_share)) {
            tcp_close(self->tcp, 500);
            self->used = 0;
            return -1;
        }
        sha256_add(&self->transcript, body, (boot_uint32_t)length);
    }

    if (self->version == 0x0303) {
        /* An older server. Everything from here is a different protocol, so
           it lives in one function rather than as branches through this one. */
        if (!handshake_12(self, host, secret, our_share, timeout_ms)) {
            if (!trouble[0]) blame("the handshake did not finish");
            tcp_close(self->tcp, 500);
            self->used = 0;
            return -1;
        }
        self->open = 1;
        serial_write("TLS: connected over 1.2, and the certificate checks "
                     "out\n");
        return handle;
    }

    if (!x25519(shared, secret, their_share)) {
        blame("the server's key is one this cannot agree with");
        tcp_close(self->tcp, 500);
        self->used = 0;
        return -1;
    }

    /* ---- the schedule, in the order the standard writes it ---- */
    memset(zeroes, 0, sizeof(zeroes));
    sha256("", 0, empty_hash);
    hkdf_extract((const void*)0, 0, zeroes, HASH_SIZE, early);
    derive_secret(early, "derived", empty_hash, derived);
    hkdf_extract(derived, HASH_SIZE, shared, X25519_SIZE, handshake);

    transcript_hash(self, context);
    derive_secret(handshake, "c hs traffic", context, self->client_secret);
    derive_secret(handshake, "s hs traffic", context, self->server_secret);
    traffic_keys(self->client_secret, self->client_key, self->client_iv);
    traffic_keys(self->server_secret, self->server_key, self->server_iv);
    self->client_sequence = 0;
    self->server_sequence = 0;
    self->encrypted = 1;
    self->reading_encrypted = 1;

    /* ---- the encrypted half of their side ---- */
    self->pending_length = 0;
    for (;;) {
        int type = 0;
        boot_uint8_t* body = (boot_uint8_t*)0;
        int length;
        boot_uint32_t at = 0;

        /* Take whole messages out of what has already been joined before
           asking the wire for more. */
        while (self->pending_length >= 4) {
            boot_uint32_t size = get_be24(self->pending + 1);
            int kind = self->pending[0];

            if (self->pending_length < 4 + size) break;
            if (kind == HANDSHAKE_CERTIFICATE) {
                /* type, length, then one byte of request context, then the
                   chain itself with its own three-byte length. */
                if (size > 4 && (boot_uint32_t)(5 + self->pending[4] + 3) <= size + 4) {
                    boot_uint32_t start = 4 + 1 + self->pending[4] + 3;
                    boot_uint32_t chain = size + 4 - start;

                    if (chain <= sizeof(self->chain)) {
                        memcpy(self->chain, self->pending + start, chain);
                        self->chain_length = chain;
                    } else {
                        blame("the certificate chain is larger than this can "
                              "hold");
                    }
                }
                /* The transcript up to and including this message is what the
                   server's signature covers. */
                sha256_add(&self->transcript, self->pending, 4 + size);
                transcript_hash(self, self->before_verify);
                {
                    boot_uint32_t used = 4 + size;

                    for (boot_uint32_t index = used; index < self->pending_length;
                         index++)
                        self->pending[index - used] = self->pending[index];
                    self->pending_length -= used;
                }
                continue;
            }

            if (kind == HANDSHAKE_CERTIFICATE_VERIFY) {
                /* The server signs a fixed string, the transcript so far, and
                 * proves it holds the key in the certificate it just sent.
                 * Without this the certificate proves nothing: anybody can
                 * replay somebody else's certificate. */
                if (self->chain_length && !self->have_leaf) {
                    if (!check_chain(self, host)) verify_failed = 1;
                }
                if (self->have_leaf && size > 4) {
                    static const char context[] =
                        "                                "
                        "                                "
                        "TLS 1.3, server CertificateVerify";
                    boot_uint32_t offered = get_be16(self->pending + 4);
                    int algorithm = offered == 0x0804 ? X509_RSA_PSS_SHA256
                                    : offered == 0x0401 ? X509_RSA_PKCS1_SHA256
                                    : offered == 0x0403 ? X509_ECDSA_SHA256
                                    : X509_UNKNOWN;
                    boot_uint32_t signature_length =
                        get_be16(self->pending + 6);
                    SHA256 hash;
                    boot_uint8_t digest[HASH_SIZE];

                    sha256_start(&hash);
                    sha256_add(&hash, context, sizeof(context) - 1);
                    sha256_add(&hash, "", 1);          /* the zero separator */
                    sha256_add(&hash, self->before_verify, HASH_SIZE);
                    sha256_finish(&hash, digest);

                    task_yield();
                    if (algorithm == X509_UNKNOWN) {
                        blame("the server signed with an algorithm this "
                              "cannot check yet");
                        self->identity_checked = 0;
                        verify_failed = 1;
                    } else if (algorithm == X509_ECDSA_SHA256
                               ? x509_ecdsa_verify(self->leaf.curve_x,
                                                   self->leaf.curve_y, digest,
                                                   self->pending + 8,
                                                   signature_length)
                               : x509_rsa_verify(self->leaf.modulus,
                                                 self->leaf.modulus_length,
                                                 self->leaf.exponent, algorithm,
                                                 digest, self->pending + 8,
                                                 signature_length)) {
                        self->identity_checked = 1;
                    } else {
                        blame(x509_trouble());
                        self->identity_checked = 0;
                        verify_failed = 1;
                    }
                }
                sha256_add(&self->transcript, self->pending, 4 + size);
                {
                    boot_uint32_t used = 4 + size;

                    for (boot_uint32_t index = used; index < self->pending_length;
                         index++)
                        self->pending[index - used] = self->pending[index];
                    self->pending_length -= used;
                }
                continue;
            }

            if (kind == HANDSHAKE_FINISHED) {
                /* Their Finished is a code over everything said so far, keyed
                 * from their handshake secret. Checking it proves that the far
                 * end derived the same keys from the same transcript - that
                 * nothing in the conversation was altered on the way. It does
                 * not prove who they are; that is the certificate's job. */
                boot_uint8_t finished_key[HASH_SIZE];
                boot_uint8_t expected[HASH_SIZE];
                boot_uint8_t so_far[HASH_SIZE];
                int same = 1;

                transcript_hash(self, so_far);
                hkdf_expand_label(self->server_secret, "finished",
                                  (const boot_uint8_t*)0, 0, finished_key,
                                  HASH_SIZE);
                hmac_sha256(finished_key, HASH_SIZE, so_far, HASH_SIZE,
                            expected);
                if (size != HASH_SIZE) same = 0;
                else
                    for (boot_uint32_t index = 0; index < HASH_SIZE; index++)
                        if (expected[index] != self->pending[4 + index])
                            same = 0;
                if (!same) {
                    blame("the server's proof of the conversation did not "
                          "match - something altered it");
                    finished_seen = -1;
                    break;
                }
                finished_seen = 1;
            }

            sha256_add(&self->transcript, self->pending, 4 + size);
            {
                boot_uint32_t used = 4 + size;

                for (boot_uint32_t index = used; index < self->pending_length;
                     index++)
                    self->pending[index - used] = self->pending[index];
                self->pending_length -= used;
            }
            if (finished_seen) break;
        }
        if (finished_seen) break;

        length = read_record(self, &type, &body, timeout_ms);
        if (length < 0) break;
        if (length == 0 && type != RECORD_CHANGE_CIPHER) {
            blame("the connection ended in the middle of the handshake");
            break;
        }
        if (type == RECORD_CHANGE_CIPHER) continue;
        if (type == RECORD_ALERT) {
            log_alert(body, (boot_uint32_t)length);
            blame("the server gave up during the handshake");
            break;
        }
        if (type != RECORD_HANDSHAKE) {
            blame("the server sent something that is not a handshake message");
            break;
        }
        if (self->pending_length + (boot_uint32_t)length > sizeof(self->pending)) {
            blame("the server's certificate chain is larger than this can hold");
            break;
        }
        memcpy(self->pending + self->pending_length, body,
               (boot_uint32_t)length);
        self->pending_length += (boot_uint32_t)length;
        (void)at;
    }

    if (verify_failed) {
        /* Refused rather than continued. The connection would work; it would
           simply be a connection to somebody who has not proved who they are,
           and carrying on while saying so is a decision for a person, not a
           default. */
        tcp_close(self->tcp, 500);
        self->used = 0;
        return -1;
    }
    if (finished_seen != 1) {
        if (!trouble[0]) blame("the handshake did not finish");
        tcp_close(self->tcp, 500);
        self->used = 0;
        return -1;
    }

    /* ---- our Finished, then the keys for everything after ---- */
    /* The transcript as it stands *now* - up to and including their Finished
     * and not one message further.
     *
     * The application keys come from this point, and our own Finished is sent
     * afterwards: mixing it in as well produces keys that are wrong by exactly
     * one message, which is a handshake that completes perfectly and a first
     * record that will not decrypt. Taken here rather than after, because
     * "after" is the natural place to write it and the wrong one. */
    transcript_hash(self, context);

    {
        boot_uint8_t finished_key[HASH_SIZE];
        boot_uint8_t so_far[HASH_SIZE];
        boot_uint8_t message[4 + HASH_SIZE];

        transcript_hash(self, so_far);
        hkdf_expand_label(self->client_secret, "finished",
                          (const boot_uint8_t*)0, 0, finished_key, HASH_SIZE);
        message[0] = HANDSHAKE_FINISHED;
        message[1] = 0;
        message[2] = 0;
        message[3] = HASH_SIZE;
        hmac_sha256(finished_key, HASH_SIZE, so_far, HASH_SIZE, message + 4);
        if (!send_record(self, RECORD_HANDSHAKE, message, sizeof(message),
                         timeout_ms)) {
            blame("our half of the handshake could not be sent");
            tcp_close(self->tcp, 500);
            self->used = 0;
            return -1;
        }
        sha256_add(&self->transcript, message, sizeof(message));
    }

    derive_secret(handshake, "derived", empty_hash, derived);
    hkdf_extract(derived, HASH_SIZE, zeroes, HASH_SIZE, master);
    derive_secret(master, "c ap traffic", context, self->client_secret);
    derive_secret(master, "s ap traffic", context, self->server_secret);
    traffic_keys(self->client_secret, self->client_key, self->client_iv);
    traffic_keys(self->server_secret, self->server_key, self->server_iv);
    self->client_sequence = 0;
    self->server_sequence = 0;

    self->open = 1;
    serial_write(self->identity_checked
                 ? "TLS: connected, and the certificate checks out\n"
                 : "TLS: connected, encrypted, identity NOT checked\n");
    return handle;
}

int tls_send(int handle, const void* data, boot_uint32_t length,
             boot_uint32_t timeout_ms) {
    SESSION* self = session_of(handle);
    const boot_uint8_t* at = (const boot_uint8_t*)data;
    boot_uint32_t sent = 0;

    if (!self || !self->open) return -1;
    while (sent < length) {
        boot_uint32_t take = length - sent;

        if (take > 8192) take = 8192;
        if (!send_record(self, RECORD_APPLICATION, at + sent, take, timeout_ms))
            return -1;
        sent += take;
    }
    return (int)sent;
}

int tls_receive(int handle, void* buffer, boot_uint32_t size,
                boot_uint32_t timeout_ms) {
    SESSION* self = session_of(handle);
    boot_uint8_t* out = (boot_uint8_t*)buffer;

    if (!self) return -1;

    for (;;) {
        int type = 0;
        boot_uint8_t* body = (boot_uint8_t*)0;
        int length;

        /* Whatever was already decrypted and not yet taken. */
        if (self->plain_taken < self->plain_length) {
            boot_uint32_t left = self->plain_length - self->plain_taken;
            boot_uint32_t take = left < size ? left : size;

            memcpy(out, self->plain + self->plain_taken, take);
            self->plain_taken += take;
            return (int)take;
        }
        if (!self->open) return 0;

        length = read_record(self, &type, &body, timeout_ms);
        if (length == 0 && type == RECORD_CHANGE_CIPHER) continue;
        if (length == 0) { self->open = 0; return 0; }
        if (length < 0) return -1;

        if (type == RECORD_ALERT) {
            /* A close notify is the polite end of a conversation, not a
               failure. Anything else is worth the log line. */
            if (length >= 2 && body[1] == 0) { self->open = 0; return 0; }
            log_alert(body, (boot_uint32_t)length);
            self->open = 0;
            return 0;
        }
        if (type == RECORD_HANDSHAKE) {
            /* Session tickets and key updates arrive after the handshake.
               Tickets are for resuming later, which this does not do; a key
               update would need answering, and a server only sends one if it
               has been running a long time. Both are ignored, and the second
               is worth knowing about. */
            if (length > 0 && body[0] == HANDSHAKE_KEY_UPDATE)
                serial_write("TLS: the server asked to change keys; this does "
                             "not do that yet\n");
            continue;
        }
        if (type != RECORD_APPLICATION) continue;

        memcpy(self->plain, body, (boot_uint32_t)length);
        self->plain_length = (boot_uint32_t)length;
        self->plain_taken = 0;
    }
}

int tls_close(int handle) {
    SESSION* self = session_of(handle);

    if (!self) return -1;
    if (self->open) {
        boot_uint8_t alert[2] = { 1, 0 };      /* warning, close notify */

        send_record(self, RECORD_ALERT, alert, sizeof(alert), 500);
    }
    tcp_close(self->tcp, 1000);
    memset(self, 0, sizeof(SESSION));
    return 0;
}

int tls_is_open(int handle) {
    SESSION* self = session_of(handle);

    return self && (self->open || self->plain_taken < self->plain_length);
}

int tls_identity_checked(int handle) {
    SESSION* self = session_of(handle);

    return self ? self->identity_checked : 0;
}
