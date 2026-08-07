#ifndef KERNEL_GRAPHICS_H
#define KERNEL_GRAPHICS_H

#include "../include/bootinfo.h"

/* Drawing on the framebuffer, and the mode a program enters to do it.
 *
 * The console owns the screen the rest of the time. A program that wants to
 * draw asks for it, gets a buffer of its own to work in, and gives it back -
 * at which point the console repaints what was there before. That handover is
 * the whole design: DOS did the same thing with mode 13h, and the reason it
 * worked was that leaving the mode always restored the text screen, however
 * badly the program had behaved while it had the screen.
 *
 * There is no mode switching in the hardware sense. UEFI chose the resolution
 * before ExitBootServices and it cannot be changed afterwards, so "graphics
 * mode" here means the console stops drawing and something else starts. */

typedef struct {
    boot_uint32_t width;
    boot_uint32_t height;
    boot_uint32_t pitch;            /* bytes between the starts of two rows */
    boot_uint32_t bytes_per_pixel;  /* always 4 today, stated rather than assumed */
    void* pixels;                   /* the buffer to draw in, writable */
} GRAPHICS_SCREEN;

void graphics_init(const BOOT_INFO* info);

/* Take the screen. Fills in `screen` and returns 1, or returns 0 when there is
   no framebuffer or the buffer could not be allocated. Entering twice without
   leaving is refused - two owners of one screen is not a state worth having. */
int graphics_enter(GRAPHICS_SCREEN* screen);

/* Push everything drawn so far out to the display. Nothing appears until this
   is called, which is what stops a half-drawn frame from being seen. */
void graphics_present(void);

/* Push one rectangle instead of the whole screen.
 *
 * This is the difference between a game that runs and one that crawls. The
 * screen is whatever size the firmware chose - often far larger than the area
 * a program actually uses - and sending all of it sixty times a second costs
 * more than everything else the program does put together. Coordinates are
 * clipped, so a caller that knows what it changed need not also know where the
 * edges are. */
void graphics_present_rect(int x, int y, int width, int height);

/* Give the screen back and let the console repaint. Safe to call when not in
   graphics mode. */
void graphics_leave(void);

int graphics_active(void);

/* Pack a colour for this framebuffer's channel order. A program must never
   assemble a pixel itself: the byte order differs between machines, and code
   that guesses draws in the wrong colours on half of them. */
boot_uint32_t graphics_color(boot_uint8_t red, boot_uint8_t green,
                             boot_uint8_t blue);

/* Primitives. Coordinates are signed and clipped, so drawing partly off the
   screen is ordinary rather than an error. */
void graphics_clear(boot_uint32_t color);
void graphics_pixel(int x, int y, boot_uint32_t color);
void graphics_line(int x0, int y0, int x1, int y1, boot_uint32_t color);
void graphics_rect(int x, int y, int width, int height, boot_uint32_t color);
void graphics_fill(int x, int y, int width, int height, boot_uint32_t color);

/* Copy a block of pixels in. `stride` is the source's bytes per row, which is
   not always its width - image formats pad. */
void graphics_blit(const void* pixels, int x, int y, int width, int height,
                   boot_uint32_t stride);

/* Text in the console's font, at pixel coordinates rather than character
   cells. A graphics program still needs to label things. */
void graphics_text(int x, int y, const char* text, boot_uint32_t color,
                   boot_uint32_t background, int transparent);

#endif
