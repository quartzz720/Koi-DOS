#ifndef KOI_GIF_H
#define KOI_GIF_H

#include "koi.h"

/* GIF, frames and all.
 *
 * The format the web of small pictures is made of: badges, buttons, dividers,
 * a banner that says UNDER CONSTRUCTION and flashes. Not a relative of JPEG at
 * all - a palette of at most 256 colours, LZW compression, and optionally
 * several frames with a delay on each. That last part is why a GIF moves and a
 * PNG does not, and it is a property of the file rather than of anything in
 * the browser.
 *
 * Written before JPEG deliberately. It is the smaller job by a wide margin -
 * LZW is a hundred lines and the rest is bookkeeping - and it is what the
 * pages this system is aimed at actually contain.
 *
 * ---- How it is used -------------------------------------------------------
 *
 * `gif_open` reads the header and finds every frame, without decoding any of
 * them. `gif_frame` decodes one onto a canvas the caller keeps between calls,
 * because a GIF frame is not a picture - it is a change to the picture before
 * it, and the file says what to do with the area afterwards.
 *
 * So an animation is: open once, keep one canvas, and ask for frame 0, 1, 2 in
 * turn, each after its own delay.
 */

#define GIF_MAX_FRAMES 64

typedef struct {
    long at;                    /* where in the file this frame begins */
    int x, y, width, height;
    int delay_ms;
    int transparent;            /* the index that shows through, or -1 */
    int disposal;               /* what to do with this frame's area after */
    int local_palette;
    long palette_at;
    int palette_count;
    int interlaced;
} GIF_FRAME;

typedef struct {
    const koi_uint8* data;
    long length;
    int width, height;          /* the whole picture, which frames sit inside */
    long global_palette;
    int global_count;
    int background;
    int frame_count;
    GIF_FRAME frames[GIF_MAX_FRAMES];
} GIF;

int gif_is_gif(const void* file, long length);

/* Read the header and walk the blocks. Nothing is decoded here, so this is
   cheap enough to do before deciding whether the picture is wanted. */
int gif_open(const void* file, long length, GIF* out);

/* Decode frame `index` onto `canvas`, which is width x height pixels of
 * 0x00RRGGBB and belongs to the caller between calls. `background` is what
 * shows through where the picture is transparent.
 *
 * `scratch` is somewhere to put the decoded indices - one byte per pixel of
 * the frame - because this does not allocate.
 */
int gif_frame(const GIF* self, int index, koi_uint32* canvas,
              koi_uint32 background, koi_uint8* scratch, long scratch_size);

const char* gif_trouble(void);

#endif
