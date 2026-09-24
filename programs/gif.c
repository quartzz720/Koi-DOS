#include "gif.h"

/* GIF.
 *
 * ---- The shape of a file --------------------------------------------------
 *
 * "GIF89a", then the size of the whole picture and an optional palette for it,
 * then a stream of blocks until a semicolon: an image, a graphic control (the
 * delay and which colour is transparent), a comment, or an extension nobody
 * needs. Each image says where it sits inside the whole picture and may bring
 * a palette of its own.
 *
 * ---- LZW, which is the only real work -------------------------------------
 *
 * Codes of growing width index a table that is built while reading. The table
 * starts as the palette itself plus two controls, and every code read adds one
 * entry: what the previous code meant, plus the first byte of what this one
 * means. That rule is the whole algorithm, and it is why the compressor and
 * the decompressor stay in step without the table ever being transmitted.
 *
 * The one place it bites: a code can refer to the entry being created by the
 * code itself - the sequence that repeats immediately. The rule for that case
 * is written out below rather than being clever, because it is the case every
 * broken implementation gets wrong.
 *
 * Bits come least significant first, and codes are packed across byte
 * boundaries. Data arrives in sub-blocks with a length byte in front of each,
 * which the bit reader below walks through as if they were one stream.
 */

#define MAX_CODES 4096

static char trouble[96];

const char* gif_trouble(void) {
    return trouble[0] ? trouble : "nothing went wrong";
}

static void blame(const char* what) {
    int at = 0;

    while (what[at] && at + 1 < (int)sizeof(trouble)) {
        trouble[at] = what[at];
        at++;
    }
    trouble[at] = 0;
}

int gif_is_gif(const void* file, long length) {
    const koi_uint8* data = (const koi_uint8*)file;

    if (length < 6) return 0;
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') return 0;
    return data[3] == '8' && (data[4] == '7' || data[4] == '9') &&
           data[5] == 'a';
}

/* Sub-blocks: a length, that many bytes, and so on until a zero length. Used
   for image data and for the extensions that are skipped. */
static long skip_blocks(const koi_uint8* data, long length, long at) {
    while (at < length) {
        int size = data[at++];

        if (!size) return at;
        at += size;
    }
    return at;
}

int gif_open(const void* file, long length, GIF* out) {
    const koi_uint8* data = (const koi_uint8*)file;
    long at = 13;
    int delay_ms = 0;
    int transparent = -1;
    int disposal = 0;

    trouble[0] = 0;
    if (!gif_is_gif(file, length)) { blame("that is not a GIF"); return 0; }
    if (length < 14) { blame("the file ends before its header"); return 0; }

    out->data = data;
    out->length = length;
    out->width = data[6] | (data[7] << 8);
    out->height = data[8] | (data[9] << 8);
    out->background = data[11];
    out->frame_count = 0;
    out->global_palette = 0;
    out->global_count = 0;

    if (data[10] & 0x80) {
        out->global_count = 2 << (data[10] & 7);
        out->global_palette = at;
        at += (long)out->global_count * 3;
    }
    if (out->width <= 0 || out->height <= 0) {
        blame("the picture has no size");
        return 0;
    }

    while (at < length) {
        int block = data[at++];

        if (block == 0x3B) break;                  /* the end */

        if (block == 0x21) {                       /* an extension */
            int kind;

            if (at >= length) break;
            kind = data[at++];
            if (kind == 0xF9 && at + 6 <= length) {
                /* The graphic control: how long this frame stays, which
                   colour shows through, and what to do with its area after. */
                int flags = data[at + 1];

                delay_ms = (data[at + 2] | (data[at + 3] << 8)) * 10;
                transparent = (flags & 1) ? data[at + 4] : -1;
                disposal = (flags >> 2) & 7;
                at = skip_blocks(data, length, at);
                continue;
            }
            at = skip_blocks(data, length, at);
            continue;
        }

        if (block == 0x2C) {                       /* an image */
            GIF_FRAME* frame;

            if (at + 9 > length) break;
            if (out->frame_count >= GIF_MAX_FRAMES) break;
            frame = &out->frames[out->frame_count];
            frame->x = data[at] | (data[at + 1] << 8);
            frame->y = data[at + 2] | (data[at + 3] << 8);
            frame->width = data[at + 4] | (data[at + 5] << 8);
            frame->height = data[at + 6] | (data[at + 7] << 8);
            {
                int flags = data[at + 8];

                frame->local_palette = (flags & 0x80) ? 1 : 0;
                frame->interlaced = (flags & 0x40) ? 1 : 0;
                frame->palette_count = frame->local_palette
                                       ? (2 << (flags & 7)) : 0;
            }
            at += 9;
            frame->palette_at = 0;
            if (frame->local_palette) {
                frame->palette_at = at;
                at += (long)frame->palette_count * 3;
            }
            frame->delay_ms = delay_ms;
            frame->transparent = transparent;
            frame->disposal = disposal;
            frame->at = at;                        /* the code size, then data */

            /* Every frame carries its own control, and one that carries none
               means what the last one said - so these are not reset here. */
            if (at >= length) break;
            at = skip_blocks(data, length, at + 1);
            out->frame_count++;
            continue;
        }

        /* Anything else is a file this does not understand well enough to
           walk, and guessing past it would be worse than stopping. */
        break;
    }

    if (!out->frame_count) { blame("the file has no picture in it"); return 0; }
    return 1;
}

