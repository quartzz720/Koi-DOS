#ifndef KOI_PNG_H
#define KOI_PNG_H

#include "koi.h"

/* PNG, decoded into the pixels this system draws with.
 *
 * The format the web actually uses for anything that is not a photograph, and
 * the reason `inflate` was written first: a PNG is a header, a few labelled
 * chunks and one zlib stream with the rows in it.
 *
 * What is supported: 8 bits per channel, greyscale, greyscale with alpha,
 * truecolour, truecolour with alpha, and palette - which is all of PNG that
 * anything in the wild produces. What is not: 16-bit channels (halved to 8
 * would be silently lossy, so they are refused), and interlaced files, which
 * are a different row order and rare enough to be worth saying no to plainly.
 *
 * Alpha is composited onto a background colour the caller chooses rather than
 * kept: nothing below this draws with transparency, and a picture handed back
 * with an alpha channel would have every caller inventing its own answer.
 */

typedef struct {
    int width;
    int height;
    /* One koi_uint32 per pixel, 0x00RRGGBB, top row first - the same shape
       koi_gfx_blit takes. */
    koi_uint32* pixels;
    int has_alpha;              /* whether anything was composited */
} PNG;

/* Decode a whole file already in memory. `pixels` must point at room for
 * width x height pixels; `pixel_capacity` says how many fit.
 *
 * `background` is what shows through where the picture is transparent.
 * Returns 1 on success. On failure `png_trouble()` says why in a sentence
 * fit to show somebody. */
int png_decode(const void* file, long length, koi_uint32* pixels,
               long pixel_capacity, koi_uint32 background, PNG* out);

/* The width and height without decoding the picture - enough to lay a page
   out, or to refuse a file that will not fit before reading all of it. */
int png_size(const void* file, long length, int* width, int* height);

/* Whether these bytes begin with PNG's signature. */
int png_is_png(const void* file, long length);

/* How much scratch space `png_decode` needs for a picture of this size: the
   rows plus their filter bytes, which is what the zlib stream expands to. */
long png_scratch_needed(int width, int height, int bytes_per_pixel);

/* Give the decoder its scratch buffer. It does not allocate: this system has
   applications that know how much memory they can spare and a decoder that
   does not, and a decoder that allocates is one that fails in the middle of
   somebody else's drawing. */
void png_scratch(void* buffer, long size);

const char* png_trouble(void);

#endif
