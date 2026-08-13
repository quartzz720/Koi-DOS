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

static void paint_taskbar(void) {
    int y = (int)screen.height - WINDOW_TASKBAR_H;
    int place = 4;

    window_raised(0, y, (int)screen.width, WINDOW_TASKBAR_H);
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

static int taskbar_hit(int x, int y) {
    int place = 4;

    if (y < (int)screen.height - WINDOW_TASKBAR_H) return -1;
    for (int index = 0; index < order_count; index++) {
        int width = text_width(order[index]->title) + WINDOW_CHAR_W * 2;
        if (width > 200) width = 200;
        if (x >= place && x < place + width) return index;
        place += width + 4;
    }
    return -1;
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
