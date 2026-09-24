#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include "../include/bootinfo.h"

/* Random bytes, for keys.
 *
 * The part of cryptography that is not arithmetic and cannot be checked
 * against a test vector: a wrong hash fails a published number, and a
 * predictable key passes every test there is and is worthless anyway. This is
 * where a system of this size is most likely to be quietly broken, so what it
 * does and does not guarantee is written down rather than assumed.
 *
 * The processor's own generator is used when it has one - every x86-64 made
 * since about 2012 does, and it is a hardware noise source. When it does not,
 * this falls back to stirring together the timestamp counter, the real-time
 * clock, how long various things took and the state accumulated so far, and
 * hashing the lot. That fallback is honest but weaker: it is enough that two
 * boots of the same machine do not produce the same keys, and it is not enough
 * to bet money on.
 *
 * `random_is_strong()` says which case the machine is in, so that anything
 * about to depend on it can say so out loud rather than pretending.
 */

/* Called once at startup, before anything asks for a key. */
void random_start(void);

/* Fill a buffer. Never fails; on a machine with no hardware source it is the
   fallback above. */
void random_bytes(void* out, boot_uint32_t length);

/* Whether the processor's generator is behind those bytes. */
int random_is_strong(void);

/* Anything unpredictable that happens anyway - a key pressed, a packet
   arriving, a sector read - can be stirred in here. Costs almost nothing and
   makes the fallback meaningfully better on a machine that has been running
   for a while. */
void random_stir(const void* data, boot_uint32_t length);

#endif
