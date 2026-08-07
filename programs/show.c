#include "koi.h"

/* Show a BMP file.
 *
 * BMP because it is the one image format a system can read without a decoder:
 * no compression to undo, no entropy coding, no tables. PNG needs inflate and
 * JPEG needs a discrete cosine transform, and neither belongs in the first
 * thing that puts a picture on the screen.
 *
 * Only the uncompressed forms are read - 24 and 32 bits per pixel, BI_RGB.
 * Everything else says so and stops, because an image drawn from a format
 * that was not understood is worse than no image.
 *
 * The file is read forwards, once, without seeking: there is no seek call, and
 * a BMP does not need one. Rows are stored bottom-up, so the row that arrives
 * first is the one at the bottom of the picture. */

#define MAX_WIDTH 4096
#define HEADER_SIZE 54          /* file header 14 + the smallest info header 40 */
#define MAX_EXTRA_HEADER 128    /* the largest header BMP defines is 124 bytes */

/* BI_RGB means the pixels are simply there. BI_BITFIELDS means they are also
   simply there, with the channel positions written down instead of assumed -
   which is what almost every tool emits for 32-bit output, so refusing it
   would make 32-bit support theoretical. */
#define BMP_RGB 0
#define BMP_BITFIELDS 3

/* The layout BI_BITFIELDS almost always describes: blue, green, red, unused,
   in ascending byte order. Anything else is rejected rather than guessed at. */
#define MASK_RED 0x00FF0000U
#define MASK_GREEN 0x0000FF00U
#define MASK_BLUE 0x000000FFU

static koi_uint8 row_bytes[MAX_WIDTH * 4];
static koi_uint32 row_pixels[MAX_WIDTH];
static koi_uint8 extra_header[MAX_EXTRA_HEADER];
static koi_uint8 skip[512];

static koi_uint32 read32(const koi_uint8* data) {
    return (koi_uint32)data[0] | ((koi_uint32)data[1] << 8) |
           ((koi_uint32)data[2] << 16) | ((koi_uint32)data[3] << 24);
}

static koi_uint16 read16(const koi_uint8* data) {
    return (koi_uint16)(data[0] | (data[1] << 8));
}

static void print_number(long value) {
    char buffer[24];
    int length = 0;

    if (value < 0) { koi_putchar('-'); value = -value; }
    do { buffer[length++] = (char)('0' + (value % 10)); value /= 10; }
    while (value);
    while (length--) koi_putchar(buffer[length]);
}

/* Read exactly `length` bytes, or report that the file ended early. */
static int read_exactly(long handle, void* buffer, long length) {
    koi_uint8* output = (koi_uint8*)buffer;
    long done = 0;

    while (done < length) {
        long got = koi_read(handle, output + done, length - done);
        if (got <= 0) return 0;
        done += got;
    }
    return 1;
}

static int skip_forward(long handle, long count) {
    while (count > 0) {
        long chunk = count < (long)sizeof(skip) ? count : (long)sizeof(skip);
        if (!read_exactly(handle, skip, chunk)) return 0;
        count -= chunk;
    }
    return 1;
}

