#include "window.h"

koi_uint32 window_face;
koi_uint32 window_light;
koi_uint32 window_shadow;
koi_uint32 window_text;
koi_uint32 window_client_paper;
koi_uint32 window_title_active;
koi_uint32 window_title_idle;
koi_uint32 window_accent;

static koi_uint32 desktop_top;
static koi_uint32 desktop_bottom;
static koi_uint32* desktop_wallpaper;
static int desktop_wallpaper_width;
static int desktop_wallpaper_height;

static KOI_SCREEN screen;
static WINDOW windows[WINDOW_MAX];
/* Bottom to top. The last one is the active one, which is what makes "raise"
   and "activate" the same operation and stops them ever disagreeing. */
static WINDOW* order[WINDOW_MAX];
static int order_count;

static WINDOW_MENU desktop_menus[WINDOW_MENU_MAX];
static int desktop_menu_count;

static char desktop_title[WINDOW_TITLE_MAX];

/* One button at the left of the taskbar, if somebody asks for one.
 *
 * Deliberately not "the Start button". A windowing library that knew about
 * Start menus would be a library with a shell's opinions in it, and every
 * program that opens a desktop for two windows would inherit them. What lives
 * here is the mechanism - a button on a bar this file owns, drawn pressed
 * while its menu is open, and a popup that has to appear above every window
 * because only this file knows the drawing order and the pointer.
 *
 * What the button is called, what is in its menu and what choosing an entry
 * does are the caller's, and Mizu is the caller that makes it a Start menu. */
static char launcher_label[24];
static int launcher_pressed;
static int running;
static int dirty = 1;

/* An open drop-down: which bar it belongs to (0 the desktop, else a window),
   which menu, and where it was drawn so a click can be tested against it. */
static WINDOW* menu_owner;
static WINDOW* chosen_owner;
static int menu_open = -1;
static int menu_x, menu_y, menu_w, menu_h;

/* ---- The pointer ---------------------------------------------------------
 *
 * Drawn by hand over whatever is underneath, with the covered pixels kept so
 * they can be put back. Redrawing the world every time it moves a pixel is
 * what makes a pointer feel dragged through sand.
 */
#define CURSOR_W 12
#define CURSOR_H 19

static const char* cursor_shape[CURSOR_H] = {
    "o           ", "oo          ", "o*o         ", "o**o        ",
    "o***o       ", "o****o      ", "o*****o     ", "o******o    ",
    "o*******o   ", "o********o  ", "o*********o ", "o*****ooooo ",
    "o**o**o     ", "o*o o**o    ", "oo  o**o    ", "o    o**o   ",
    "     o**o   ", "      o*o   ", "       o    "
};

static koi_uint32 cursor_under[CURSOR_H][CURSOR_W];
static int cursor_x = -1, cursor_y = -1, cursor_saved;
static koi_uint32 cursor_ink, cursor_edge;

static koi_uint32* pixel_row(int y) {
    return (koi_uint32*)((koi_uint8*)screen.pixels + (koi_uint64)y * screen.pitch);
}

static koi_uint32 read32(const koi_uint8* data) {
    return (koi_uint32)data[0] | ((koi_uint32)data[1] << 8) |
           ((koi_uint32)data[2] << 16) | ((koi_uint32)data[3] << 24);
}

static koi_uint16 read16(const koi_uint8* data) {
    return (koi_uint16)(data[0] | (data[1] << 8));
}

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

static void free_wallpaper(void) {
    if (desktop_wallpaper) koi_free(desktop_wallpaper);
    desktop_wallpaper = (koi_uint32*)0;
    desktop_wallpaper_width = 0;
    desktop_wallpaper_height = 0;
}

static int load_wallpaper(void) {
    enum { HEADER_SIZE = 54, MAX_EXTRA_HEADER = 128 };
    static const koi_uint32 MASK_RED = 0x00FF0000U;
    static const koi_uint32 MASK_GREEN = 0x0000FF00U;
    static const koi_uint32 MASK_BLUE = 0x000000FFU;
    koi_uint8 header[HEADER_SIZE];
    koi_uint8 extra_header[MAX_EXTRA_HEADER];
    koi_uint8 row_bytes[4096 * 4];
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
    long target_height;

    free_wallpaper();
    /* Beside the program, not at a written-down address.
     *
     * This said "\MIZU\WALLPAPER.BMP", which was true of every machine until
     * the day somebody installed the package somewhere else - and dosget
     * chooses that directory, not this file. Asking where the program was
     * loaded from costs one call and is right wherever it ends up. */
    {
        char path[128];

        if (!koi_beside("WALLPAPER.BMP", path, sizeof(path))) return 0;
        handle = koi_open(path, OPEN_READ);
    }
    if (handle < 0) return 0;
    if (!read_exactly(handle, header, HEADER_SIZE)) { koi_close(handle); return 0; }
    if (header[0] != 'B' || header[1] != 'M') { koi_close(handle); return 0; }

    data_offset = read32(header + 10);
    info_size = read32(header + 14);
    image_width = (long)(int)read32(header + 18);
    image_height = (long)(int)read32(header + 22);
    depth = read16(header + 28);
    compression = read32(header + 30);

    if (image_height < 0) { image_height = -image_height; upside_down = 1; }
    if (info_size > 40) {
        long extra = (long)info_size - 40;
        if (extra > MAX_EXTRA_HEADER) { koi_close(handle); return 0; }
        if (!read_exactly(handle, extra_header, extra)) { koi_close(handle); return 0; }
        consumed += extra;
        if (extra >= 12) {
            red_mask = read32(extra_header);
            green_mask = read32(extra_header + 4);
            blue_mask = read32(extra_header + 8);
        }
    } else if (compression == 3) {
        if (!read_exactly(handle, extra_header, 12)) { koi_close(handle); return 0; }
        consumed += 12;
        red_mask = read32(extra_header);
        green_mask = read32(extra_header + 4);
        blue_mask = read32(extra_header + 8);
    }

    if (compression == 3) {
        if (depth != 32 || red_mask != MASK_RED || green_mask != MASK_GREEN ||
            blue_mask != MASK_BLUE) { koi_close(handle); return 0; }
    } else if (compression != 0 || (depth != 24 && depth != 32)) {
        koi_close(handle);
        return 0;
    }
    if (data_offset < 14 + info_size) { koi_close(handle); return 0; }

    bytes_per_row = image_width * (depth / 8);
    padded_row = (bytes_per_row + 3) & ~3L;
    if ((long)data_offset - consumed > 0) {
        long skip = (long)data_offset - consumed;
        while (skip > 0) {
            long chunk = skip < (long)sizeof(row_bytes) ? skip : (long)sizeof(row_bytes);
            if (!read_exactly(handle, row_bytes, chunk)) { koi_close(handle); return 0; }
            skip -= chunk;
        }
    }

    desktop_wallpaper_width = (int)image_width;
    desktop_wallpaper_height = (int)image_height;
    desktop_wallpaper = (koi_uint32*)koi_alloc((koi_uint64)desktop_wallpaper_width *
                                              (koi_uint64)desktop_wallpaper_height *
                                              sizeof(koi_uint32));
    if (!desktop_wallpaper) { koi_close(handle); return 0; }

    for (long line = 0; line < image_height; line++) {
        long target = upside_down ? line : image_height - 1 - line;
        koi_uint32* output = desktop_wallpaper + (koi_uint64)target * desktop_wallpaper_width;
        if (!read_exactly(handle, row_bytes, padded_row)) {
            free_wallpaper();
            koi_close(handle);
            return 0;
        }
        for (long x = 0; x < image_width; x++) {
            const koi_uint8* pixel = row_bytes + x * (depth / 8);
            output[x] = koi_gfx_color(pixel[2], pixel[1], pixel[0]);
        }
    }

    koi_close(handle);
    target_height = (long)screen.height - WINDOW_TOPBAR_H - WINDOW_TASKBAR_H;
    if (target_height <= 0) { free_wallpaper(); return 0; }
    return 1;
}

