#include "window.h"

/* Save the current desktop gradient as a BMP wallpaper.
 *
 * The desktop already knows how to paint a gradient. This utility turns that
 * same shape into a 24-bit BMP so the desktop can later treat it as a fixed
 * wallpaper file without needing a separate image toolchain. */

#define WALLPAPER_PATH "\\WALLPAPER.BMP"

static void write16(long handle, koi_uint16 value) {
    koi_uint8 out[2];
    out[0] = (koi_uint8)(value & 0xFF);
    out[1] = (koi_uint8)((value >> 8) & 0xFF);
    koi_write(handle, out, 2);
}

static void write32(long handle, koi_uint32 value) {
    koi_uint8 out[4];
    out[0] = (koi_uint8)(value & 0xFF);
    out[1] = (koi_uint8)((value >> 8) & 0xFF);
    out[2] = (koi_uint8)((value >> 16) & 0xFF);
    out[3] = (koi_uint8)((value >> 24) & 0xFF);
    koi_write(handle, out, 4);
}

int main(void) {
    KOI_SCREEN screen;
    long handle;
    int width;
    int height;
    int top = WINDOW_TOPBAR_H;
    int bottom;
    int span;
    koi_uint8 row[4096 * 3];

    if (koi_gfx_enter(&screen) != 0) {
        koi_print("This system has no framebuffer to read.\n");
        return 1;
    }

    width = (int)screen.width;
    height = (int)screen.height;
    bottom = height - WINDOW_TASKBAR_H;
    span = bottom - top;
    if (width <= 0 || span <= 0 || width > 4096) {
        koi_print("Wallpaper size is not supported.\n");
        koi_gfx_leave();
        return 1;
    }

    handle = koi_open(WALLPAPER_PATH, OPEN_WRITE);
    if (handle < 0) {
        koi_print("Could not open ");
        koi_print(WALLPAPER_PATH);
        koi_print(" for writing.\n");
        koi_gfx_leave();
        return 1;
    }

    {
        long row_bytes = width * 3;
        long padded_row = (row_bytes + 3) & ~3L;
        long pad_bytes = padded_row - row_bytes;
        long file_size = 54 + padded_row * span;

        koi_write(handle, "BM", 2);
        write32(handle, (koi_uint32)file_size);
        write16(handle, 0);
        write16(handle, 0);
        write32(handle, 54);
        write32(handle, 40);
        write32(handle, (koi_uint32)width);
        write32(handle, (koi_uint32)span);
        write16(handle, 1);
        write16(handle, 24);
        write32(handle, 0);
        write32(handle, (koi_uint32)(padded_row * span));
        write32(handle, 2835);
        write32(handle, 2835);
        write32(handle, 0);
        write32(handle, 0);

        for (int y = 0; y < span; y++) {
            koi_uint32 color = koi_gfx_color(
                0xD6 + (0x8F - 0xD6) * y / span,
                0xEE + (0xC6 - 0xEE) * y / span,
                0xF5 + (0xDD - 0xF5) * y / span);

            for (int x = 0; x < width; x++) {
                row[x * 3 + 0] = (koi_uint8)(color & 0xFF);
                row[x * 3 + 1] = (koi_uint8)((color >> 8) & 0xFF);
                row[x * 3 + 2] = (koi_uint8)((color >> 16) & 0xFF);
            }
            for (long pad = 0; pad < pad_bytes; pad++)
                row[row_bytes + pad] = 0;
            koi_write(handle, row, padded_row);
        }
    }

    koi_close(handle);
    koi_gfx_leave();
    koi_print("Saved wallpaper to ");
    koi_print(WALLPAPER_PATH);
    koi_print("\n");
    return 0;
}