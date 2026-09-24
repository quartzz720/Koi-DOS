#ifndef KERNEL_TLS_H
#define KERNEL_TLS_H

#include "../include/bootinfo.h"

/* TLS, client side: the thing between TCP and https.
 *
 * ---- What this is and is not ---------------------------------------------
 *
 * It is the record layer and the handshake of RFC 8446 - TLS 1.3 - over the
 * arithmetic that was written and checked against the standards' own vectors
 * first: X25519 to agree a secret, HKDF-SHA256 to turn it into keys, and
 * ChaCha20-Poly1305 to carry the records.
 *
 * And, since a fair part of the small web has not moved, TLS 1.2 as well
 * (RFC 5246, with the ChaCha20-Poly1305 suites of RFC 7905). That is a
 * different protocol wearing the same name: the certificate arrives in the
 * clear, the key exchange is a message of its own and is signed, the keys come
 * out of a PRF rather than HKDF, and the record layer authenticates different
 * bytes. It shares the cipher and the certificate checking, which is what made
 * it affordable to add at all. Which one a connection turned out to be is
 * decided by the server's hello and carried in the session.
 *
 * On 1.2 the key exchange may also be over P-256 rather than X25519 - a good
 * many servers of that vintage will do that curve and no other.
 *
 * ---- The half that decides whether any of it means anything --------------
 *
 * Encryption answers "can anybody read this". It does not answer "who am I
 * talking to", and the second question is the one https exists for. Answering
 * it means checking the certificate the server sends: parsing X.509, verifying
 * a signature chain up to a root this machine already trusts, and checking the
 * name on it against the name that was typed.
 *
 * `tls_identity_checked` says whether that happened. While it returns 0, a
 * connection here is private from a passer-by and worth nothing against
 * somebody who can answer in the server's place - and everything above this
 * has to say so in those words rather than draw a padlock. A padlock that
 * cannot be justified is worse than none: it is a promise to somebody who
 * cannot check it.
 */

#define TLS_MAX_SESSIONS 4

/* Dial, do the handshake, and come back with a handle - or -1, with
   `tls_trouble()` holding a sentence about why. `host` is the name typed,
   which goes in the handshake so that a server with several sites answers as
   the right one. */
int tls_connect(boot_uint32_t address, boot_uint16_t port, const char* host,
                boot_uint32_t timeout_ms);

int tls_send(int handle, const void* data, boot_uint32_t length,
             boot_uint32_t timeout_ms);
/* What arrived, 0 when the other end has finished, -1 on failure. */
int tls_receive(int handle, void* buffer, boot_uint32_t size,
                boot_uint32_t timeout_ms);
int tls_close(int handle);
int tls_is_open(int handle);

/* Whether anybody checked who answered. Zero until certificates are. */
int tls_identity_checked(int handle);

/* Why the last call failed. Never null. */
const char* tls_trouble(void);

#endif
