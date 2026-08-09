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

/* One row out to the adapter.
 *
 * `rep movsq` rather than a loop of stores. A C loop over a volatile pointer
 * is the slowest thing that can be written here: volatile forbids the compiler
 * from merging or reordering anything, so it emits one four-byte store per
 * pixel and a million of them per frame. The string move is a single
 * instruction the processor is allowed to turn into whole cache-line
 * transfers, which is what the write-combining mapping exists to let it do.
 *
 * A trailing odd pixel is copied on its own: rows are almost always an even
 * number of pixels, and "almost" is not a thing to build on. */
static void copy_row(volatile boot_uint32_t* destination,
                     const boot_uint32_t* source, boot_uint32_t pixels) {
    boot_uint64_t quads = pixels / 2;

    if (quads) {
        __asm__ volatile ("rep movsq"
                          : "+D"(destination), "+S"(source), "+c"(quads)
                          : : "memory");
    }
    if (pixels & 1) destination[0] = source[0];
}

void graphics_present(void) {
    if (!active) return;
    graphics_present_rect(0, 0, (int)screen_width, (int)screen_height);
}

void graphics_present_rect(int x, int y, int width, int height) {
    if (!active || width <= 0 || height <= 0) return;

    /* Clip rather than refuse. A caller that knows what it changed should not
       also have to know where the edges are. */
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= (int)screen_width || y >= (int)screen_height) return;
    if (x + width > (int)screen_width) width = (int)screen_width - x;
    if (y + height > (int)screen_height) height = (int)screen_height - y;
    if (width <= 0 || height <= 0) return;

    for (int line = 0; line < height; line++) {
        volatile boot_uint32_t* destination =
            framebuffer + (boot_uint64_t)(y + line) * pixels_per_scan_line + x;
        const boot_uint32_t* source =
            buffer + (boot_uint64_t)(y + line) * screen_width + x;
        copy_row(destination, source, (boot_uint32_t)width);
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

/* A run of one colour, as one instruction.
 *
 * The obvious C loop is one four-byte store per pixel, and a full-screen clear
 * is a million of them - which measured as the most expensive thing in a
 * frame, more than sending the frame to the adapter. `rep stosq` writes eight
 * bytes at a time and is a single instruction the processor is free to widen
 * further. Two pixels of the same colour are one quadword, which is why the
 * colour is doubled up first. */
static void fill_run(boot_uint32_t* pixels, boot_uint32_t color,
                     boot_uint32_t count) {
    boot_uint64_t pair = ((boot_uint64_t)color << 32) | color;
    boot_uint64_t quads = count / 2;

    if (((boot_uint64_t)pixels & 7) && count) {
        /* An odd starting address has no quadword to begin with; place one
           pixel to reach alignment and carry on from there. */
        *pixels++ = color;
        count--;
        quads = count / 2;
    }
    if (quads) {
        boot_uint64_t* out = (boot_uint64_t*)pixels;
        __asm__ volatile ("rep stosq"
                          : "+D"(out), "+c"(quads)
                          : "a"(pair)
                          : "memory");
        pixels = (boot_uint32_t*)out;
    }
    if (count & 1) *pixels = color;
}

void graphics_pixel(int x, int y, boot_uint32_t color) {
    if (!active) return;
    if (x < 0 || y < 0 || (boot_uint32_t)x >= screen_width ||
        (boot_uint32_t)y >= screen_height) return;
    row_of(y)[x] = color;
}

void graphics_clear(boot_uint32_t color) {
    if (!active) return;
    /* The buffer is one contiguous run, so the whole screen is a single fill
       rather than one per row. */
    fill_run(buffer, color, screen_width * screen_height);
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

    for (int line = y; line < bottom; line++)
        fill_run(row_of(line) + x, color, (boot_uint32_t)(right - x));
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

/* Text, optionally emboldened, slanted or underlined.
 *
 * All three are done to the glyph the font already has rather than by keeping
 * a second and a third set of them. That is how bitmap systems have always
 * done it and it is not a shortcut: a bold face drawn by hand is a different
 * design, but a bitmap font at sixteen pixels has no room for one, and the
 * mechanical version is what everybody has actually seen.
 *
 *   bold      the glyph drawn again one pixel to the right, so every stroke
 *             becomes two pixels wide
 *   italic    each row shifted right in proportion to how high it is - a
 *             shear, which is what italic is when there is no second design
 *   underline the row below the baseline filled
 *
 * The slant is by a quarter, which at eight pixels wide leans the top of a
 * glyph two pixels past its foot: enough to read as deliberate and little
 * enough that neighbouring letters do not collide. */
void graphics_text_styled(int x, int y, const char* text, boot_uint32_t color,
                          boot_uint32_t background, int transparent,
                          int style) {
    if (!active || !text) return;

    for (boot_uint64_t index = 0, cell = 0; text[index]; cell++) {
        int step = 1;
        const boot_uint8_t* glyph = font_glyph(font_decode(text + index, &step));
        int origin = x + (int)cell * FONT_WIDTH;

        index += (boot_uint64_t)step;

        for (int line = 0; line < FONT_HEIGHT; line++) {
            boot_uint32_t bits = glyph[line];
            int slant = (style & GRAPHICS_TEXT_ITALIC)
                      ? (FONT_HEIGHT - 1 - line) / 4 : 0;

            if (style & GRAPHICS_TEXT_BOLD) bits |= bits >> 1;
            if ((style & GRAPHICS_TEXT_UNDERLINE) && line == FONT_HEIGHT - 3)
                bits = 0xFF;

            for (int column = 0; column < FONT_WIDTH; column++) {
                int set = (bits & (0x80U >> column)) != 0;
                if (!set && transparent) continue;
                graphics_pixel(origin + column + slant, y + line,
                               set ? color : background);
            }
        }
    }
}

void graphics_text(int x, int y, const char* text, boot_uint32_t color,
                   boot_uint32_t background, int transparent) {
    graphics_text_styled(x, y, text, color, background, transparent, 0);
}