int main(const char* arguments) {
    KOI_SCREEN screen;
    koi_uint8 header[HEADER_SIZE];
    long handle;
    koi_uint32 data_offset;
    koi_uint32 info_size;
    long image_width;
    long image_height;
    koi_uint16 depth;
    koi_uint32 compression;
    koi_uint32 red_mask = 0;
    koi_uint32 green_mask = 0;
    koi_uint32 blue_mask = 0;
    long consumed = HEADER_SIZE;
    long bytes_per_row;
    long padded_row;
    int upside_down = 0;
    int origin_x;
    int origin_y;

    if (!arguments || !arguments[0]) {
        koi_print("show <file.bmp>\n\n");
        koi_print("Displays an uncompressed 24- or 32-bit BMP, centred.\n");
        koi_print("Press any key to return to the shell.\n");
        return 1;
    }

    handle = koi_open(arguments, OPEN_READ);
    if (handle < 0) {
        koi_print("Cannot open ");
        koi_print(arguments);
        koi_print("\n");
        return 1;
    }

    if (!read_exactly(handle, header, HEADER_SIZE)) {
        koi_print("That file is too short to be a BMP.\n");
        koi_close(handle);
        return 1;
    }
    if (header[0] != 'B' || header[1] != 'M') {
        koi_print("That is not a BMP - the first two bytes are not \"BM\".\n");
        koi_close(handle);
        return 1;
    }

    data_offset = read32(header + 10);
    info_size = read32(header + 14);
    image_width = (long)(int)read32(header + 18);
    image_height = (long)(int)read32(header + 22);
    depth = read16(header + 28);
    compression = read32(header + 30);

    /* A negative height is the format's way of saying the rows are stored top
       down instead of the usual bottom up. Both appear in the wild. */
    if (image_height < 0) { image_height = -image_height; upside_down = 1; }

    /* Anything past the 40 bytes already read: the rest of a V4 or V5 header,
       or - with a plain header and BI_BITFIELDS - the three channel masks that
       follow it. Both cases put the masks first, so both are read the same
       way. `consumed` tracks the position, because there is no seek. */
    if (info_size > 40) {
        long extra = (long)info_size - 40;
        if (extra > MAX_EXTRA_HEADER) {
            koi_print("That BMP has a header this does not know.\n");
            koi_close(handle);
            return 1;
        }
        if (!read_exactly(handle, extra_header, extra)) {
            koi_print("That file ends inside its own header.\n");
            koi_close(handle);
            return 1;
        }
        consumed += extra;
        if (extra >= 12) {
            red_mask = read32(extra_header);
            green_mask = read32(extra_header + 4);
            blue_mask = read32(extra_header + 8);
        }
    } else if (compression == BMP_BITFIELDS) {
        if (!read_exactly(handle, extra_header, 12)) {
            koi_print("That file ends before its channel masks.\n");
            koi_close(handle);
            return 1;
        }
        consumed += 12;
        red_mask = read32(extra_header);
        green_mask = read32(extra_header + 4);
        blue_mask = read32(extra_header + 8);
    }

    if (compression == BMP_BITFIELDS) {
        if (depth != 32 || red_mask != MASK_RED || green_mask != MASK_GREEN ||
            blue_mask != MASK_BLUE) {
            koi_print("That BMP arranges its colours in a way this does not\n");
            koi_print("understand. Save it as a 24-bit BMP instead.\n");
            koi_close(handle);
            return 1;
        }
    } else if (compression != BMP_RGB) {
        koi_print("That BMP is compressed, which this cannot read.\n");
        koi_close(handle);
        return 1;
    }
    if (depth != 24 && depth != 32) {
        koi_print("Only 24- and 32-bit BMPs are supported; this one is ");
        print_number(depth);
        koi_print("-bit.\n");
        koi_close(handle);
        return 1;
    }
    if (image_width <= 0 || image_width > MAX_WIDTH || image_height <= 0) {
        koi_print("That image is too wide or has no size.\n");
        koi_close(handle);
        return 1;
    }
    if (info_size < 40 || data_offset < 14 + info_size) {
        koi_print("That BMP's headers do not make sense.\n");
        koi_close(handle);
        return 1;
    }

    /* Rows are padded out to a multiple of four bytes. */
    bytes_per_row = image_width * (depth / 8);
    padded_row = (bytes_per_row + 3) & ~3L;

    if (!skip_forward(handle, (long)data_offset - consumed)) {
        koi_print("That file ends before its pixels begin.\n");
        koi_close(handle);
        return 1;
    }

    if (koi_gfx_enter(&screen) != 0) {
        koi_print("This system has no framebuffer to draw on.\n");
        koi_close(handle);
        return 1;
    }

    koi_gfx_clear(koi_gfx_color(0, 0, 0));
    origin_x = ((int)screen.width - (int)image_width) / 2;
    origin_y = ((int)screen.height - (int)image_height) / 2;

    for (long line = 0; line < image_height; line++) {
        long target = upside_down ? line : image_height - 1 - line;

        if (!read_exactly(handle, row_bytes, padded_row)) break;

        /* BMP stores blue, green, red in that order regardless of what the
           display wants, so every pixel goes through koi_gfx_color rather
           than being copied across. */
        for (long x = 0; x < image_width; x++) {
            const koi_uint8* pixel = row_bytes + x * (depth / 8);
            row_pixels[x] = koi_gfx_color(pixel[2], pixel[1], pixel[0]);
        }

        {
            int y = origin_y + (int)target;
            if (y < 0 || y >= (int)screen.height) continue;
            {
                koi_uint8* base = (koi_uint8*)screen.pixels;
                koi_uint32* output =
                    (koi_uint32*)(base + (koi_uint64)y * screen.pitch);
                for (long x = 0; x < image_width; x++) {
                    int target_x = origin_x + (int)x;
                    if (target_x < 0 || target_x >= (int)screen.width) continue;
                    output[target_x] = row_pixels[x];
                }
            }
        }
    }
    koi_close(handle);

    koi_gfx_present();
    (void)koi_getchar();
    koi_gfx_leave();
    return 0;
}