/* ---- LZW ----------------------------------------------------------------- */

typedef struct {
    const koi_uint8* data;
    long length;
    long at;                    /* where in the file */
    int left;                   /* bytes remaining in this sub-block */
    koi_uint32 bits;            /* what has been read and not used */
    int held;                   /* how many bits that is */
} CODES;

static int next_code(CODES* self, int width) {
    while (self->held < width) {
        int byte;

        if (!self->left) {
            if (self->at >= self->length) return -1;
            self->left = self->data[self->at++];
            if (!self->left) return -1;            /* the end of the data */
        }
        if (self->at >= self->length) return -1;
        byte = self->data[self->at++];
        self->left--;
        self->bits |= (koi_uint32)byte << self->held;
        self->held += 8;
    }
    {
        int code = (int)(self->bits & (koi_uint32)((1 << width) - 1));

        self->bits >>= width;
        self->held -= width;
        return code;
    }
}

static int decode_indices(const GIF* self, const GIF_FRAME* frame,
                          koi_uint8* out, long out_size) {
    /* The table: for each code, the byte it ends with and the code for
       everything before it. Held this way rather than as strings because a
       string can be four thousand bytes long and there are four thousand of
       them - and walking backwards from the end costs nothing. */
    static short previous[MAX_CODES];
    static koi_uint8 last[MAX_CODES];
    CODES codes;
    int minimum;
    int clear_code, end_code, next_free, width;
    int old = -1;
    long written = 0;
    koi_uint8 first_byte = 0;

    if (frame->at >= self->length) { blame("the frame is empty"); return 0; }
    minimum = self->data[frame->at];
    if (minimum < 2 || minimum > 8) { blame("the frame's code size is impossible"); return 0; }

    codes.data = self->data;
    codes.length = self->length;
    codes.at = frame->at + 1;
    codes.left = 0;
    codes.bits = 0;
    codes.held = 0;

    clear_code = 1 << minimum;
    end_code = clear_code + 1;
    next_free = end_code + 1;
    width = minimum + 1;

    for (int at = 0; at < clear_code; at++) {
        previous[at] = -1;
        last[at] = (koi_uint8)at;
    }

    for (;;) {
        int code = next_code(&codes, width);
        int walk;
        long span = 0;

        if (code < 0) break;
        if (code == end_code) break;
        if (code == clear_code) {
            next_free = end_code + 1;
            width = minimum + 1;
            old = -1;
            continue;
        }

        if (code < next_free) {
            walk = code;
        } else if (code == next_free && old >= 0) {
            /* The one awkward case: a code for the entry this very code is
               about to create. It can only mean "what the last code meant,
               followed by its own first byte" - which is the sequence that
               repeats immediately, and is why this case exists at all. */
            walk = old;
        } else {
            blame("the compressed data is damaged");
            return 0;
        }

        /* Walk the chain to find how long it is, then write it backwards. */
        {
            int step = walk;

            while (step >= 0) { span++; step = previous[step]; }
        }
        if (code == next_free && old >= 0) span++;
        if (written + span > out_size) { blame("the picture is larger than there is room for"); return 0; }

        {
            long end = written + span;
            long put = end - 1;
            int step = walk;

            if (code == next_free && old >= 0) {
                /* The extra byte at the end is the first of the sequence, and
                   that is known before the walk. */
                put--;
            }
            while (step >= 0) {
                out[put--] = last[step];
                first_byte = last[step];
                step = previous[step];
            }
            if (code == next_free && old >= 0) out[end - 1] = first_byte;
            written = end;
        }

        if (old >= 0 && next_free < MAX_CODES) {
            previous[next_free] = (short)old;
            last[next_free] = first_byte;
            next_free++;
            if (next_free == (1 << width) && width < 12) width++;
        }
        old = code;
    }

    /* A frame that stops early is a damaged file, and showing the part that
       arrived is better than showing nothing - so the rest stays as it was. */
    return 1;
}

