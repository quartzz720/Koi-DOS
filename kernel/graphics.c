#include "graphics.h"
#include "console.h"
#include "font.h"
#include "memory.h"
#include "string.h"

/* The same two format codes the console works from. They describe BYTE order,
   not the value of the 32-bit word - see the note in console.c, which is where
   this was got wrong first. */
#define PIXEL_FORMAT_RGB 0
#define PIXEL_FORMAT_BGR 1

static volatile boot_uint32_t* framebuffer;
static boot_uint32_t screen_width;
static boot_uint32_t screen_height;
static boot_uint32_t pixels_per_scan_line;
static boot_uint32_t pixel_format;

static boot_uint32_t* buffer;
static boot_uint64_t buffer_pages;
static int active;

void graphics_init(const BOOT_INFO* info) {
    framebuffer = (volatile boot_uint32_t*)(unsigned long long)info->framebuffer_base;
    screen_width = info->framebuffer_width;
    screen_height = info->framebuffer_height;
    pixels_per_scan_line = info->framebuffer_pixels_per_scan_line;
    pixel_format = info->framebuffer_pixel_format;
}

boot_uint32_t graphics_color(boot_uint8_t red, boot_uint8_t green,
                             boot_uint8_t blue) {
    if (pixel_format == PIXEL_FORMAT_BGR)
        return ((boot_uint32_t)red << 16) | ((boot_uint32_t)green << 8) | blue;
    return ((boot_uint32_t)blue << 16) | ((boot_uint32_t)green << 8) | red;
}

int graphics_active(void) {
    return active;
}

int graphics_enter(GRAPHICS_SCREEN* screen) {
    if (!screen || active || !framebuffer || !screen_width || !screen_height)
        return 0;

    buffer_pages = ((boot_uint64_t)screen_width * screen_height * 4
                    + PAGE_SIZE - 1) / PAGE_SIZE;
    buffer = (boot_uint32_t*)alloc_pages(buffer_pages);
    if (!buffer) {
        buffer_pages = 0;
        return 0;
    }
    /* Black rather than whatever the pages last held. A program that draws
       only part of the screen should not be shown someone else's memory. */
    memset(buffer, 0, buffer_pages * PAGE_SIZE);

    screen->width = screen_width;
    screen->height = screen_height;
    screen->pitch = screen_width * 4;
    screen->bytes_per_pixel = 4;
    screen->pixels = buffer;
    active = 1;
    return 1;
}

void graphics_present(void) {
    if (!active) return;
    for (boot_uint32_t y = 0; y < screen_height; y++) {
        volatile boot_uint32_t* destination =
            framebuffer + (boot_uint64_t)y * pixels_per_scan_line;
        const boot_uint32_t* source = buffer + (boot_uint64_t)y * screen_width;
        for (boot_uint32_t x = 0; x < screen_width; x++)
            destination[x] = source[x];
    }
}

void graphics_leave(void) {
    if (!active) return;
    active = 0;
    free_pages(buffer, buffer_pages);
    buffer = (boot_uint32_t*)0;
    buffer_pages = 0;
    /* The console still holds everything that was on screen, so giving it back
       is a repaint rather than a redraw - nothing was lost while it was
       hidden, and the shell does not have to know it ever happened. */
    console_redraw();
}

/* ---- Primitives ---------------------------------------------------------
 *
 * Every one of these clips. Drawing off the edge is what a program does while
 * it is being written, and a driver that treats it as an error either refuses
 * to draw anything or writes past the buffer. Neither is useful.
 */

static boot_uint32_t* row_of(int y) {
    return buffer + (boot_uint64_t)y * screen_width;
}

void graphics_pixel(int x, int y, boot_uint32_t color) {
    if (!active) return;
    if (x < 0 || y < 0 || (boot_uint32_t)x >= screen_width ||
        (boot_uint32_t)y >= screen_height) return;
    row_of(y)[x] = color;
}

void graphics_clear(boot_uint32_t color) {
    if (!active) return;
    for (boot_uint32_t y = 0; y < screen_height; y++) {
        boot_uint32_t* line = row_of((int)y);
        for (boot_uint32_t x = 0; x < screen_width; x++) line[x] = color;
    }
}

void graphics_fill(int x, int y, int width, int height, boot_uint32_t color) {
    int right;
    int bottom;

    if (!active || width <= 0 || height <= 0) return;
    right = x + width;
    bottom = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > (int)screen_width) right = (int)screen_width;
    if (bottom > (int)screen_height) bottom = (int)screen_height;

    for (int line = y; line < bottom; line++) {
        boot_uint32_t* pixels = row_of(line);
        for (int column = x; column < right; column++) pixels[column] = color;
    }
}

void graphics_rect(int x, int y, int width, int height, boot_uint32_t color) {
    if (!active || width <= 0 || height <= 0) return;
    graphics_fill(x, y, width, 1, color);
    graphics_fill(x, y + height - 1, width, 1, color);
    graphics_fill(x, y, 1, height, color);
    graphics_fill(x + width - 1, y, 1, height, color);
}

/* Bresenham, in the integer-only form. No division, no floating point - which
   matters here because the kernel is built with -mgeneral-regs-only and has no
   floating point registers to spare. */
void graphics_line(int x0, int y0, int x1, int y1, boot_uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int step_x = x0 < x1 ? 1 : -1;
    int step_y = y0 < y1 ? 1 : -1;
    int error;

    if (!active) return;
    dy = -dy;
    error = dx + dy;

    for (;;) {
        graphics_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        {
            int doubled = error * 2;
            if (doubled >= dy) {
                if (x0 == x1) break;
                error += dy;
                x0 += step_x;
            }
            if (doubled <= dx) {
                if (y0 == y1) break;
                error += dx;
                y0 += step_y;
            }
        }
    }
}

void graphics_blit(const void* pixels, int x, int y, int width, int height,
                   boot_uint32_t stride) {
    const boot_uint8_t* source = (const boot_uint8_t*)pixels;

    if (!active || !pixels || width <= 0 || height <= 0) return;

    for (int line = 0; line < height; line++) {
        const boot_uint32_t* input =
            (const boot_uint32_t*)(source + (boot_uint64_t)line * stride);
        int target_y = y + line;

        if (target_y < 0 || (boot_uint32_t)target_y >= screen_height) continue;
        {
            boot_uint32_t* output = row_of(target_y);
            for (int column = 0; column < width; column++) {
                int target_x = x + column;
                if (target_x < 0 || (boot_uint32_t)target_x >= screen_width)
                    continue;
                output[target_x] = input[column];
            }
        }
    }
}

void graphics_text(int x, int y, const char* text, boot_uint32_t color,
                   boot_uint32_t background, int transparent) {
    if (!active || !text) return;

    for (boot_uint64_t index = 0; text[index]; index++) {
        const boot_uint8_t* glyph = font_8x16[(unsigned char)text[index]];
        int origin = x + (int)index * FONT_WIDTH;

        for (int line = 0; line < FONT_HEIGHT; line++) {
            boot_uint8_t bits = glyph[line];
            for (int column = 0; column < FONT_WIDTH; column++) {
                int set = (bits & (0x80U >> column)) != 0;
                if (!set && transparent) continue;
                graphics_pixel(origin + column, y + line,
                               set ? color : background);
            }
        }
    }
}
