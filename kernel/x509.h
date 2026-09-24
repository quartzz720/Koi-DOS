#ifndef KERNEL_X509_H
#define KERNEL_X509_H

#include "../include/bootinfo.h"

/* Certificates: who is on the other end, and who says so.
 *
 * The half of https that is not encryption. A certificate says "this name
 * belongs to whoever holds the private key for this public key", and it is
 * signed by somebody else, who was signed by somebody else, up to a handful of
 * keys this machine carries and believes without being told. Checking that
 * chain is what turns a private conversation into a conversation with a
 * particular server.
 *
 * ---- What is read, and what is refused -----------------------------------
 *
 * RSA signatures, with SHA-256, in both spellings the web uses - PKCS#1 v1.5
 * for the signatures on certificates, PSS for the one TLS 1.3 asks a server to
 * make live. ECDSA is not read yet, and a chain that uses it is refused in
 * those words rather than waved through: a check that cannot be made must not
 * be reported as a check that passed.
 *
 * DER is walked rather than parsed into a tree. Every field wanted is at a
 * known place in a known order, and a tree would be a second representation to
 * keep in step with the first.
 */

#define X509_UNKNOWN 0
#define X509_RSA_PKCS1_SHA256 1
#define X509_RSA_PKCS1_SHA384 2
#define X509_RSA_PSS_SHA256 3
#define X509_ECDSA_SHA256 4
#define X509_ECDSA_SHA384 5

#define X509_NAME_MAX 64

typedef struct {
    const boot_uint8_t* tbs;            /* the part that is signed */
    boot_uint32_t tbs_length;
    const boot_uint8_t* signature;
    boot_uint32_t signature_length;
    int algorithm;

    /* The public key, when it is RSA. */
    const boot_uint8_t* modulus;
    boot_uint32_t modulus_length;
    boot_uint32_t exponent;

    /* Or the two coordinates, when the key is on P-256. Thirty-two bytes
       each, pointing into the certificate itself. */
    const boot_uint8_t* curve_x;
    const boot_uint8_t* curve_y;
    boot_uint32_t curve_size;           /* 32 for P-256, 48 for P-384 */

    const boot_uint8_t* issuer;         /* raw, for matching a parent */
    boot_uint32_t issuer_length;
    const boot_uint8_t* subject;
    boot_uint32_t subject_length;

    char not_before[16];                /* YYYYMMDDHHMMSS */
    char not_after[16];
    int is_authority;

    const boot_uint8_t* names;          /* subjectAltName, raw */
    boot_uint32_t names_length;
    char common_name[X509_NAME_MAX];
} CERTIFICATE;

int x509_parse(const boot_uint8_t* der, boot_uint32_t length,
               CERTIFICATE* out);

/* Whether `child` was signed by the key in `parent`. */
int x509_signed_by(const CERTIFICATE* child, const CERTIFICATE* parent);

/* Whether the name typed is one this certificate is for, wildcards included. */
int x509_name_matches(const CERTIFICATE* self, const char* host);

/* Whether `when` - "YYYYMMDDHHMMSS" - is inside its validity. */
int x509_valid_at(const CERTIFICATE* self, const char* when);

/* A signature made with somebody's RSA key over a SHA-256 hash, in either
   spelling. Used for the chain and for TLS 1.3's CertificateVerify. */
int x509_rsa_verify(const boot_uint8_t* modulus, boot_uint32_t modulus_length,
                    boot_uint32_t exponent, int algorithm,
                    const boot_uint8_t hash[32],
                    const boot_uint8_t* signature,
                    boot_uint32_t signature_length);

/* The same, for a hash that is not 32 bytes - SHA-384 in a chain. */
int x509_rsa_verify_wide(const boot_uint8_t* modulus,
                         boot_uint32_t modulus_length,
                         boot_uint32_t exponent, int algorithm,
                         const boot_uint8_t* hash, boot_uint32_t hash_length,
                         const boot_uint8_t* signature,
                         boot_uint32_t signature_length);

/* The roots this machine believes without being told. Returns the one whose
   subject matches `issuer`, or null. */
const CERTIFICATE* x509_root_for(const boot_uint8_t* issuer,
                                 boot_uint32_t issuer_length);
int x509_root_count(void);

/* An ECDSA signature, as DER, against a P-256 key. Used by the handshake for
   CertificateVerify as well as by the chain. */
int x509_ecdsa_verify(const boot_uint8_t* key_x, const boot_uint8_t* key_y,
                      const boot_uint8_t hash[32],
                      const boot_uint8_t* signature,
                      boot_uint32_t signature_length);

/* A signature made by a key already parsed out of a certificate, with the
   curve taken from that certificate rather than assumed. */
int x509_ecdsa_verify_key(const CERTIFICATE* key, const boot_uint8_t* hash,
                          boot_uint32_t hash_length,
                          const boot_uint8_t* signature,
                          boot_uint32_t signature_length);

const char* x509_trouble(void);

#endif
