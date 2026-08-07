#include "console.h"
#include "font.h"
#include "graphics.h"
#include "memory.h"
#include "string.h"

/* Text console on a linear framebuffer.
 *
 * Everything is drawn into a back buffer in ordinary RAM and then copied to
 * the framebuffer. Scrolling is the reason: it has to move the whole screen up
 * by one line, and reading back from a framebuffer across PCIe is orders of
 * magnitude slower than reading RAM. With a back buffer the move is a plain
 * memmove and only the write direction ever touches the device.
 *
 * If the back buffer cannot be allocated the console falls back to drawing
 * straight into the framebuffer - degraded, but never dark. */

#define PIXEL_FORMAT_RGB 0  /* EfiPixelRedGreenBlueReserved8BitPerColor */
#define PIXEL_FORMAT_BGR 1  /* EfiPixelBlueGreenRedReserved8BitPerColor */

static volatile boot_uint32_t* framebuffer;
static boot_uint32_t* back_buffer;
static boot_uint64_t back_buffer_pages;

static boot_uint32_t screen_width;
static boot_uint32_t screen_height;
static boot_uint32_t pixels_per_scan_line;
static boot_uint32_t pixel_format;

static boot_uint32_t columns;
static boot_uint32_t rows;
static boot_uint32_t cursor_column;
static boot_uint32_t cursor_row;
static int cursor_drawn;

static boot_uint32_t foreground_color;
static boot_uint32_t background_color;

/* The DOS look, and what everything falls back to. */
static CONSOLE_THEME theme = {
    COLOR_LIGHT_GRAY, COLOR_BLUE, COLOR_LIGHT_GREEN, COLOR_LIGHT_RED
};

/* Palette as 8-bit RGB triples; converted to the hardware's channel order at
   init, so the rest of the file never thinks about pixel format again. */
static const boot_uint8_t palette_rgb[16][3] = {
    {   0,   0,   0 }, {   0,   0, 170 }, {   0, 170,   0 }, {   0, 170, 170 },
    { 170,   0,   0 }, { 170,   0, 170 }, { 170,  85,   0 }, { 170, 170, 170 },
    {  85,  85,  85 }, {  85,  85, 255 }, {  85, 255,  85 }, {  85, 255, 255 },
    { 255,  85,  85 }, { 255,  85, 255 }, { 255, 255,  85 }, { 255, 255, 255 }
};

static boot_uint32_t palette[16];

static boot_uint32_t encode_color(boot_uint8_t red, boot_uint8_t green,
                                  boot_uint8_t blue) {
    /* The UEFI names describe BYTE order, not the value of the 32-bit word.
       PixelBlueGreenRedReserved means the bytes run B,G,R,X, which on a
       little-endian machine reads back as the familiar 0x00RRGGBB - that is
       the layout QEMU and most firmware report. PixelRedGreenBlueReserved is
       the mirror image. Getting this backwards swaps red and blue. */
    if (pixel_format == PIXEL_FORMAT_BGR)
        return ((boot_uint32_t)red << 16) | ((boot_uint32_t)green << 8) | blue;
    return ((boot_uint32_t)blue << 16) | ((boot_uint32_t)green << 8) | red;
}

/* Where drawing goes: the back buffer when we have one, else the device. */
static boot_uint32_t* target_row(boot_uint32_t y) {
    if (back_buffer) return back_buffer + (boot_uint64_t)y * screen_width;
    return (boot_uint32_t*)(framebuffer + (boot_uint64_t)y * pixels_per_scan_line);
}

/* Copy a rectangle out to the device. Kept tight on purpose: pushing a whole
 * scanline for every character would move 160 times more pixels than the cell
 * that actually changed.
 *
 * Nothing goes out while a program owns the screen. The console keeps writing
 * to its back buffer throughout - so text printed during graphics mode is not
 * lost, it simply appears when the screen comes back - but the display belongs
 * to whoever entered graphics mode until they leave. Without this the caret
 * alone would blink a hole in every picture, because waiting for a keypress is
 * what a graphics program does last. */
