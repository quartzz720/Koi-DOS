#ifndef KOI_INFLATE_H
#define KOI_INFLATE_H

#include "koi.h"

/* DEFLATE (RFC 1951), and the zlib wrapper around it (RFC 1950).
 *
 * The one decompressor worth writing, because everything wants it: PNG is
 * zlib, zip is raw deflate, gzip is deflate with a different wrapper, and a
 * font file or a spreadsheet from this century is a zip with a different
 * ending. Written once here rather than three times later.
 *
 * Deliberately whole-buffer: input in memory, output in memory, no callbacks
 * and no streaming. A picture and an archive entry are both things this system
 * reads into memory anyway, and a streaming interface would be a state machine
 * to get wrong for no gain at these sizes.
 */

/* How much came out, or -1. `out_size` is the room available; a stream that
   would need more than that is a failure rather than a truncation, because
   half a picture that reports success is the same lie as half a file. */
long inflate_raw(const void* input, long input_length,
                 void* out, long out_size);

/* The same, with the two-byte zlib header and the Adler checksum around it -
   which is what a PNG's image data is. The checksum is verified: a picture
   that decodes to rubbish and says nothing is worse than one that refuses. */
long inflate_zlib(const void* input, long input_length,
                  void* out, long out_size);

/* Why the last call failed, in a sentence. Never null. */
const char* inflate_trouble(void);

#endif
