#include "koi.h"

/* Everything the graphics layer can draw, on one screen.
 *
 * Written as a test that happens to look like something, rather than a demo
 * that happens to test: each block below exercises one primitive, and a
 * primitive that is wrong is wrong in a way that is visible from across the
 * room. The gradient catches a channel order read backwards, the clipped
 * shapes catch a missing bounds check, and the diagonals catch Bresenham
 * stepping the wrong axis. */

static void bar(int x, int y, int width, int height, int red, int green,
                int blue) {
    koi_gfx_fill(x, y, width, height, koi_gfx_color(red, green, blue));
}

int main(const char* arguments) {
    KOI_SCREEN screen;
    koi_uint32 white;
    koi_uint32 black;
    int width;
    int height;

    (void)arguments;

    if (koi_gfx_enter(&screen) != 0) {
        koi_print("This system has no framebuffer to draw on.\n");
        return 1;
    }
    width = (int)screen.width;
    height = (int)screen.height;
    white = koi_gfx_color(255, 255, 255);
    black = koi_gfx_color(0, 0, 0);

    koi_gfx_clear(koi_gfx_color(0, 0, 40));

    /* A gradient across the top, one column at a time. Red on the left, green
       in the middle, blue on the right: if the framebuffer's channel order
       were being guessed rather than asked for, this is where it shows. */
    for (int x = 0; x < width; x++) {
        int position = width > 1 ? x * 255 / (width - 1) : 0;
        int red = 255 - position;
        int blue = position;
        int green = position < 128 ? position * 2 : (255 - position) * 2;
        koi_gfx_fill(x, 0, 1, 60, koi_gfx_color(red, green, blue));
    }

    /* The sixteen console colours, as solid blocks. */
    {
        static const unsigned char shades[16][3] = {
            {   0,   0,   0 }, {   0,   0, 170 }, {   0, 170,   0 },
            {   0, 170, 170 }, { 170,   0,   0 }, { 170,   0, 170 },
            { 170,  85,   0 }, { 170, 170, 170 }, {  85,  85,  85 },
            {  85,  85, 255 }, {  85, 255,  85 }, {  85, 255, 255 },
            { 255,  85,  85 }, { 255,  85, 255 }, { 255, 255,  85 },
            { 255, 255, 255 }
        };
        int block = width / 16;
        for (int index = 0; index < 16; index++)
            bar(index * block, 70, block - 2, 40, shades[index][0],
                shades[index][1], shades[index][2]);
    }

    /* Outlines and fills, one inside the other, so a rectangle drawn one pixel
       out is obvious rather than arguable. */
    koi_gfx_rect(40, 140, 200, 120, white);
    koi_gfx_fill(50, 150, 180, 100, koi_gfx_color(200, 40, 40));
    koi_gfx_rect(60, 160, 160, 80, black);

    /* A fan of lines from one corner: every slope, both axes leading. */
    for (int step = 0; step <= 16; step++) {
        koi_gfx_line(300, 140, 300 + step * 20, 260, koi_gfx_color(80, 220, 255));
        koi_gfx_line(300, 140, 300 + 320, 140 + step * 8,
                     koi_gfx_color(255, 220, 80));
    }

    /* Deliberately off the edge, in all four directions. Nothing here should
       corrupt anything or refuse to draw the part that does fit. */
    koi_gfx_fill(-40, 300, 120, 60, koi_gfx_color(0, 200, 120));
    koi_gfx_fill(width - 80, 300, 120, 60, koi_gfx_color(0, 200, 120));
    koi_gfx_fill(120, -30, 60, 120, koi_gfx_color(0, 200, 120));
    koi_gfx_fill(200, height - 60, 60, 120, koi_gfx_color(0, 200, 120));
    koi_gfx_line(-100, 380, width + 100, 420, white);

    /* Straight into the buffer, which is the whole point of being handed a
       pointer: a checkerboard written without a system call per pixel. */
    {
        koi_uint8* pixels = (koi_uint8*)screen.pixels;
        koi_uint32 shade = koi_gfx_color(255, 255, 255);
        for (int y = 300; y < 380 && y < height; y++) {
            koi_uint32* row = (koi_uint32*)(pixels + (koi_uint64)y * screen.pitch);
            for (int x = 400; x < 560 && x < width; x++)
                if (((x / 8) + (y / 8)) & 1) row[x] = shade;
        }
    }

    koi_gfx_text(40, 400, "Koi-DOS graphics", white, KOI_TEXT_TRANSPARENT);
    koi_gfx_text(40, 420, "press any key to return to the shell", white,
                 koi_gfx_color(0, 0, 40));

    koi_gfx_present();
    (void)koi_getchar();
    koi_gfx_leave();

    koi_print("Back in text mode.\n");
    return 0;
}