static void present(boot_uint32_t x, boot_uint32_t y,
                    boot_uint32_t width, boot_uint32_t height) {
    if (graphics_active()) return;
    if (!back_buffer || x >= screen_width || y >= screen_height) return;
    if (x + width > screen_width) width = screen_width - x;
    if (y + height > screen_height) height = screen_height - y;
    for (boot_uint32_t line = y; line < y + height; line++) {
        volatile boot_uint32_t* destination =
            framebuffer + (boot_uint64_t)line * pixels_per_scan_line + x;
        const boot_uint32_t* source =
            back_buffer + (boot_uint64_t)line * screen_width + x;
        for (boot_uint32_t offset = 0; offset < width; offset++)
            destination[offset] = source[offset];
    }
}

static void fill_rows(boot_uint32_t first_row, boot_uint32_t row_count,
                      boot_uint32_t color) {
    for (boot_uint32_t y = first_row; y < first_row + row_count; y++) {
        boot_uint32_t* line = target_row(y);
        for (boot_uint32_t x = 0; x < screen_width; x++) line[x] = color;
    }
}

void console_init(const BOOT_INFO* info) {
    framebuffer = (volatile boot_uint32_t*)(unsigned long long)info->framebuffer_base;
    screen_width = info->framebuffer_width;
    screen_height = info->framebuffer_height;
    pixels_per_scan_line = info->framebuffer_pixels_per_scan_line;
    pixel_format = info->framebuffer_pixel_format;

    for (int index = 0; index < 16; index++)
        palette[index] = encode_color(palette_rgb[index][0], palette_rgb[index][1],
                                      palette_rgb[index][2]);

    columns = screen_width / FONT_WIDTH;
    rows = screen_height / FONT_HEIGHT;
    cursor_column = 0;
    cursor_row = 0;
    cursor_drawn = 0;
    foreground_color = palette[COLOR_LIGHT_GRAY];
    background_color = palette[COLOR_BLACK];

    /* One 32-bit pixel per screen position. Rounded up to whole pages. */
    back_buffer_pages = ((boot_uint64_t)screen_width * screen_height * 4
                         + PAGE_SIZE - 1) / PAGE_SIZE;
    back_buffer = (boot_uint32_t*)alloc_pages(back_buffer_pages);
    if (!back_buffer) back_buffer_pages = 0;
}

void console_set_theme(const CONSOLE_THEME* replacement) {
    if (replacement) theme = *replacement;
}

const CONSOLE_THEME* console_theme(void) {
    return &theme;
}

void console_use_theme(void) {
    console_set_color(theme.foreground, theme.background);
}

void console_set_color(boot_uint8_t foreground, boot_uint8_t background) {
    foreground_color = palette[foreground & 0x0F];
    background_color = palette[background & 0x0F];
}

void console_clear(void) {
    fill_rows(0, screen_height, background_color);
    present(0, 0, screen_width, screen_height);
    cursor_column = 0;
    cursor_row = 0;
    cursor_drawn = 0;
}

void console_redraw(void) {
    /* Without a back buffer there is nothing held to redraw from: the console
       has been writing straight to the device all along, and whatever covered
       it took the only copy. Saying so beats painting the screen black. */
    if (!back_buffer) {
        console_write("\n[the screen could not be restored]\n");
        return;
    }
    present(0, 0, screen_width, screen_height);
}

static void draw_glyph(boot_uint32_t column, boot_uint32_t row,
                       unsigned char code) {
    const boot_uint8_t* glyph = font_8x16[code];
    boot_uint32_t origin_x = column * FONT_WIDTH;
    boot_uint32_t origin_y = row * FONT_HEIGHT;

    for (boot_uint32_t y = 0; y < FONT_HEIGHT; y++) {
        boot_uint32_t* line = target_row(origin_y + y) + origin_x;
        boot_uint8_t bits = glyph[y];
        for (boot_uint32_t x = 0; x < FONT_WIDTH; x++)
            line[x] = (bits & (0x80U >> x)) ? foreground_color : background_color;
    }
    present(origin_x, origin_y, FONT_WIDTH, FONT_HEIGHT);
}