static void cursor_hide(void) {
    if (!cursor_saved) return;
    for (int row = 0; row < CURSOR_H; row++) {
        int py = cursor_y + row;
        koi_uint32* line;
        if (py < 0 || py >= (int)screen.height) continue;
        line = pixel_row(py);
        for (int col = 0; col < CURSOR_W; col++) {
            int px = cursor_x + col;
            if (px < 0 || px >= (int)screen.width) continue;
            line[px] = cursor_under[row][col];
        }
    }
    koi_gfx_present_rect(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
    cursor_saved = 0;
}

static void cursor_show(int x, int y) {
    cursor_x = x;
    cursor_y = y;
    for (int row = 0; row < CURSOR_H; row++) {
        int py = y + row;
        koi_uint32* line;
        if (py < 0 || py >= (int)screen.height) continue;
        line = pixel_row(py);
        for (int col = 0; col < CURSOR_W; col++) {
            int px = x + col;
            char shape;
            if (px < 0 || px >= (int)screen.width) continue;
            cursor_under[row][col] = line[px];
            shape = cursor_shape[row][col];
            if (shape == 'o') line[px] = cursor_edge;
            else if (shape == '*') line[px] = cursor_ink;
        }
    }
    cursor_saved = 1;
    koi_gfx_present_rect(x, y, CURSOR_W, CURSOR_H);
}

/* ---- Drawing ------------------------------------------------------------- */

/* Columns, not bytes.
 *
 * Every width here was a byte count until there was a second alphabet: a
 * Russian title is twice the bytes of an English one and the same number of
 * cells, so a title bar measured in bytes centres its text off the edge and a
 * menu measured in bytes claims twice the strip it draws in. Continuation
 * bytes of a UTF-8 sequence are the ones that are not a new character. */
static int text_width(const char* text) {
    int columns = 0;
    for (int index = 0; text[index]; index++)
        if (((unsigned char)text[index] & 0xC0) != 0x80) columns++;
    return columns * WINDOW_CHAR_W;
}

void window_label(int x, int y, const char* text, koi_uint32 color) {
    koi_gfx_text(x, y, text, color, KOI_TEXT_TRANSPARENT);
}

void window_label_styled(int x, int y, const char* text, koi_uint32 color,
                         int style) {
    koi_gfx_text_styled(x, y, text, color, KOI_TEXT_TRANSPARENT, style);
}

/* A solid triangle, for the boxes on a title bar.
 *
 * Drawn rather than typed. The first version used the arrow characters from
 * code page 437 and got three empty squares, because this font has a hundred
 * and twenty-five glyphs and those are not among them - and a control that
 * depends on which code page is fitted is a control that will be empty again
 * the day a different font arrives. */
static void triangle(int x, int y, int width, int height, int down,
                     koi_uint32 color) {
    for (int row = 0; row < height; row++) {
        int span = down ? (width - row * 2) : (row * 2 + 1);
        int start = down ? (x + row) : (x + width / 2 - row);

        if (span < 1) break;
        if (start < x) { span -= x - start; start = x; }
        if (span > 0) koi_gfx_line(start, y + row, start + span - 1, y + row,
                                   color);
    }
}

/* A bevel: light on the top and left, shadow on the bottom and right. One
   pixel, not two - the mock-up this follows is deliberately cleaner than the
   16-bit systems it descends from, which drew four. */
static void bevel(int x, int y, int width, int height, koi_uint32 top_left,
                  koi_uint32 bottom_right) {
    koi_gfx_line(x, y, x + width - 1, y, top_left);
    koi_gfx_line(x, y, x, y + height - 1, top_left);
    koi_gfx_line(x, y + height - 1, x + width - 1, y + height - 1, bottom_right);
    koi_gfx_line(x + width - 1, y, x + width - 1, y + height - 1, bottom_right);
}

void window_raised(int x, int y, int width, int height) {
    koi_gfx_fill(x, y, width, height, window_face);
    bevel(x, y, width, height, window_light, window_shadow);
}

void window_sunken(int x, int y, int width, int height) {
    koi_gfx_fill(x, y, width, height, window_client_paper);
    bevel(x, y, width, height, window_shadow, window_light);
}

/* The desktop: a vertical wash from one water to another.
 *
 * Computed rather than loaded. A picture would be a file to ship, a format to
 * decode and a decision about what happens on a screen of a different size;
 * two colours and a division answer all three. */
static void paint_desktop(void) {
    int top = WINDOW_TOPBAR_H;
    int bottom = (int)screen.height - WINDOW_TASKBAR_H;
    int span = bottom - top;
    if (desktop_wallpaper && desktop_wallpaper_width > 0 &&
        desktop_wallpaper_height > 0) {
        for (int row = 0; row < span; row++) {
            int source_y = (int)((koi_uint64)row * desktop_wallpaper_height / span);
            koi_uint32* target = pixel_row(top + row);
            koi_uint32* source = desktop_wallpaper +
                                 (koi_uint64)source_y * desktop_wallpaper_width;

            for (int x = 0; x < (int)screen.width; x++) {
                int source_x = (int)((koi_uint64)x * desktop_wallpaper_width /
                                     (int)screen.width);
                target[x] = source[source_x];
            }
        }
    } else {
        int red_a = (int)((desktop_top >> 16) & 0xFF);
        int green_a = (int)((desktop_top >> 8) & 0xFF);
        int blue_a = (int)(desktop_top & 0xFF);
        int red_b = (int)((desktop_bottom >> 16) & 0xFF);
        int green_b = (int)((desktop_bottom >> 8) & 0xFF);
        int blue_b = (int)(desktop_bottom & 0xFF);

        if (span < 1) span = 1;
        for (int row = 0; row < span; row++) {
            koi_uint32 color = koi_gfx_color(
                red_a + (red_b - red_a) * row / span,
                green_a + (green_b - green_a) * row / span,
                blue_a + (blue_b - blue_a) * row / span);
            koi_gfx_line(0, top + row, (int)screen.width - 1, top + row, color);
        }
    }
}

/* A menu strip, and the x of each label, so a click can be turned back into
   which one was hit without a second layout pass. */
static int menu_hit(const WINDOW_MENU* menus, int count, int x, int y,
                    int strip_x, int strip_y, int strip_w) {
    int place = strip_x + WINDOW_CHAR_W;

    if (y < strip_y || y >= strip_y + WINDOW_MENU_H) return -1;
    if (x < strip_x || x >= strip_x + strip_w) return -1;
    for (int index = 0; index < count; index++) {
        int width = text_width(menus[index].label) + WINDOW_CHAR_W * 2;
        if (x >= place && x < place + width) return index;
        place += width;
    }
    return -1;
}

static void paint_menu_strip(const WINDOW_MENU* menus, int count, int x, int y,
                             int width, int highlight) {
    int place = x + WINDOW_CHAR_W;

    koi_gfx_fill(x, y, width, WINDOW_MENU_H, window_face);
    koi_gfx_line(x, y + WINDOW_MENU_H - 1, x + width - 1, y + WINDOW_MENU_H - 1,
                 window_shadow);
    for (int index = 0; index < count; index++) {
        int item_width = text_width(menus[index].label) + WINDOW_CHAR_W * 2;
        if (index == highlight) {
            koi_gfx_fill(place, y + 1, item_width, WINDOW_MENU_H - 2,
                         window_accent);
            window_label(place + WINDOW_CHAR_W, y + 2, menus[index].label,
                         window_client_paper);
        } else {
            window_label(place + WINDOW_CHAR_W, y + 2, menus[index].label,
                         window_text);
        }
        place += item_width;
    }
}

/* The open drop-down, and where it landed. Drawn last of everything so it is
   over the windows rather than under them. */
static void paint_dropdown(void) {
    const WINDOW_MENU* menus = menu_owner ? menu_owner->menus : desktop_menus;
    int count = menu_owner ? menu_owner->menu_count : desktop_menu_count;
    const WINDOW_MENU* menu;
    int strip_x, strip_y, place;

    if (menu_open < 0 || menu_open >= count) return;
    menu = &menus[menu_open];

    if (menu_owner) {
        strip_x = menu_owner->x + WINDOW_BORDER;
        strip_y = menu_owner->y + WINDOW_BORDER + WINDOW_TITLE_H;
    } else {
        strip_x = 0;
        strip_y = 0;
    }

    place = strip_x + WINDOW_CHAR_W;
    for (int index = 0; index < menu_open; index++)
        place += text_width(menus[index].label) + WINDOW_CHAR_W * 2;

    menu_w = 0;
    for (int index = 0; index < menu->count; index++) {
        int width = text_width(menu->items[index].label ?
                               menu->items[index].label : "") +
                    WINDOW_CHAR_W * 4;
        if (width > menu_w) menu_w = width;
    }
    if (menu_w < 120) menu_w = 120;
    menu_h = menu->count * (WINDOW_CHAR_H + 4) + 6;
    menu_x = place;
    menu_y = strip_y + WINDOW_MENU_H;
    if (menu_x + menu_w > (int)screen.width) menu_x = (int)screen.width - menu_w;

    window_raised(menu_x, menu_y, menu_w, menu_h);
    for (int index = 0; index < menu->count; index++) {
        int row = menu_y + 3 + index * (WINDOW_CHAR_H + 4);
        if (!menu->items[index].label) {
            koi_gfx_line(menu_x + 4, row + WINDOW_CHAR_H / 2,
                         menu_x + menu_w - 5, row + WINDOW_CHAR_H / 2,
                         window_shadow);
            continue;
        }
        window_label(menu_x + WINDOW_CHAR_W, row + 2, menu->items[index].label,
                     window_text);
    }
}

/* The three boxes at the right of a title bar, in the order the mock-up has
   them: minimise, then the two that will grow and restore. */
#define TITLE_MINIMISE 0
#define TITLE_LOWER 1
#define TITLE_CLOSE 2

static void title_button(int x, int y, int kind) {
    window_raised(x, y, 18, 16);
    if (kind == TITLE_MINIMISE) {
        koi_gfx_fill(x + 4, y + 10, 10, 3, window_text);
    } else if (kind == TITLE_LOWER) {
        triangle(x + 4, y + 5, 10, 5, 1, window_text);
    } else {
        /* A cross, because the button closes the window. It used to be an
           upward triangle, which said "grow" and did the one irreversible
           thing on the bar. */
        koi_gfx_line(x + 5, y + 4, x + 12, y + 11, window_text);
        koi_gfx_line(x + 5, y + 5, x + 11, y + 11, window_text);
        koi_gfx_line(x + 12, y + 4, x + 5, y + 11, window_text);
        koi_gfx_line(x + 11, y + 4, x + 5, y + 10, window_text);
    }
}

static void paint_window(WINDOW* window, int active) {
    int client_x, client_y, client_w, client_h;
    int title_y = window->y + WINDOW_BORDER;

    if (window->minimised) return;

    /* The frame, then the title, then whatever the window itself draws. */
    window_raised(window->x, window->y, window->width, window->height);
    koi_gfx_fill(window->x + WINDOW_BORDER, title_y,
                 window->width - 2 * WINDOW_BORDER, WINDOW_TITLE_H,
                 active ? window_title_active : window_title_idle);
    {
        int centre = window->x + (window->width - text_width(window->title)) / 2;
        window_label(centre, title_y + 3, window->title, window_text);
    }
    title_button(window->x + WINDOW_BORDER + 2, title_y + 3, TITLE_MINIMISE);
    title_button(window->x + window->width - WINDOW_BORDER - 40, title_y + 3,
                 TITLE_LOWER);
    title_button(window->x + window->width - WINDOW_BORDER - 20, title_y + 3,
                 TITLE_CLOSE);

    if (window->menu_count)
        paint_menu_strip(window->menus, window->menu_count,
                         window->x + WINDOW_BORDER,
                         title_y + WINDOW_TITLE_H,
                         window->width - 2 * WINDOW_BORDER,
                         (menu_owner == window) ? menu_open : -1);

    window_client(window, &client_x, &client_y, &client_w, &client_h);
    window_sunken(client_x - 1, client_y - 1, client_w + 2, client_h + 2);
    if (window->paint) {
        koi_gfx_scissor(client_x, client_y, client_w, client_h);
        window->paint(window, client_x, client_y, client_w, client_h);
        koi_gfx_reset_scissor();
    }

    /* The grip. Three diagonal strokes in the corner, which is the shape every
       system has used for "drag me" since windows could be resized at all -
       and which is the only part of a frame that has to be learnt once rather
       than explained. */
    for (int line = 0; line < 3; line++) {
        int offset = 3 + line * 4;
        koi_gfx_line(window->x + window->width - offset - 1,
                     window->y + window->height - 4,
                     window->x + window->width - 4,
                     window->y + window->height - offset - 1, window_shadow);
    }
}

/* The Start button, and where the rest of the bar begins after it.
 *
 * Its width is measured from its text rather than fixed, so a translation does
 * not overflow it and an icon can be given room later without anything else
 * moving: the space in front of the label is where one goes, and it is zero
 * wide until there is something to put in it. */
#define LAUNCHER_ICON_W 0

static int launcher_width(void) {
    if (!launcher_label[0]) return 0;
    return text_width(launcher_label) + LAUNCHER_ICON_W + WINDOW_CHAR_W * 2;
}

static void paint_launcher(int y) {
    int width = launcher_width();

    if (!width) return;
    /* Pressed in while its menu is open, which is the only cue that the menu
       belongs to this button rather than floating above the bar. */
    if (launcher_pressed) window_sunken(2, y + 3, width, WINDOW_TASKBAR_H - 6);
    else window_raised(2, y + 3, width, WINDOW_TASKBAR_H - 6);
    window_label_styled(2 + WINDOW_CHAR_W + LAUNCHER_ICON_W, y + 6,
                        launcher_label, window_text, KOI_TEXT_BOLD);
}

static void paint_taskbar(void) {
    int y = (int)screen.height - WINDOW_TASKBAR_H;
    int place = launcher_width() ? launcher_width() + 8 : 4;

    window_raised(0, y, (int)screen.width, WINDOW_TASKBAR_H);
    paint_launcher(y);
    for (int index = 0; index < order_count; index++) {
        WINDOW* window = order[index];
        int width = text_width(window->title) + WINDOW_CHAR_W * 2;

        if (width > 200) width = 200;
        if (place + width > (int)screen.width - 4) break;
        /* The active one pressed in, the rest raised - which is the only cue
           the bar can give about which window a keystroke would reach. */
        if (index == order_count - 1 && !window->minimised)
            window_sunken(place, y + 3, width, WINDOW_TASKBAR_H - 6);
        else
            window_raised(place, y + 3, width, WINDOW_TASKBAR_H - 6);
        window_label(place + WINDOW_CHAR_W, y + 6, window->title, window_text);
        place += width + 4;
    }
}

void window_tile(void) {
    int visible = 0;
    int columns = 1;
    int rows;
    int top = WINDOW_TOPBAR_H;
    int height = (int)screen.height - WINDOW_TASKBAR_H - top;
    int slot = 0;

    for (int index = 0; index < order_count; index++)
        if (!order[index]->minimised) visible++;
    if (!visible) return;

    /* As square a grid as the count allows: two windows side by side, three or
       four in two columns. Anything cleverer needs to know what is in them. */
    while (columns * columns < visible) columns++;
    if (columns > 1 && columns * (columns - 1) >= visible) rows = columns - 1;
    else rows = columns;
    if (rows < 1) rows = 1;

    for (int index = 0; index < order_count; index++) {
        WINDOW* window = order[index];
        int column, row;

        if (window->minimised) continue;
        column = slot % columns;
        row = slot / columns;
        window->x = column * ((int)screen.width / columns);
        window->y = top + row * (height / rows);
        window->width = (int)screen.width / columns - 4;
        window->height = height / rows - 4;
        slot++;
    }
    dirty = 1;
}

void window_repaint(void) { dirty = 1; }
void window_quit(void) { running = 0; }

static void draw_everything(void) {
    cursor_hide();
    paint_desktop();

    koi_gfx_fill(0, 0, (int)screen.width, WINDOW_TOPBAR_H, window_face);
    if (desktop_menu_count)
        paint_menu_strip(desktop_menus, desktop_menu_count, 0, 0,
                         (int)screen.width, menu_owner ? -1 : menu_open);
    window_label((int)screen.width - text_width(desktop_title) - WINDOW_CHAR_W,
                 3, desktop_title, window_text);

    for (int index = 0; index < order_count; index++)
        paint_window(order[index], index == order_count - 1);

    paint_taskbar();
    if (menu_open >= 0) paint_dropdown();
    koi_gfx_present();
    dirty = 0;
}

/* ---- Windows ------------------------------------------------------------- */

void window_client(const WINDOW* window, int* x, int* y, int* width,
                   int* height) {
    int top = window->y + WINDOW_BORDER + WINDOW_TITLE_H +
              (window->menu_count ? WINDOW_MENU_H : 0);

    *x = window->x + WINDOW_BORDER + 1;
    *y = top + 1;
    *width = window->width - 2 * WINDOW_BORDER - 2;
    *height = window->y + window->height - WINDOW_BORDER - top - 2;
    if (*width < 0) *width = 0;
    if (*height < 0) *height = 0;
}

WINDOW* window_new(const char* title, int x, int y, int width, int height) {
    for (int index = 0; index < WINDOW_MAX; index++) {
        WINDOW* window = &windows[index];
        long at = 0;

        if (window->used) continue;
        memset(window, 0, sizeof(*window));
        window->used = 1;
        while (title[at] && at < WINDOW_TITLE_MAX - 1) {
            window->title[at] = title[at];
            at++;
        }
        window->title[at] = 0;
        window->x = x;
        window->y = y;
        window->width = width;
        window->height = height;
        order[order_count++] = window;
        dirty = 1;
        return window;
    }
    return (WINDOW*)0;
}

void window_delete(WINDOW* window) {
    int out = 0;

    if (!window) return;
    for (int index = 0; index < order_count; index++)
        if (order[index] != window) order[out++] = order[index];
    order_count = out;
    window->used = 0;
    if (menu_owner == window) { menu_owner = (WINDOW*)0; menu_open = -1; }
    dirty = 1;
}

void window_raise(WINDOW* window) {
    int out = 0;

    if (!window || (order_count && order[order_count - 1] == window)) return;
    for (int index = 0; index < order_count; index++)
        if (order[index] != window) order[out++] = order[index];
    order[out++] = window;
    order_count = out;
    dirty = 1;
}

WINDOW* window_active(void) {
    return order_count ? order[order_count - 1] : (WINDOW*)0;
}

/* The topmost window under a point, or NULL. Searched from the top down,
   because that is the one a click belongs to. */
static WINDOW* window_at(int x, int y) {
    for (int index = order_count - 1; index >= 0; index--) {
        WINDOW* window = order[index];
        if (window->minimised) continue;
        if (x >= window->x && x < window->x + window->width &&
            y >= window->y && y < window->y + window->height) return window;
    }
    return (WINDOW*)0;
}

static int launcher_hit(int x, int y) {
    return launcher_width() && y >= (int)screen.height - WINDOW_TASKBAR_H &&
           x >= 2 && x < 2 + launcher_width();
}

static int taskbar_hit(int x, int y) {
    int place = launcher_width() ? launcher_width() + 8 : 4;

    if (y < (int)screen.height - WINDOW_TASKBAR_H) return -1;
    for (int index = 0; index < order_count; index++) {
        int width = text_width(order[index]->title) + WINDOW_CHAR_W * 2;
        if (width > 200) width = 200;
        if (x >= place && x < place + width) return index;
        place += width + 4;
    }
    return -1;
}

void window_launcher(const char* label) {
    int at = 0;

    while (label && label[at] && at + 1 < (int)sizeof(launcher_label)) {
        launcher_label[at] = label[at];
        at++;
    }
    launcher_label[at] = 0;
    dirty = 1;
}

void window_launcher_anchor(int* x, int* y) {
    if (x) *x = 2;
    if (y) *y = (int)screen.height - WINDOW_TASKBAR_H;
}

void window_launcher_pressed(int pressed) {
    launcher_pressed = pressed != 0;
    dirty = 1;
}

int window_popup(const WINDOW_ITEM* items, int count, int x, int y) {
    int width = 140;
    int height;
    int rows = 0;
    int chosen = -1;
    int keyed = -1;              /* the row the keyboard is on, or none */
    KOI_POINTER pointer;
    int was_down = 0;
    int opening;

    if (count > WINDOW_ITEM_MAX) count = WINDOW_ITEM_MAX;
    for (int index = 0; index < count; index++) {
        int item = text_width(items[index].label ? items[index].label : "") +
                   WINDOW_CHAR_W * 4;
        if (item > width) width = item;
        rows++;
    }
    if (!rows) return -1;
    height = rows * (WINDOW_CHAR_H + 4) + 6;

    /* Upwards from the point given, and kept on the screen. The thing that
       opens one of these is on the taskbar at the bottom. */
    y -= height;
    if (y < 0) y = 0;
    if (x + width > (int)screen.width) x = (int)screen.width - width;
    if (x < 0) x = 0;

    /* The press that opened this menu is usually still down, and it is not an
       ordinary press.
     *
     * Two ways of working a menu have to come out of that one press, because
     * both are in people's hands already: hold the button, slide onto an
     * entry and let go - and click once, let go over nothing, and have the
     * menu stand there until a second click chooses. Counting the opening
     * press as any other press left only the first of them working: the
     * release that ends a plain click lands on nothing and was read as "the
     * pointer was put down outside the menu", which dismisses it. The menu
     * could be opened and could not be used without dragging.
     *
     * So it is remembered as the opening press. Letting it go over an entry
     * chooses that entry; letting it go anywhere else leaves the menu open
     * and everything after it behaves normally. */
    koi_mouse(&pointer);
    opening = (pointer.buttons & KOI_BUTTON_LEFT) != 0;

    for (;;) {
        int highlight = -1;

        koi_mouse(&pointer);
        if (pointer.x >= x && pointer.x < x + width &&
            pointer.y >= y && pointer.y < y + height) {
            int row = (pointer.y - y - 3) / (WINDOW_CHAR_H + 4);
            if (row >= 0 && row < rows) { highlight = row; keyed = -1; }
        }
        /* The keyboard's row when the pointer is not over the menu, so the two
           never both claim to be pointing at something. */
        if (highlight < 0) highlight = keyed;

        cursor_hide();
        window_raised(x, y, width, height);
        for (int index = 0; index < rows; index++) {
            int row = y + 3 + index * (WINDOW_CHAR_H + 4);
            if (!items[index].label) {
                koi_gfx_line(x + 4, row + WINDOW_CHAR_H / 2,
                             x + width - 5, row + WINDOW_CHAR_H / 2,
                             window_shadow);
                continue;
            }
            if (index == highlight) {
                koi_gfx_fill(x + 2, row, width - 4, WINDOW_CHAR_H + 4,
                             window_accent);
                window_label(x + WINDOW_CHAR_W, row + 2, items[index].label,
                             window_client_paper);
            } else {
                window_label(x + WINDOW_CHAR_W, row + 2, items[index].label,
                             window_text);
            }
        }
        koi_gfx_present_rect(x, y, width, height);
        cursor_show(pointer.x, pointer.y);

        if (pointer.buttons & KOI_BUTTON_LEFT) {
            if (!opening) was_down = 1;
        } else if (opening) {
            /* The click that opened the menu has ended. Over an entry that
               was a press-and-drag choice; anywhere else the menu stays. */
            opening = 0;
            if (highlight >= 0 && items[highlight].label) {
                chosen = items[highlight].id;
                break;
            }
        } else if (was_down) {
            /* Released. On an item it chooses; anywhere else it dismisses. A
               separator is not an item and swallows the click rather than
               closing, which is what every menu does. */
            if (highlight >= 0 && items[highlight].label)
                chosen = items[highlight].id;
            else if (highlight >= 0) { was_down = 0; continue; }
            break;
        }

        /* The keyboard reaches it too. A menu that only a pointer can open is
           a menu somebody without a working pointer cannot use at all, and
           this one has the machine's power switch in it. */
        if (koi_keypressed()) {
            int key = koi_getchar();

            if (key == 27) break;
            if (key == KOI_KEY_UP || key == KOI_KEY_DOWN) {
                int step = key == KOI_KEY_DOWN ? 1 : -1;
                int at = keyed < 0 ? (step > 0 ? -1 : rows) : keyed;

                /* Past a separator rather than onto it, and round the ends. */
                for (int tried = 0; tried < rows; tried++) {
                    at += step;
                    if (at < 0) at = rows - 1;
                    if (at >= rows) at = 0;
                    if (items[at].label) break;
                }
                keyed = at;
            } else if (key == '\n' || key == '\r') {
                if (keyed >= 0 && items[keyed].label) chosen = items[keyed].id;
                break;
            }
        }
        koi_sleep(10);
    }

    cursor_hide();
    dirty = 1;
    return chosen;
}

/* ---- Modal questions ------------------------------------------------------
 *
 * A box in the middle of the screen with everything behind it dimmed, and one
 * or two buttons along the bottom. There are two of them - a question and a
 * question with something to type - and they share their frame rather than
 * each drawing its own, because two boxes that are meant to look like the
 * same box and are drawn by two pieces of code end up looking like two boxes.
 *
 * The dimming is the point and not decoration. A box that merely sits on top
 * of the desktop looks like one more window among the windows, and a question
 * that can be mistaken for a window is one people answer without reading -
 * which matters when the question is whether to turn the machine off.
 * Darkening what is behind says, in the one language a screen has, that
 * nothing else is listening until this is answered.
 *
 * It is done by the kernel (koi_gfx_dim) because it reads the pixels back,
 * and reading a screen's worth of pixels through a system call each is not
 * something a program can do. */

typedef struct {
    int x, y, width, height;
    int button_w, button_h, buttons_y;
    int accept_x, cancel_x;
    int body_y;                  /* under the title, where a field would go */
} MODAL;

/* `cancel` may be absent, and then the box has one button and is a notice
   rather than a question - which is the difference between telling somebody
   something and asking them to agree to it. */
static void modal_layout(MODAL* box, const char* title, const char* message,
                         const char* accept, const char* cancel, int body_h) {
    int widest = text_width(accept);

    if (cancel && text_width(cancel) > widest) widest = text_width(cancel);
    box->button_w = widest + WINDOW_CHAR_W * 4;
    if (box->button_w < 90) box->button_w = 90;
    box->button_h = WINDOW_CHAR_H + 10;

    box->width = text_width(message) + WINDOW_CHAR_W * 4;
    if (box->width < text_width(title) + WINDOW_CHAR_W * 6)
        box->width = text_width(title) + WINDOW_CHAR_W * 6;
    if (cancel) {
        if (box->width < box->button_w * 2 + WINDOW_CHAR_W * 6)
            box->width = box->button_w * 2 + WINDOW_CHAR_W * 6;
    } else if (box->width < box->button_w + WINDOW_CHAR_W * 6) {
        box->width = box->button_w + WINDOW_CHAR_W * 6;
    }
    if (box->width > (int)screen.width - 40) box->width = (int)screen.width - 40;

    box->height = WINDOW_BORDER * 2 + WINDOW_TITLE_H + WINDOW_CHAR_H * 3
                  + box->button_h + body_h;
    box->x = ((int)screen.width - box->width) / 2;
    box->y = ((int)screen.height - box->height) / 2;
    box->body_y = box->y + WINDOW_BORDER + WINDOW_TITLE_H + WINDOW_CHAR_H * 2 - 2;
    box->buttons_y = box->y + box->height - WINDOW_BORDER - box->button_h - 8;
    if (cancel) {
        box->accept_x = box->x + box->width / 2 - box->button_w - WINDOW_CHAR_W;
        box->cancel_x = box->x + box->width / 2 + WINDOW_CHAR_W;
    } else {
        box->accept_x = box->x + (box->width - box->button_w) / 2;
        box->cancel_x = -1;
    }
}

/* The desktop as it really is, and then dimmed.
 *
 * Drawn again first because whatever was on the screen a moment ago - the
 * menu this was chosen from, most of the time - is otherwise still lying
 * there, and would be dimmed along with everything else and sit behind the
 * question looking like part of it. What is darkened has to be the desktop,
 * not the last thing drawn on top of it.
 *
 * A little over a third of the light is left: dark enough that the desktop
 * has plainly stepped back, light enough to still see what one was doing and
 * so what the question is about. */
static void modal_open(void) {
    draw_everything();
    cursor_hide();
    koi_gfx_dim(0, 0, (int)screen.width, (int)screen.height, 96);
    koi_gfx_present();
}

static void modal_frame(const MODAL* box, const char* title,
                        const char* message) {
    window_raised(box->x, box->y, box->width, box->height);
    koi_gfx_fill(box->x + WINDOW_BORDER, box->y + WINDOW_BORDER,
                 box->width - 2 * WINDOW_BORDER, WINDOW_TITLE_H,
                 window_title_active);
    window_label(box->x + (box->width - text_width(title)) / 2,
                 box->y + WINDOW_BORDER + 3, title, window_text);
    window_label(box->x + (box->width - text_width(message)) / 2,
                 box->y + WINDOW_BORDER + WINDOW_TITLE_H + WINDOW_CHAR_H - 4,
                 message, window_text);
}

/* 1 for the accepting button, 0 for the other, -1 for neither. */
static int modal_button_at(const MODAL* box, int x, int y) {
    if (y < box->buttons_y || y >= box->buttons_y + box->button_h) return -1;
    if (x >= box->accept_x && x < box->accept_x + box->button_w) return 1;
    if (box->cancel_x >= 0 && x >= box->cancel_x &&
        x < box->cancel_x + box->button_w) return 0;
    return -1;
}

static void modal_buttons(const MODAL* box, const char* accept,
                          const char* cancel, int over, int held, int answer) {
    for (int button = cancel ? 0 : 1; button < 2; button++) {
        int place = button ? box->accept_x : box->cancel_x;
        const char* label = button ? accept : cancel;
        int down = held == button && over == button;

        if (down) window_sunken(place, box->buttons_y, box->button_w,
                                box->button_h);
        else window_raised(place, box->buttons_y, box->button_w, box->button_h);
        /* The one the keyboard is on wears a line under its text: the mark a
           focused button has always worn, and one that needs no second colour
           to be visible on any theme. */
        window_label_styled(place + (box->button_w - text_width(label)) / 2 + down,
                            box->buttons_y + 5 + down, label, window_text,
                            answer == button ? KOI_TEXT_UNDERLINE : 0);
    }
}

int window_confirm(const char* title, const char* message, const char* accept,
                   const char* cancel, int accept_by_default) {
    MODAL box;
    int answer = (accept_by_default || !cancel) ? 1 : 0;   /* which button has the keyboard */
    int held = -1;                            /* the button the press went down on */
    int chosen = -1;
    KOI_POINTER pointer;
    int was_down;

    modal_layout(&box, title, message, accept, cancel, 0);
    modal_open();

    koi_mouse(&pointer);
    was_down = (pointer.buttons & KOI_BUTTON_LEFT) != 0;

    for (;;) {
        int over;

        koi_mouse(&pointer);
        over = modal_button_at(&box, pointer.x, pointer.y);

        cursor_hide();
        modal_frame(&box, title, message);
        modal_buttons(&box, accept, cancel, over, held, answer);
        koi_gfx_present_rect(box.x, box.y, box.width, box.height);
        cursor_show(pointer.x, pointer.y);

        if (pointer.buttons & KOI_BUTTON_LEFT) {
            if (!was_down) held = over;
            was_down = 1;
        } else if (was_down) {
            was_down = 0;
            /* Chosen only when the release lands on the button the press went
               down on, which is how a pointer says "no, not that one" after it
               has already been pressed. */
            if (held >= 0 && over == held) { chosen = held; break; }
            held = -1;
        }

        if (koi_keypressed()) {
            int key = koi_getchar();

            if (key == 27) { chosen = 0; break; }
            if (cancel && (key == KOI_KEY_LEFT || key == KOI_KEY_RIGHT ||
                           key == '\t'))
                answer = !answer;
            else if (key == '\n' || key == '\r') { chosen = answer; break; }
        }
        koi_sleep(10);
    }

    cursor_hide();
    dirty = 1;
    return chosen;
}

int window_message(const char* title, const char* message, const char* accept) {
    return window_confirm(title, message, accept, (const char*)0, 1);
}

/* The same box, with a line to type in.
 *
 * The field shows the end of what has been typed rather than the beginning,
 * because the end is where the caret is and a path being typed is a thing
 * whose last few characters are the ones in question. */
int window_prompt(const char* title, const char* message, const char* accept,
                  const char* cancel, char* buffer, int size) {
    MODAL box;
    int field_x, field_y, field_w;
    int field_h = WINDOW_CHAR_H + 8;
    int answer = 1;
    int held = -1;
    int chosen = -1;
    int length = 0;
    /* What the field starts with arrives selected, as a field somebody is
       about to retype always has: the first thing typed replaces it whole.
       Without this, a caller that remembers the last command - which is the
       obliging thing for a Run box to do - hands the next one a field that
       silently appends to it, and `nosuch` followed by `ver` runs `nosuchver`. */
    int selected;
    KOI_POINTER pointer;
    int was_down;

    if (!buffer || size < 2) return 0;
    while (buffer[length] && length < size - 1) length++;
    buffer[length] = 0;
    selected = length > 0;

    modal_layout(&box, title, message, accept, cancel, field_h + 6);
    /* Wide enough to be worth typing into even when the question is short. */
    if (box.width < WINDOW_CHAR_W * 44) {
        box.width = WINDOW_CHAR_W * 44;
        box.x = ((int)screen.width - box.width) / 2;
        box.accept_x = box.x + box.width / 2 - box.button_w - WINDOW_CHAR_W;
        box.cancel_x = box.x + box.width / 2 + WINDOW_CHAR_W;
    }
    field_x = box.x + WINDOW_CHAR_W * 2;
    field_w = box.width - WINDOW_CHAR_W * 4;
    field_y = box.body_y + 4;

    modal_open();
    koi_mouse(&pointer);
    was_down = (pointer.buttons & KOI_BUTTON_LEFT) != 0;

    for (;;) {
        int over;
        int columns = (field_w - WINDOW_CHAR_W) / WINDOW_CHAR_W;
        int from = 0;

        /* Show the tail that fits, counting characters and not bytes - one
           Russian letter is two bytes and the same one cell. */
        {
            int cells = 0;
            for (int at = 0; at < length; at++)
                if (((unsigned char)buffer[at] & 0xC0) != 0x80) cells++;
            while (cells > columns) {
                from++;
                while (from < length &&
                       ((unsigned char)buffer[from] & 0xC0) == 0x80) from++;
                cells--;
            }
        }

        koi_mouse(&pointer);
        over = modal_button_at(&box, pointer.x, pointer.y);

        cursor_hide();
        modal_frame(&box, title, message);
        window_sunken(field_x, field_y, field_w, field_h);
        if (selected)
            koi_gfx_fill(field_x + 4, field_y + 4, text_width(buffer + from),
                         WINDOW_CHAR_H, window_accent);
        window_label(field_x + 4, field_y + 4, buffer + from,
                     selected ? window_client_paper : window_text);
        {
            int caret = field_x + 4 + text_width(buffer + from);
            koi_gfx_line(caret, field_y + 3, caret, field_y + field_h - 4,
                         window_text);
        }
        modal_buttons(&box, accept, cancel, over, held, answer);
        koi_gfx_present_rect(box.x, box.y, box.width, box.height);
        cursor_show(pointer.x, pointer.y);

        if (pointer.buttons & KOI_BUTTON_LEFT) {
            if (!was_down) held = over;
            was_down = 1;
        } else if (was_down) {
            was_down = 0;
            if (held >= 0 && over == held) { chosen = held; break; }
            held = -1;
        }

        if (koi_keypressed()) {
            int key = koi_getchar();

            if (key == 27) { chosen = 0; break; }
            else if (key == '\n' || key == '\r') { chosen = answer; break; }
            else if (key == '\t') answer = !answer;
            else if (key == '\b') {
                if (selected) { length = 0; buffer[0] = 0; selected = 0; }
                /* A whole character, not a byte: stepping back one byte at a
                   time leaves half of a Russian letter in the buffer, which
                   is not a character at all and draws as rubbish. */
                while (length > 0 &&
                       ((unsigned char)buffer[length - 1] & 0xC0) == 0x80)
                    length--;
                if (length > 0) length--;
                buffer[length] = 0;
            } else if (key >= 0x20 && key < 0x100) {
                if (selected) { length = 0; buffer[0] = 0; selected = 0; }
                if (length < size - 1) {
                    buffer[length++] = (char)key;
                    buffer[length] = 0;
                }
            }
        }
        koi_sleep(10);
    }

    cursor_hide();
    dirty = 1;
    return chosen == 1 && buffer[0] ? 1 : 0;
}

void window_desktop_menu(const WINDOW_MENU* menus, int count) {
    if (count > WINDOW_MENU_MAX) count = WINDOW_MENU_MAX;
    for (int index = 0; index < count; index++) desktop_menus[index] = menus[index];
    desktop_menu_count = count;
    dirty = 1;
}

int window_open_desktop(const char* title) {
    long at = 0;

    if (koi_gfx_enter(&screen) != 0) return 0;
    while (title[at] && at < WINDOW_TITLE_MAX - 1) {
        desktop_title[at] = title[at];
        at++;
    }
    desktop_title[at] = 0;

    window_face = koi_gfx_color(0xCF, 0xE3, 0xEA);
    window_light = koi_gfx_color(0xFF, 0xFF, 0xFF);
    window_shadow = koi_gfx_color(0x6E, 0x94, 0xA4);
    window_text = koi_gfx_color(0x10, 0x28, 0x34);
    window_client_paper = koi_gfx_color(0xFF, 0xFF, 0xFF);
    window_title_active = koi_gfx_color(0x9E, 0xCF, 0xE2);
    window_title_idle = koi_gfx_color(0xC6, 0xDA, 0xE1);
    window_accent = koi_gfx_color(0x35, 0x7C, 0xA0);
    desktop_top = koi_gfx_color(0xD6, 0xEE, 0xF5);
    desktop_bottom = koi_gfx_color(0x8F, 0xC6, 0xDD);
    cursor_ink = koi_gfx_color(0xFF, 0xFF, 0xFF);
    cursor_edge = koi_gfx_color(0x00, 0x1A, 0x28);

    load_wallpaper();

    koi_mouse_place((int)screen.width / 2, (int)screen.height / 2);
    running = 1;
    dirty = 1;
    return 1;
}

int window_reopen_desktop(void) {
    if (koi_gfx_enter(&screen) != 0) return 0;
    koi_mouse_place((int)screen.width / 2, (int)screen.height / 2);
    cursor_saved = 0;
    dirty = 1;
    return 1;
}

void window_close_desktop(void) {
    cursor_hide();
    koi_gfx_leave();
    running = 0;
}

/* ---- The loop ------------------------------------------------------------ */

static int dragging;
static int sizing;
static int drag_x, drag_y;

#define GRIP 14

/* A click that landed on a menu bar, wherever that bar was. Returns 1 when it
   was taken, so the caller stops looking. */
static int take_menu_click(int x, int y) {
    WINDOW* top = window_active();
    int hit;

    /* An open drop-down first: a click inside it chooses, and a click
       anywhere else closes it and is then reconsidered as an ordinary one. */
    if (menu_open >= 0) {
        const WINDOW_MENU* menus = menu_owner ? menu_owner->menus : desktop_menus;
        const WINDOW_MENU* menu = &menus[menu_open];

        if (x >= menu_x && x < menu_x + menu_w &&
            y >= menu_y && y < menu_y + menu_h) {
            int index = (y - menu_y - 3) / (WINDOW_CHAR_H + 4);
            if (index >= 0 && index < menu->count && menu->items[index].label) {
                int id = menu->items[index].id;
                /* Which bar this came from is remembered before it is cleared.
                   It used to be read out of menu_owner afterwards, by which
                   time it was already zero - so every menu event arrived
                   claiming to belong to no window, and a caller asking "was
                   this my window's Close or the desktop's Exit" got the same
                   answer to both. */
                chosen_owner = menu_owner;
                menu_open = -1;
                menu_owner = (WINDOW*)0;
                dirty = 1;
                return 0x10000 | id;
            }
            return 1;
        }
        menu_open = -1;
        menu_owner = (WINDOW*)0;
        dirty = 1;
    }

    hit = menu_hit(desktop_menus, desktop_menu_count, x, y, 0, 0,
                   (int)screen.width);
    if (hit >= 0) {
        menu_owner = (WINDOW*)0;
        menu_open = hit;
        dirty = 1;
        return 1;
    }

    if (top && !top->minimised && top->menu_count) {
        hit = menu_hit(top->menus, top->menu_count, x, y,
                       top->x + WINDOW_BORDER,
                       top->y + WINDOW_BORDER + WINDOW_TITLE_H,
                       top->width - 2 * WINDOW_BORDER);
        if (hit >= 0) {
            menu_owner = top;
            menu_open = hit;
            dirty = 1;
            return 1;
        }
    }
    return 0;
}

int window_next(WINDOW_EVENT* event) {
    static KOI_POINTER pointer;
    static unsigned int last_press;
    static int started;
    static koi_uint64 last_tick;
    static koi_uint64 last_click_at;
    static int last_click_x = -100;
    static int last_click_y = -100;

    event->type = WINDOW_EVENT_NONE;
    event->window = (WINDOW*)0;
    event->id = 0;

    if (!started) {
        koi_mouse(&pointer);
        last_press = pointer.presses[0];
        started = 1;
    }

    while (running) {
        int previous_x = pointer.x;
        int previous_y = pointer.y;

        if (dirty) { draw_everything(); cursor_show(pointer.x, pointer.y); }

        koi_sleep(10);

        /* Anything that changes on its own gets its repaint here. The shortest
           interval any open window asked for wins, and a desktop where nothing
           asked for one never wakes up at all. */
        {
            koi_uint64 now = koi_uptime();
            int soonest = 0;

            for (int index = 0; index < order_count; index++) {
                int wanted = order[index]->repaint_ms;
                if (order[index]->minimised || wanted <= 0) continue;
                if (!soonest || wanted < soonest) soonest = wanted;
            }
            if (soonest && now - last_tick >= (koi_uint64)soonest) {
                last_tick = now;
                dirty = 1;
                continue;
            }
        }

        if (koi_keypressed()) {
            int key = koi_getchar();
            WINDOW* top = window_active();

            if (key == 27 && menu_open >= 0) {
                menu_open = -1;
                menu_owner = (WINDOW*)0;
                dirty = 1;
                continue;
            }
            /* Ctrl+Escape reaches the taskbar button wherever the keyboard
               was pointing, and is not offered to the window in front - which
               is the point of it. A menu holding the machine's power switch
               that only a pointer can reach is one somebody cannot use when
               the pointer is the thing that has stopped. */
            if (key == KOI_KEY_MENU) {
                if (!launcher_label[0]) continue;
                event->type = WINDOW_EVENT_LAUNCHER;
                event->window = (WINDOW*)0;
                event->id = 0;
                return 1;
            }
            if (top && top->key) { top->key(top, key); continue; }
            event->type = WINDOW_EVENT_KEY;
            event->window = top;
            event->id = key;
            return 1;
        }

        koi_mouse(&pointer);

        if (dragging || sizing) {
            if (pointer.buttons & KOI_BUTTON_LEFT) {
                WINDOW* top = window_active();
                if (top && (pointer.x != previous_x || pointer.y != previous_y)) {
                    if (sizing) {
                        int least_w = top->minimum_width ? top->minimum_width : 160;
                        int least_h = top->minimum_height ? top->minimum_height : 100;
                        top->width = pointer.x - top->x + drag_x;
                        top->height = pointer.y - top->y + drag_y;
                        if (top->width < least_w) top->width = least_w;
                        if (top->height < least_h) top->height = least_h;
                    } else {
                        top->x = pointer.x - drag_x;
                        top->y = pointer.y - drag_y;
                        if (top->y < WINDOW_TOPBAR_H) top->y = WINDOW_TOPBAR_H;
                    }
                    dirty = 1;
                }
                continue;
            }
            dragging = 0;
            sizing = 0;
        }

        if (pointer.presses[0] != last_press) {
            /* A double click is two clicks close together in time and place,
             * not two that happened to land in one poll.
             *
             * The count from the driver is how many presses arrived since the
             * last look, which is one almost always - so anything asking for
             * two got them only when the machine was busy enough to miss a
             * poll between them. The icons in the control panel needed a
             * double click and were, in practice, decorations.
             *
             * Half a second and four pixels, which is what everything else
             * uses and what a hand actually does. */
            unsigned int clicks = pointer.presses[0] - last_press;
            koi_uint64 now = koi_uptime();
            int near = (pointer.x - last_click_x) * (pointer.x - last_click_x) +
                       (pointer.y - last_click_y) * (pointer.y - last_click_y) <= 16;

            if (clicks < 2 && near && now - last_click_at <= 500) clicks = 2;
            last_click_at = now;
            last_click_x = pointer.x;
            last_click_y = pointer.y;
            int x = pointer.x;
            int y = pointer.y;
            WINDOW* hit;
            int taken;

            last_press = pointer.presses[0];

            /* The taskbar button before anything else on the bar: it sits at
               the left where a window's own entry would otherwise be tested. */
            if (launcher_hit(x, y)) {
                event->type = WINDOW_EVENT_LAUNCHER;
                event->window = (WINDOW*)0;
                event->id = 0;
                return 1;
            }

            taken = take_menu_click(x, y);
            if (taken & 0x10000) {
                event->type = WINDOW_EVENT_MENU;
                event->window = chosen_owner;
                event->id = taken & 0xFFFF;
                return 1;
            }
            if (taken) continue;

            {
                int slot = taskbar_hit(x, y);
                if (slot >= 0) {
                    WINDOW* window = order[slot];
                    /* Marked for redrawing here rather than relying on the
                       raise to do it: raising a window that is already on top
                       is correctly a no-op, and un-minimising one is not - so
                       the button did nothing at all for the commonest case. */
                    window->minimised = 0;
                    window_raise(window);
                    dirty = 1;
                    continue;
                }
            }

            hit = window_at(x, y);
            if (!hit) continue;
            window_raise(hit);

            /* The title bar: its buttons, then dragging with what is left. */
            if (y >= hit->y + WINDOW_BORDER &&
                y < hit->y + WINDOW_BORDER + WINDOW_TITLE_H) {
                if (x < hit->x + WINDOW_BORDER + 22) {
                    hit->minimised = 1;
                    dirty = 1;
                    continue;
                }
                if (x >= hit->x + hit->width - WINDOW_BORDER - 20) {
                    event->type = WINDOW_EVENT_CLOSE;
                    event->window = hit;
                    return 1;
                }
                /* The middle one sends the window behind the others. It was
                   drawn from the first version and connected to nothing, so it
                   fell through to dragging - a button that looks like a button
                   and moves the window when pressed. */
                if (x >= hit->x + hit->width - WINDOW_BORDER - 40 &&
                    x < hit->x + hit->width - WINDOW_BORDER - 20) {
                    for (int index = order_count - 1; index > 0; index--)
                        order[index] = order[index - 1];
                    order[0] = hit;
                    dirty = 1;
                    continue;
                }
                dragging = 1;
                drag_x = x - hit->x;
                drag_y = y - hit->y;
                continue;
            }

            /* The corner, before the contents: a click there is a grab and
               not a click on whatever the window happens to draw underneath. */
            if (x >= hit->x + hit->width - GRIP &&
                y >= hit->y + hit->height - GRIP) {
                sizing = 1;
                drag_x = hit->x + hit->width - x;
                drag_y = hit->y + hit->height - y;
                continue;
            }

            {
                int client_x, client_y, client_w, client_h;
                window_client(hit, &client_x, &client_y, &client_w, &client_h);
                if (hit->click && x >= client_x && x < client_x + client_w &&
                    y >= client_y && y < client_y + client_h)
                    hit->click(hit, x - client_x, y - client_y, (int)clicks);
            }
            continue;
        }

        if (pointer.x != previous_x || pointer.y != previous_y) {
            cursor_hide();
            cursor_show(pointer.x, pointer.y);
        }
    }

    event->type = WINDOW_EVENT_QUIT;
    return 0;
}
