#ifndef KERNEL_AEAD_H
#define KERNEL_AEAD_H

#include "../include/bootinfo.h"

/* ChaCha20-Poly1305 (RFC 8439): the cipher a TLS 1.3 record is carried in.
 *
 * Chosen over AES-GCM for one reason that matters here - it is arithmetic on
 * 32-bit words, so it runs at the same speed on any processor and in constant
 * time without needing the AES instructions or a cache-timing-safe table. This
 * system runs on whatever laptop it is put on, and a cipher whose safety
 * depends on a table staying in cache is a cipher that is only safe on the
 * machine it was measured on.
 *
 * "AEAD" is the whole job in one call: the message is encrypted and the
 * message *and* its headers are authenticated together. Encryption without
 * that is the mistake every protocol of the 1990s made - an attacker who
 * cannot read a record can still change it, and a decryption that succeeds on
 * altered bytes is worse than no encryption, because it looks correct.
 */

#define AEAD_KEY_SIZE 32
#define AEAD_NONCE_SIZE 12
#define AEAD_TAG_SIZE 16

/* The stream cipher on its own, which TLS also needs for the record-number
   masking. `counter` is the block number the keystream starts at. */
void chacha20(const boot_uint8_t key[AEAD_KEY_SIZE],
              const boot_uint8_t nonce[AEAD_NONCE_SIZE],
              boot_uint32_t counter,
              const boot_uint8_t* input, boot_uint8_t* output,
              boot_uint32_t length);

/* The one-time authenticator on its own. The key must never be reused. */
void poly1305(const boot_uint8_t key[32], const boot_uint8_t* data,
              boot_uint32_t length, boot_uint8_t tag[AEAD_TAG_SIZE]);

/* Encrypt in place and produce the tag. `extra` is the data that is
   authenticated but not encrypted - in TLS, the record header. */
void aead_seal(const boot_uint8_t key[AEAD_KEY_SIZE],
               const boot_uint8_t nonce[AEAD_NONCE_SIZE],
               const boot_uint8_t* extra, boot_uint32_t extra_length,
               boot_uint8_t* data, boot_uint32_t length,
               boot_uint8_t tag[AEAD_TAG_SIZE]);

/* Decrypt in place, but only if the tag is right. Returns 0 and leaves
 * nothing readable behind when it is not.
 *
 * The tag is compared in constant time. A comparison that stops at the first
 * wrong byte tells an attacker how much of a forged tag was correct, and that
 * is enough to find the rest one byte at a time. */
int aead_open(const boot_uint8_t key[AEAD_KEY_SIZE],
              const boot_uint8_t nonce[AEAD_NONCE_SIZE],
              const boot_uint8_t* extra, boot_uint32_t extra_length,
              boot_uint8_t* data, boot_uint32_t length,
              const boot_uint8_t tag[AEAD_TAG_SIZE]);

#endif