static void invert_cell(boot_uint32_t column, boot_uint32_t row) {
    boot_uint32_t origin_x = column * FONT_WIDTH;
    boot_uint32_t origin_y = row * FONT_HEIGHT;

    /* The caret is an underline on the bottom two scanlines, the way a text
       mode cursor looks, rather than a full inverted block. */
    for (boot_uint32_t y = FONT_HEIGHT - 2; y < FONT_HEIGHT; y++) {
        boot_uint32_t* line = target_row(origin_y + y) + origin_x;
        for (boot_uint32_t x = 0; x < FONT_WIDTH; x++)
            line[x] = line[x] == foreground_color ? background_color : foreground_color;
    }
    present(origin_x, origin_y + FONT_HEIGHT - 2, FONT_WIDTH, 2);
}

void console_show_cursor(int visible) {
    if (visible == cursor_drawn) return;
    invert_cell(cursor_column, cursor_row);
    cursor_drawn = visible;
}

static void hide_cursor_for_edit(void) {
    if (cursor_drawn) {
        invert_cell(cursor_column, cursor_row);
        cursor_drawn = 0;
    }
}

static void scroll(void) {
    if (back_buffer) {
        boot_uint64_t line_pixels = (boot_uint64_t)screen_width * FONT_HEIGHT;
        boot_uint64_t kept = (boot_uint64_t)screen_width * (rows - 1) * FONT_HEIGHT;
        memmove(back_buffer, back_buffer + line_pixels, kept * sizeof(boot_uint32_t));
        fill_rows((rows - 1) * FONT_HEIGHT, FONT_HEIGHT, background_color);
        present(0, 0, screen_width, rows * FONT_HEIGHT);
    } else {
        /* No back buffer: read-modify-write straight on the device. Slow, but
           this path only exists when memory ran out. */
        for (boot_uint32_t y = 0; y < (rows - 1) * FONT_HEIGHT; y++) {
            volatile boot_uint32_t* destination =
                framebuffer + (boot_uint64_t)y * pixels_per_scan_line;
            volatile boot_uint32_t* source =
                framebuffer + (boot_uint64_t)(y + FONT_HEIGHT) * pixels_per_scan_line;
            for (boot_uint32_t x = 0; x < screen_width; x++) destination[x] = source[x];
        }
        fill_rows((rows - 1) * FONT_HEIGHT, FONT_HEIGHT, background_color);
    }
    cursor_row = rows - 1;
}

static void newline(void) {
    cursor_column = 0;
    if (++cursor_row >= rows) scroll();
}

void console_putchar(char character) {
    unsigned char code = (unsigned char)character;

    hide_cursor_for_edit();

    switch (code) {
    case '\n':
        newline();
        return;
    case '\r':
        cursor_column = 0;
        return;
    case '\t':
        do { console_putchar(' '); } while (cursor_column & 7);
        return;
    case '\b':
        if (cursor_column) cursor_column--;
        else if (cursor_row) { cursor_row--; cursor_column = columns - 1; }
        draw_glyph(cursor_column, cursor_row, ' ');
        return;
    default:
        break;
    }

    if (cursor_column >= columns) newline();
    draw_glyph(cursor_column, cursor_row, code);
    cursor_column++;
}

void console_write(const char* text) {
    while (*text) console_putchar(*text++);
}

void console_write_hex(boot_uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 60; shift >= 0; shift -= 4)
        console_putchar(digits[(value >> shift) & 0xFU]);
}

void console_write_dec(boot_uint64_t value) {
    char buffer[21];
    int index = 20;

    buffer[index] = 0;
    do {
        buffer[--index] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value);
    console_write(&buffer[index]);
}

void console_set_cursor(boot_uint32_t column, boot_uint32_t row) {
    hide_cursor_for_edit();
    if (column < columns) cursor_column = column;
    if (row < rows) cursor_row = row;
}

boot_uint32_t console_column(void) { return cursor_column; }
boot_uint32_t console_row(void) { return cursor_row; }
boot_uint32_t console_width(void) { return screen_width; }
boot_uint32_t console_height(void) { return screen_height; }
boot_uint32_t console_columns(void) { return columns; }
boot_uint32_t console_rows(void) { return rows; }