/* Interlaced GIFs arrive in four passes down the picture: every eighth row
 * from the top, then every eighth from the fourth, then every fourth from the
 * second, then every second from the first. A viewer of 1995 showed the
 * picture getting sharper as it downloaded; here it is only a question of
 * which row a decoded row belongs to.
 *
 * Written as the table it is. The first version tried to walk the four passes
 * with one loop and a pile of conditions, and got the third pass wrong - which
 * looked like a picture whose lower two thirds were missing, on the one test
 * file that happened to be interlaced.
 */
static int row_of(int decoded_row, int height, int interlaced) {
    static const int start[4] = { 0, 4, 2, 1 };
    static const int step[4] = { 8, 8, 4, 2 };
    int at = 0;

    if (!interlaced) return decoded_row;
    for (int pass = 0; pass < 4; pass++)
        for (int row = start[pass]; row < height; row += step[pass]) {
            if (at == decoded_row) return row;
            at++;
        }
    return decoded_row;
}

int gif_frame(const GIF* self, int index, koi_uint32* canvas,
              koi_uint32 background, koi_uint8* scratch, long scratch_size) {
    const GIF_FRAME* frame;
    const koi_uint8* palette;
    int palette_count;
    long needed;

    trouble[0] = 0;
    if (index < 0 || index >= self->frame_count) { blame("there is no such frame"); return 0; }
    frame = &self->frames[index];
    needed = (long)frame->width * frame->height;
    if (needed <= 0) { blame("the frame has no size"); return 0; }
    if (needed > scratch_size) { blame("there is not enough working memory for this picture"); return 0; }

    if (frame->local_palette) {
        palette = self->data + frame->palette_at;
        palette_count = frame->palette_count;
    } else {
        palette = self->data + self->global_palette;
        palette_count = self->global_count;
    }
    if (!palette_count) { blame("the picture has no colours"); return 0; }

    /* The first frame starts from the background; the others are painted onto
       what is already there, which is what makes an animation a sequence of
       changes rather than a sequence of pictures. */
    if (!index)
        for (long at = 0; at < (long)self->width * self->height; at++)
            canvas[at] = background;

    if (!decode_indices(self, frame, scratch, scratch_size)) return 0;

    for (int row = 0; row < frame->height; row++) {
        int target = frame->y + row_of(row, frame->height, frame->interlaced);

        if (target < 0 || target >= self->height) continue;
        for (int column = 0; column < frame->width; column++) {
            int x = frame->x + column;
            int value = scratch[(long)row * frame->width + column];

            if (x < 0 || x >= self->width) continue;
            if (value == frame->transparent) continue;   /* what is under it */
            if (value >= palette_count) continue;
            canvas[(long)target * self->width + x] =
                ((koi_uint32)palette[value * 3] << 16) |
                ((koi_uint32)palette[value * 3 + 1] << 8) |
                (koi_uint32)palette[value * 3 + 2];
        }
    }
    return 1;
}
