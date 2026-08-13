#ifndef WINDOW_H
#define WINDOW_H

#include "koi.h"

/* Windows, for programs that draw.
 *
 * A desktop with a menu bar, windows that overlap and can be dragged, and a
 * bar along the bottom listing them. The shape is Windows 3.0's, with the one
 * addition 3.0 did not have and needed: overlapping windows get lost behind
 * each other, and something has to name the ones you cannot see.
 *
 * ---- What this is not, yet -----------------------------------------------
 *
 * The windows belong to one program. Koi-DOS holds one program in memory at a
 * time, at a fixed address, which is why running something from the file
 * manager goes through SYS_CHAIN and the screen visibly goes away. So a window
 * here is a part of the program that opened it, not another program.
 *
 * That is not a stopgap invented to hide a missing feature - it is what
 * Windows 1.0 through 3.0 in real mode actually were, and their bundled
 * applications were parts of one image for the same reason. Everything below
 * stays true when a second program can be resident; only who supplies the
 * paint callback changes.
 *
 * ---- How a program uses it ------------------------------------------------
 *
 *     window_open_desktop("Mizu-DOS");
 *     WINDOW* clock = window_new("Clock", 40, 40, 300, 200);
 *     clock->paint = paint_clock;
 *     while (window_next(&event)) { ... }
 *     window_close_desktop();
 *
 * A window's `paint` is handed the client area in screen coordinates and draws
 * whatever it likes into it. The library owns the frame, the title, the
 * ordering and the pointer; a window owns its contents and nothing else. That
 * split is the whole reason for a library rather than a pile of helpers.
 */

#define WINDOW_MAX 12
#define WINDOW_TITLE_MAX 48
#define WINDOW_MENU_MAX 8
#define WINDOW_ITEM_MAX 12

/* One entry of a drop-down. `id` comes back in the event; a zero label is a
   separator, which is a line and cannot be chosen. */
typedef struct {
    const char* label;
    int id;
} WINDOW_ITEM;

typedef struct {
    const char* label;
    WINDOW_ITEM items[WINDOW_ITEM_MAX];
    int count;
} WINDOW_MENU;

typedef struct WINDOW WINDOW;

struct WINDOW {
    char title[WINDOW_TITLE_MAX];
    int x, y, width, height;      /* the frame, not the client area */
    int minimised;
    int used;
    /* Smaller than this and the frame has eaten the contents. A window that
       can be dragged to nothing is a window somebody loses. */
    int minimum_width;
    int minimum_height;

    /* The menu strip inside this window, under its title. Zero menus means no
       strip and the client area starts higher. */
    WINDOW_MENU menus[WINDOW_MENU_MAX];
    int menu_count;

    /* Contents. `paint` is given the client rectangle in screen coordinates;
       `click` is given a point inside it, relative to its top left. */
    void (*paint)(WINDOW* window, int x, int y, int width, int height);
    void (*click)(WINDOW* window, int x, int y, int clicks);
    void (*key)(WINDOW* window, int key);
    /* Milliseconds between repaints, or 0 for a window that only changes when
     * something happens to it.
     *
     * Most windows are the second kind and redrawing them on a timer would be
     * a screenful of work for nothing. A clock and a progress bar are the
     * first kind: nothing happens to them at all, and they were both correct
     * and both frozen until this existed. */
    int repaint_ms;
    void* data;
};

#define WINDOW_EVENT_NONE 0
#define WINDOW_EVENT_MENU 1        /* a menu item was chosen */
#define WINDOW_EVENT_CLOSE 2       /* a window's close box */
#define WINDOW_EVENT_QUIT 3        /* the desktop was asked to end */
#define WINDOW_EVENT_KEY 4         /* a key nothing else wanted */

typedef struct {
    int type;
    WINDOW* window;                /* which one, when it is about a window */
    int id;                        /* the menu item, or the key */
} WINDOW_EVENT;

/* Take the screen. Returns 0 when it could not be had. */
int window_open_desktop(const char* title);
void window_close_desktop(void);

/* Take the screen again after something else has had it, keeping every window
   where it was. Returns 0 if it could not be had back. */
int window_reopen_desktop(void);

/* The desktop's own menu bar, across the top. */
void window_desktop_menu(const WINDOW_MENU* menus, int count);

WINDOW* window_new(const char* title, int x, int y, int width, int height);
void window_delete(WINDOW* window);
void window_raise(WINDOW* window);

/* Lay every open window out side by side, filling the desktop. What "tile"
   meant in the system this borrows its shape from - not "put three windows
   back where they started", which is what it did first and is a different
   verb. */
void window_tile(void);
WINDOW* window_active(void);

/* Where a window's contents live, in screen coordinates. */
void window_client(const WINDOW* window, int* x, int* y, int* width,
                   int* height);

/* Wait for something to happen and describe it. Returns 0 when the desktop is
   finished - which is the loop's condition, so the caller never has to test
   for quit twice. */
int window_next(WINDOW_EVENT* event);

/* End the loop. The next window_next returns 0. */
void window_quit(void);

/* Ask for everything to be drawn again. Called for you when a window moves or
   the order changes; call it yourself when a window's contents change. */
void window_repaint(void);

/* The palette, so a window's contents match its frame. Water, because that is
   what Mizu means. */
extern koi_uint32 window_face;
extern koi_uint32 window_light;
extern koi_uint32 window_shadow;
extern koi_uint32 window_text;
extern koi_uint32 window_client_paper;
extern koi_uint32 window_title_active;
extern koi_uint32 window_title_idle;
extern koi_uint32 window_accent;

/* Drawing helpers a window's contents will want anyway, in the same style as
   the frame: a raised or sunken rectangle, and text. */
void window_raised(int x, int y, int width, int height);
void window_sunken(int x, int y, int width, int height);
void window_label(int x, int y, const char* text, koi_uint32 color);
/* The same with KOI_TEXT_BOLD / ITALIC / UNDERLINE. */
void window_label_styled(int x, int y, const char* text, koi_uint32 color,
                         int style);

#define WINDOW_CHAR_W 8
#define WINDOW_CHAR_H 16
#define WINDOW_TITLE_H 22
#define WINDOW_MENU_H 20
#define WINDOW_BORDER 3
#define WINDOW_TASKBAR_H 28
#define WINDOW_TOPBAR_H 22

#endif
