#include "png.h"
#include "inflate.h"

/* PNG.
 *
 * ---- The shape of a file --------------------------------------------------
 *
 * Eight bytes of signature, then chunks: a length, a four-letter name, that
 * many bytes, and a checksum. IHDR says how big and in what form, PLTE holds a
 * palette when there is one, IDAT holds the image - possibly split across
 * several chunks that have to be joined before inflating - and IEND ends it.
 * Anything else is skipped, which is most of a real file: colour profiles,
 * text, timestamps, the name of the program that saved it.
 *
 * ---- Why it is not just inflate -------------------------------------------
 *
 * Every row is filtered before compression: each byte has one of five
 * predictions subtracted from it - the byte to the left, the one above, their
 * average, or Paeth's choice between three neighbours. Undoing that has to
 * happen in order, row by row, because each row's prediction refers to the row
 * already reconstructed. It is the part that makes PNG compress well and the
 * part where an off-by-one produces a picture that is recognisably the right
 * picture and visibly wrong - diagonal smears, usually.
 */

#define CHUNK_IHDR 0
#define MAX_PALETTE 256

static char trouble[96];
static koi_uint8* scratch;
static long scratch_size;

const char* png_trouble(void) {
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

void png_scratch(void* buffer, long size) {
    scratch = (koi_uint8*)buffer;
    scratch_size = size;
}

static unsigned long be32(const koi_uint8* at) {
    return ((unsigned long)at[0] << 24) | ((unsigned long)at[1] << 16) |
           ((unsigned long)at[2] << 8) | (unsigned long)at[3];
}

int png_is_png(const void* file, long length) {
    static const koi_uint8 signature[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    const koi_uint8* data = (const koi_uint8*)file;

    if (length < 8) return 0;
    for (int at = 0; at < 8; at++) if (data[at] != signature[at]) return 0;
    return 1;
}

static int channels_of(int colour_type) {
    switch (colour_type) {
    case 0: return 1;           /* grey */
    case 2: return 3;           /* red, green, blue */
    case 3: return 1;           /* an index into the palette */
    case 4: return 2;           /* grey and alpha */
    case 6: return 4;           /* colour and alpha */
    default: return 0;
    }
}

int png_size(const void* file, long length, int* width, int* height) {
    const koi_uint8* data = (const koi_uint8*)file;

    trouble[0] = 0;
    if (!png_is_png(file, length)) { blame("that is not a PNG"); return 0; }
    if (length < 33) { blame("the file ends before its header"); return 0; }
    if (data[12] != 'I' || data[13] != 'H' || data[14] != 'D' ||
        data[15] != 'R') { blame("the file does not start with a header"); return 0; }
    *width = (int)be32(data + 16);
    *height = (int)be32(data + 20);
    if (*width <= 0 || *height <= 0) { blame("the picture has no size"); return 0; }
    return 1;
}

long png_scratch_needed(int width, int height, int bytes_per_pixel) {
    /* Every row is one filter byte and then its pixels; two rows of that are
       held at once, and the whole lot is what inflate produces. */
    return ((long)width * bytes_per_pixel + 1) * (long)height;
}

/* Paeth's predictor: of the three neighbours, whichever is nearest to what
   their linear combination suggests. Written out rather than shortened,
   because every trick version of this is where the diagonal smears come
   from. */
static int paeth(int left, int above, int upper_left) {
    int estimate = left + above - upper_left;
    int distance_left = estimate - left;
    int distance_above = estimate - above;
    int distance_corner = estimate - upper_left;

    if (distance_left < 0) distance_left = -distance_left;
    if (distance_above < 0) distance_above = -distance_above;
    if (distance_corner < 0) distance_corner = -distance_corner;

    if (distance_left <= distance_above && distance_left <= distance_corner)
        return left;
    if (distance_above <= distance_corner) return above;
    return upper_left;
}

static int unfilter(koi_uint8* rows, int width, int height, int pixel_bytes) {
    long stride = (long)width * pixel_bytes;

    for (int row = 0; row < height; row++) {
        koi_uint8* line = rows + (long)row * (stride + 1);
        int filter = line[0];
        koi_uint8* pixels = line + 1;
        const koi_uint8* above = row ? rows + (long)(row - 1) * (stride + 1) + 1
                                     : (const koi_uint8*)0;

        switch (filter) {
        case 0:
            break;
        case 1:
            for (long at = pixel_bytes; at < stride; at++)
                pixels[at] = (koi_uint8)(pixels[at] + pixels[at - pixel_bytes]);
            break;
        case 2:
            if (!above) break;
            for (long at = 0; at < stride; at++)
                pixels[at] = (koi_uint8)(pixels[at] + above[at]);
            break;
        case 3:
            for (long at = 0; at < stride; at++) {
                int left = at >= pixel_bytes ? pixels[at - pixel_bytes] : 0;
                int up = above ? above[at] : 0;

                pixels[at] = (koi_uint8)(pixels[at] + ((left + up) >> 1));
            }
            break;
        case 4:
            for (long at = 0; at < stride; at++) {
                int left = at >= pixel_bytes ? pixels[at - pixel_bytes] : 0;
                int up = above ? above[at] : 0;
                int corner = (above && at >= pixel_bytes)
                             ? above[at - pixel_bytes] : 0;

                pixels[at] = (koi_uint8)(pixels[at] +
                                         paeth(left, up, corner));
            }
            break;
        default:
            blame("a row of the picture is filtered in a way PNG does not have");
            return 0;
        }
    }
    return 1;
}

static koi_uint32 mix(int colour, int alpha, int background) {
    /* Composited rather than kept: nothing below draws with transparency, so
       the choice has to be made here, once, against a colour the caller
       named. */
    return (koi_uint32)((colour * alpha + background * (255 - alpha)) / 255);
}

int png_decode(const void* file, long length, koi_uint32* pixels,
               long pixel_capacity, koi_uint32 background, PNG* out) {
    const koi_uint8* data = (const koi_uint8*)file;
    long at = 8;
    int width = 0, height = 0, depth = 0, colour_type = 0, interlace = 0;
    int channels, pixel_bytes;
    long compressed_length = 0;
    long expected;
    koi_uint8 palette[MAX_PALETTE * 3];
    koi_uint8 palette_alpha[MAX_PALETTE];
    int palette_count = 0;
    koi_uint8* compressed;
    koi_uint8* rows;
    long produced;
    int any_alpha = 0;

    trouble[0] = 0;
    if (!png_size(file, length, &width, &height)) return 0;

    depth = data[24];
    colour_type = data[25];
    interlace = data[28];
    channels = channels_of(colour_type);

    if (!channels) { blame("the picture has a kind of colour PNG does not have"); return 0; }
    if (depth == 16) {
        blame("this is a 16-bit PNG; only 8 bits a channel are read, and "
              "halving them quietly would be a lie about the picture");
        return 0;
    }
    if (depth != 8 && !((colour_type == 3 || colour_type == 0) &&
                        (depth == 1 || depth == 2 || depth == 4))) {
        blame("only 8 bits a channel are read, a small palette, or a few "
              "shades of grey");
        return 0;
    }
    if (interlace) {
        blame("this PNG is interlaced, and that is a different row order this "
              "does not read yet");
        return 0;
    }
    if ((long)width * height > pixel_capacity) {
        blame("the picture is larger than there is room for");
        return 0;
    }

    for (int index = 0; index < MAX_PALETTE; index++) palette_alpha[index] = 255;

    pixel_bytes = (colour_type == 3 || colour_type == 0) ? 1 : channels;
    expected = ((long)width * pixel_bytes + 1) * height;
    if (depth < 8)
        expected = (((long)width * depth + 7) / 8 + 1) * height;

    /* The scratch is used as two things in turn: first the joined IDAT bytes,
       then the rows they expand into. They are laid end to end rather than
       overlapping, because inflate reads its input while writing its output
       and the two must not be the same memory. */
    if (!scratch || scratch_size < expected + length) {
        blame("there is not enough working memory for this picture");
        return 0;
    }
    compressed = scratch;
    rows = scratch + length;

    while (at + 8 <= length) {
        unsigned long size = be32(data + at);
        const koi_uint8* name = data + at + 4;
        const koi_uint8* body = data + at + 8;

        if (at + 12 + (long)size > length) {
            blame("the file ends in the middle of a chunk");
            return 0;
        }
        if (name[0] == 'P' && name[1] == 'L' && name[2] == 'T' && name[3] == 'E') {
            palette_count = (int)(size / 3);
            if (palette_count > MAX_PALETTE) palette_count = MAX_PALETTE;
            for (int index = 0; index < palette_count * 3; index++)
                palette[index] = body[index];
        } else if (name[0] == 't' && name[1] == 'R' && name[2] == 'N' &&
                   name[3] == 'S') {
            /* Transparency for a palette: one alpha per entry, and entries
               not listed are opaque. This is how a PNG icon has a hole in it
               without carrying a whole alpha channel. */
            for (unsigned long index = 0; index < size && index < MAX_PALETTE;
                 index++)
                palette_alpha[index] = body[index];
        } else if (name[0] == 'I' && name[1] == 'D' && name[2] == 'A' &&
                   name[3] == 'T') {
            if (compressed_length + (long)size > length) {
                blame("the picture's data is longer than the file");
                return 0;
            }
            for (unsigned long index = 0; index < size; index++)
                compressed[compressed_length++] = body[index];
        } else if (name[0] == 'I' && name[1] == 'E' && name[2] == 'N' &&
                   name[3] == 'D') {
            break;
        }
        at += 12 + (long)size;
    }

    if (!compressed_length) { blame("the file has no picture in it"); return 0; }

    produced = inflate_zlib(compressed, compressed_length, rows,
                            scratch_size - length);
    if (produced < 0) { blame(inflate_trouble()); return 0; }
    if (produced < expected) {
        blame("the picture's data stops before the last row");
        return 0;
    }

    if (depth < 8) {
        /* A palette of two, four or sixteen colours packs several pixels into
           a byte. Unpacked here into one byte each, so that everything below
           has one shape to deal with. */
        long packed_stride = ((long)width * depth + 7) / 8;

        if (!unfilter(rows, (int)packed_stride, height, 1)) return 0;
        for (int row = 0; row < height; row++) {
            const koi_uint8* line = rows + (long)row * (packed_stride + 1) + 1;

            for (int column = 0; column < width; column++) {
                int per_byte = 8 / depth;
                int index = line[column / per_byte];
                int shift = 8 - depth * ((column % per_byte) + 1);
                int value = (index >> shift) & ((1 << depth) - 1);
                int red, green, blue, alpha;

                if (colour_type == 0) {
                    /* A few shades of grey, spread over the whole range: two
                       levels are black and white, four are 0, 85, 170, 255.
                       Multiplying by 255/max rather than shifting, because a
                       shift leaves white at 254 and white matters. */
                    int levels = (1 << depth) - 1;

                    red = green = blue = value * 255 / levels;
                    alpha = 255;
                } else {
                    if (value >= palette_count) value = 0;
                    red = palette[value * 3];
                    green = palette[value * 3 + 1];
                    blue = palette[value * 3 + 2];
                    alpha = palette_alpha[value];
                }
                if (alpha != 255) any_alpha = 1;
                pixels[(long)row * width + column] =
                    (mix(red, alpha, (int)((background >> 16) & 0xFF)) << 16) |
                    (mix(green, alpha, (int)((background >> 8) & 0xFF)) << 8) |
                    mix(blue, alpha, (int)(background & 0xFF));
            }
        }
    } else {
        if (!unfilter(rows, width, height, pixel_bytes)) return 0;
        for (int row = 0; row < height; row++) {
            const koi_uint8* line = rows +
                (long)row * ((long)width * pixel_bytes + 1) + 1;

            for (int column = 0; column < width; column++) {
                const koi_uint8* pixel = line + (long)column * pixel_bytes;
                int red, green, blue, alpha = 255;

                switch (colour_type) {
                case 0: red = green = blue = pixel[0]; break;
                case 4: red = green = blue = pixel[0]; alpha = pixel[1]; break;
                case 2: red = pixel[0]; green = pixel[1]; blue = pixel[2]; break;
                case 6:
                    red = pixel[0]; green = pixel[1]; blue = pixel[2];
                    alpha = pixel[3];
                    break;
                default: {
                    int value = pixel[0];

                    if (value >= palette_count) value = 0;
                    red = palette[value * 3];
                    green = palette[value * 3 + 1];
                    blue = palette[value * 3 + 2];
                    alpha = palette_alpha[value];
                    break;
                }
                }
                if (alpha != 255) any_alpha = 1;
                pixels[(long)row * width + column] =
                    (mix(red, alpha, (int)((background >> 16) & 0xFF)) << 16) |
                    (mix(green, alpha, (int)((background >> 8) & 0xFF)) << 8) |
                    mix(blue, alpha, (int)(background & 0xFF));
            }
        }
    }

    out->width = width;
    out->height = height;
    out->pixels = pixels;
    out->has_alpha = any_alpha;
    return 1;
}
