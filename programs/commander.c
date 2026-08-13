#include "koi.h"
#include "editcore.h"
#include "settings.h"

/* Koi-Commander 1.0 - a shell you point at.
 *
 * Two panels, a pointer, and a wheel. The shape is Norton Commander's, and the
 * reason is not nostalgia: two directories side by side is the arrangement that
 * answers "where is it" and "where is it going" in one screen, and a machine
 * with one screen and no windows to overlap has nothing better to spend it on.
 * Windows 1.0 reached the same conclusion from the other direction - it tiled,
 * because overlapping windows on a screen this size hide more than they show.
 *
 * It is a program, not part of the system. It arrives with `dosget install
 * commander` and can be removed again, and the system underneath is exactly
 * the system without it. That is deliberate: a graphical shell that cannot be
 * taken off is an operating system with a graphical shell, and this one is not.
 *
 * It was called Mizu while it was the only graphical thing here. The name has
 * gone to what comes next - a desktop, with windows - and this kept the one
 * that describes it: a commander, in the line Norton started. Finished rather
 * than abandoned. Two panels answer "where is it" and "where is it going", and
 * a program that answers its question does not need to become something else.
 *
 * ---- Running things ------------------------------------------------------
 *
 * One program runs at a time, at a fixed address. So this cannot call a
 * program: it asks the shell to run one after it has exited, and to start it
 * again afterwards - koi_chain, twice, most recent first. Everything on screen
 * is gone in between, which is why the state that matters travels back as a
 * command line: the two paths, which panel was active, and where the bar was.
 *
 * Small DOS shells did exactly this, for exactly this reason.
 */

#define CHAR_W 8
#define CHAR_H 16
#define ROW_H 18
#define BAR_H 26
#define MARGIN 8
#define FRAME 2

#define NAME_MAX 32
#define PATH_MAX 128
#define ITEMS_MAX 512
#define LINE_MAX 256

/* Norton's colours, which are Norton's colours because they work: a dark blue
   field, light text on it, and one bar of reversed colour that the eye finds
   before it has finished looking. */
static koi_uint32 c_desktop;
static koi_uint32 c_panel;
static koi_uint32 c_frame;
static koi_uint32 c_frame_active;
static koi_uint32 c_text;
static koi_uint32 c_directory;
static koi_uint32 c_program;
static koi_uint32 c_select;
static koi_uint32 c_select_text;
static koi_uint32 c_bar;
static koi_uint32 c_bar_text;
static koi_uint32 c_bar_key;
static koi_uint32 c_shadow;

typedef struct {
    char name[NAME_MAX];
    unsigned int size;
    unsigned int attributes;
} ENTRY;

typedef struct {
    /* Each panel remembers its own drive.
     *
     * There is one current drive per program, so a panel that only stored a
     * path was showing that path on whichever drive the other panel had last
     * asked for - which is why changing drive changed both, and why changing
     * back could not work: nothing remembered where the other one had been. */
    int letter;
    char path[PATH_MAX];
    ENTRY items[ITEMS_MAX];
    int count;
    int top;               /* first entry drawn */
    int selected;
    int x, y, w, h;        /* where it sits on screen */
    int rows;              /* how many entries fit in it */
} PANEL;

static KOI_SCREEN screen;
static PANEL panels[2];
static int active;
static int running = 1;

/* Two drive letters, and the difference between them is the whole of a bug
 * that only appears with a USB stick plugged in.
 *
 * `home` is the drive Mizu itself was loaded from. `browse` is the drive being
 * looked at, which koi_setdrive moves without restarting anything. They are
 * usually the same and must not be assumed to be: a program started from the
 * panel is reached by a path on `browse`, and Mizu is reached by a path on
 * `home`, and running one command line on the wrong drive finds nothing. */
static int home_letter;
static int browse_letter;

/* Which drive this program's paths currently mean. */
static int drive_now(void) {
    long count = koi_sysinfo(KOI_INFO_VOLUME_COUNT, 0);

    for (long index = 0; index < count; index++)
        if (koi_sysinfo(KOI_INFO_VOLUME_IS_CURRENT, index) == 1)
            return (int)koi_sysinfo(KOI_INFO_VOLUME_LETTER, index);
    return 0;
}

/* ---- The pointer --------------------------------------------------------
 *
 * Drawn by hand, over whatever is underneath, with the pixels it covers kept so
 * they can be put back. The alternative - redrawing the whole screen every time
 * the pointer moves a pixel - is what makes a pointer feel like it is being
 * dragged through sand.
 */
#define CURSOR_W 12
#define CURSOR_H 19

static const char* cursor_shape[CURSOR_H] = {
    "o           ",
    "oo          ",
    "o*o         ",
    "o**o        ",
    "o***o       ",
    "o****o      ",
    "o*****o     ",
    "o******o    ",
    "o*******o   ",
    "o********o  ",
    "o*********o ",
    "o*****ooooo ",
    "o**o**o     ",
    "o*o o**o    ",
    "oo  o**o    ",
    "o    o**o   ",
    "     o**o   ",
    "      o*o   ",
    "       o    "
};

static koi_uint32 cursor_under[CURSOR_H][CURSOR_W];
static int cursor_x = -1;
static int cursor_y = -1;
static int cursor_saved;
static koi_uint32 cursor_ink;
static koi_uint32 cursor_edge;

static koi_uint32* pixel_row(int y) {
    return (koi_uint32*)((koi_uint8*)screen.pixels +
                         (koi_uint64)y * screen.pitch);
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

/* ---- Small things -------------------------------------------------------- */

static void text_at(int x, int y, const char* text, koi_uint32 color) {
    koi_gfx_text(x, y, text, color, KOI_TEXT_TRANSPARENT);
}

static int is_directory(const ENTRY* entry) {
    return (entry->attributes & KOI_ATTRIBUTE_DIRECTORY) != 0;
}

/* Does this name end in `suffix`? Case-insensitive, because FAT stores 8.3
   names upper-cased and nobody types them that way. */
static int ends_with(const char* name, const char* suffix) {
    koi_uint64 length = strlen(name);
    koi_uint64 tail = strlen(suffix);
    if (tail > length) return 0;
    for (koi_uint64 index = 0; index < tail; index++)
        if (toupper(name[length - tail + index]) != toupper(suffix[index]))
            return 0;
    return 1;
}

static int is_runnable(const ENTRY* entry) {
    if (is_directory(entry)) return 0;
    return ends_with(entry->name, ".EXE") || ends_with(entry->name, ".BAT");
}

/* Something the system knows how to open, even though it is not a program.
 *
 * Handled by chaining the command that opens it, exactly as if it were one, so
 * that there is one path through this and not two. A file manager that can
 * start programs and separately not-quite-start other things ends up with two
 * mechanisms and one of them rots. */
static const char* opener_for(const ENTRY* entry) {
    if (is_directory(entry)) return (const char*)0;
    if (ends_with(entry->name, ".WAV")) return "PLAY";
    return (const char*)0;
}

/* A size a person reads rather than counts. */
static void size_text(unsigned int bytes, char* out, koi_uint64 size) {
    if (bytes < 10000U) koi_snprintf(out, size, "%u", bytes);
    else if (bytes < 10000U * 1024U)
        koi_snprintf(out, size, "%uK", bytes / 1024U);
    else koi_snprintf(out, size, "%uM", bytes / (1024U * 1024U));
}

/* One row: the name padded out to `width`, then the size right-aligned in the
   six columns after it.
 *
 * Built by hand rather than with a format string. koi_snprintf takes a width,
 * but not a width passed as an argument - `%-*.*s` is not one of its
 * conversions, and what it does with one is print the stars. Which is exactly
 * what the first version of this drew: a panel of `½*.*S`, once per file,
 * perfectly aligned. */
static void row_text(char* out, koi_uint64 size, const char* name, int width,
                     const char* right) {
    koi_uint64 at = 0;
    koi_uint64 tail = strlen(right);

    while (name[at] && at < (koi_uint64)width && at + 1 < size)
        out[at] = name[at], at++;
    while (at < (koi_uint64)width && at + 1 < size) out[at++] = ' ';

    /* Six columns for the size, so that the numbers line up under each other
       and a directory's DIR sits where a size would be. */
    for (koi_uint64 pad = tail; pad < 7 && at + 1 < size; pad++) out[at++] = ' ';
    for (koi_uint64 index = 0; index < tail && at + 1 < size; index++)
        out[at++] = right[index];
    out[at] = 0;
}

/* Join a directory and a name into a path, without the doubled backslash that
   the root would otherwise produce. */
static void join_path(const char* directory, const char* name, char* out,
                      koi_uint64 size) {
    if (directory[0] == '\\' && directory[1] == 0)
        koi_snprintf(out, size, "\\%s", name);
    else
        koi_snprintf(out, size, "%s\\%s", directory, name);
}

/* ---- Reading a directory ------------------------------------------------- */

/* Directories first, then files, each alphabetically - which is the order every
   file manager has used since before this one, and the one where the eye knows
   where to start. Insertion sort: the lists are short and it is stable, so
   equal keys keep the order the filesystem gave them. */
static void sort_entries(PANEL* panel) {
    for (int index = 1; index < panel->count; index++) {
        ENTRY held = panel->items[index];
        int place = index - 1;

        while (place >= 0) {
            const ENTRY* other = &panel->items[place];
            int order;

            if (is_directory(other) != is_directory(&held))
                order = is_directory(&held) ? -1 : 1;
            else
                order = strcmp(held.name, other->name);
            if (order >= 0) break;
            panel->items[place + 1] = panel->items[place];
            place--;
        }
        panel->items[place + 1] = held;
    }
}

static void read_directory(PANEL* panel) {
    KOI_FIND_DATA found;
    char pattern[PATH_MAX + 4];
    long search;

    /* Stand on this panel's drive before listing it. Every path below is
       relative to whichever drive is current, so this is what makes two panels
       on two drives mean anything. */
    if (panel->letter && panel->letter != drive_now())
        koi_setdrive(panel->letter);

    panel->count = 0;
    panel->top = 0;

    join_path(panel->path, "*", pattern, sizeof(pattern));
    search = koi_findfirst(pattern, &found);
    if (search >= 0) {
        do {
            ENTRY* entry;
            /* `.` and `..` come back from the filesystem; the one that goes up
               is added below, in a known place, so that it is always the first
               row whether or not this directory happens to carry it. */
            if (found.name[0] == '.') continue;
            if (panel->count >= ITEMS_MAX) break;
            entry = &panel->items[panel->count++];
            strncpy(entry->name, found.name, NAME_MAX - 1);
            entry->name[NAME_MAX - 1] = 0;
            entry->size = found.size;
            entry->attributes = found.attributes;
        } while (koi_findnext(search, &found) == 0);
        koi_findclose(search);
    }

    sort_entries(panel);

    /* And the way back, first, unless this is the root. */
    if (!(panel->path[0] == '\\' && panel->path[1] == 0)) {
        if (panel->count >= ITEMS_MAX) panel->count = ITEMS_MAX - 1;
        for (int index = panel->count; index > 0; index--)
            panel->items[index] = panel->items[index - 1];
        panel->count++;
        strcpy(panel->items[0].name, "..");
        panel->items[0].size = 0;
        panel->items[0].attributes = KOI_ATTRIBUTE_DIRECTORY;
    }

    if (panel->selected >= panel->count) panel->selected = panel->count - 1;
    if (panel->selected < 0) panel->selected = 0;
}

/* Keep the selected row on screen, whichever end it walked off. */
static void follow_selection(PANEL* panel) {
    if (panel->selected < panel->top) panel->top = panel->selected;
    if (panel->selected >= panel->top + panel->rows)
        panel->top = panel->selected - panel->rows + 1;
    if (panel->top > panel->count - panel->rows)
        panel->top = panel->count - panel->rows;
    if (panel->top < 0) panel->top = 0;
}

/* ---- Drawing ------------------------------------------------------------- */

static void draw_panel(PANEL* panel, int is_active) {
    koi_uint32 border = is_active ? c_frame_active : c_frame;
    char line[LINE_MAX];
    int columns = (panel->w - 2 * MARGIN) / CHAR_W;
    int name_columns;

    if (columns > LINE_MAX - 1) columns = LINE_MAX - 1;
    name_columns = columns - 8;
    if (name_columns < 8) name_columns = 8;

    koi_gfx_fill(panel->x, panel->y, panel->w, panel->h, c_panel);
    koi_gfx_rect(panel->x, panel->y, panel->w, panel->h, border);
    koi_gfx_rect(panel->x + 1, panel->y + 1, panel->w - 2, panel->h - 2, border);

    /* The path, in the frame - which is where Norton put it, and it is the
       right place: it belongs to the panel and not to the screen. */
    koi_gfx_fill(panel->x + FRAME, panel->y + FRAME, panel->w - 2 * FRAME,
                 CHAR_H + 4, border);
    /* With the drive letter, because a file manager that can change drive and
       does not say which one it is on is showing you a directory listing with
       the most important word missing. */
    koi_snprintf(line, sizeof(line), " %c:%s ", (char)panel->letter,
                 panel->path);
    text_at(panel->x + MARGIN, panel->y + FRAME + 2, line,
            is_active ? c_select_text : c_text);

    for (int row = 0; row < panel->rows; row++) {
        int index = panel->top + row;
        int y = panel->y + FRAME + CHAR_H + 6 + row * ROW_H;
        const ENTRY* entry;
        koi_uint32 ink;
        char size[16];

        if (index >= panel->count) break;
        entry = &panel->items[index];

        if (index == panel->selected) {
            /* The bar is drawn on the inactive panel too, dimmed. A panel whose
               selection vanishes when it loses focus has lost the user's place
               in it, and it is still where they left it. */
            koi_gfx_fill(panel->x + FRAME + 2, y - 1, panel->w - 2 * FRAME - 4,
                         ROW_H, is_active ? c_select : c_shadow);
            ink = is_active ? c_select_text : c_text;
        } else if (is_directory(entry)) {
            ink = c_directory;
        } else if (is_runnable(entry)) {
            ink = c_program;
        } else {
            ink = c_text;
        }

        if (is_directory(entry)) {
            row_text(line, sizeof(line), entry->name, name_columns,
                     entry->name[0] == '.' ? "UP" : "DIR");
        } else {
            size_text(entry->size, size, sizeof(size));
            row_text(line, sizeof(line), entry->name, name_columns, size);
        }
        text_at(panel->x + MARGIN, y, line, ink);
    }

    /* How much of the list is showing, when not all of it is. Two fingers on a
       touchpad move this, and without something moving there is no way to tell
       a scroll that arrived from one that did not. */
    if (panel->count > panel->rows) {
        int track_x = panel->x + panel->w - FRAME - 6;
        int track_y = panel->y + FRAME + CHAR_H + 6;
        int track_h = panel->rows * ROW_H;
        int thumb_h = track_h * panel->rows / panel->count;
        int thumb_y = track_y + track_h * panel->top / panel->count;

        if (thumb_h < 8) thumb_h = 8;
        koi_gfx_fill(track_x, track_y, 4, track_h, c_shadow);
        koi_gfx_fill(track_x, thumb_y, 4, thumb_h, border);
    }
}

/* The bar along the bottom. The labels are the keys, and they are also the
   buttons - a shell for a machine that may or may not have a pointer must be
   usable either way, and the same row of words serves both. */
typedef struct {
    const char* key;
    const char* label;
    int code;
    int x, w;
} BUTTON;

static BUTTON buttons[] = {
    { "Tab", "Panel", '\t', 0, 0 },
    { "Enter", "Open", '\n', 0, 0 },
    { "F3", "View", KOI_KEY_F1 + 2, 0, 0 },
    { "F4", "Edit", KOI_KEY_F1 + 3, 0, 0 },
    { "F9", "Drive", KOI_KEY_F1 + 8, 0, 0 },
    { "F10", "Quit", KOI_KEY_F1 + 9, 0, 0 }
};

#define BUTTON_COUNT ((int)(sizeof(buttons) / sizeof(buttons[0])))

static void draw_bars(void) {
    char line[LINE_MAX];
    int y = (int)screen.height - BAR_H;
    int at = MARGIN;
    long free_kib = koi_sysinfo(KOI_INFO_MEMORY_FREE, 0);

    koi_gfx_fill(0, 0, (int)screen.width, BAR_H, c_bar);
    text_at(MARGIN, 5, "Koi-Commander 1.0", c_bar_text);

    /* The clock, in the corner, ticking.
     *
     * Windows 1.0 shipped one and everybody remembers it, and what made it
     * that clock was never the face - it was that it kept going next to
     * whatever you were doing. A clock that owns the whole screen is a
     * different object with the same hands. This is the corner it belongs in
     * until there are windows to put it in. */
    {
        long now = koi_sysinfo(KOI_INFO_TIME, 0);

        koi_snprintf(line, sizeof(line), "%02d:%02d:%02d   %ld KiB free",
                     KOI_TIME_HOUR(now), KOI_TIME_MINUTE(now),
                     KOI_TIME_SECOND(now), free_kib);
    }
    text_at((int)screen.width - MARGIN - (int)strlen(line) * CHAR_W, 5, line,
            c_bar_text);

    koi_gfx_fill(0, y, (int)screen.width, BAR_H, c_bar);
    for (int index = 0; index < BUTTON_COUNT; index++) {
        int width;

        koi_snprintf(line, sizeof(line), "%s %s", buttons[index].key,
                     buttons[index].label);
        width = (int)strlen(line) * CHAR_W + CHAR_W;
        buttons[index].x = at;
        buttons[index].w = width;

        text_at(at, y + 5, buttons[index].key, c_bar_key);
        text_at(at + (int)strlen(buttons[index].key) * CHAR_W + CHAR_W, y + 5,
                buttons[index].label, c_bar_text);
        at += width + CHAR_W;
    }
}

static void layout(void) {
    int top = BAR_H + MARGIN;
    int bottom = (int)screen.height - BAR_H - MARGIN;
    int width = ((int)screen.width - 3 * MARGIN) / 2;
    int height = bottom - top;

    for (int index = 0; index < 2; index++) {
        PANEL* panel = &panels[index];
        panel->x = MARGIN + index * (width + MARGIN);
        panel->y = top;
        panel->w = width;
        panel->h = height;
        panel->rows = (height - FRAME * 2 - CHAR_H - 8) / ROW_H;
        if (panel->rows < 1) panel->rows = 1;
    }
}

static void draw_all(void) {
    cursor_hide();
    koi_gfx_clear(c_desktop);
    draw_bars();
    for (int index = 0; index < 2; index++)
        draw_panel(&panels[index], index == active);
    koi_gfx_present();
}

/* ---- Looking at a file --------------------------------------------------- */

#define VIEW_BYTES (128U * 1024U)
#define VIEW_LINES 8192

/* Show a file, with the wheel to move through it.
 *
 * Reads a fixed amount and says so when there was more, rather than pretending
 * a large file is small. This is a viewer, not an editor: nothing it does can
 * change the file, which is what makes it safe to point at anything at all. */
static void view_file(const char* path) {
    long handle = koi_open(path, OPEN_READ);
    char* text;
    long got;
    static int line_at[VIEW_LINES];
    int lines = 0;
    int top = 0;
    int rows;
    int columns;
    int last_scroll;
    KOI_POINTER pointer;
    int truncated = 0;

    if (handle < 0) return;
    text = (char*)koi_alloc(VIEW_BYTES + 1);
    if (!text) { koi_close(handle); return; }

    got = koi_read(handle, text, VIEW_BYTES);
    if (got < 0) got = 0;
    if ((unsigned long)got == VIEW_BYTES) truncated = 1;
    text[got] = 0;
    koi_close(handle);

    /* An index of where each line starts, built once. Scrolling then costs
       nothing, which is the difference between a wheel that moves the text and
       one that thinks about it first. */
    line_at[lines++] = 0;
    for (long index = 0; index < got && lines < VIEW_LINES; index++) {
        if (text[index] == '\n') line_at[lines++] = (int)index + 1;
    }

    rows = ((int)screen.height - 2 * BAR_H - 2 * MARGIN) / ROW_H;
    columns = ((int)screen.width - 2 * MARGIN) / CHAR_W;
    if (columns > LINE_MAX - 1) columns = LINE_MAX - 1;

    koi_mouse(&pointer);
    last_scroll = pointer.scroll;

    for (;;) {
        char line[LINE_MAX];
        int y = BAR_H + MARGIN;

        cursor_hide();
        koi_gfx_clear(c_desktop);
        koi_gfx_fill(0, 0, (int)screen.width, BAR_H, c_bar);
        koi_snprintf(line, sizeof(line), "View  %s%s", path,
                     truncated ? "  (first 128 KiB)" : "");
        text_at(MARGIN, 5, line, c_bar_text);
        koi_gfx_fill(0, (int)screen.height - BAR_H, (int)screen.width, BAR_H,
                     c_bar);
        text_at(MARGIN, (int)screen.height - BAR_H + 5,
                "Wheel or arrows to move    Esc to close", c_bar_text);

        for (int row = 0; row < rows && top + row < lines; row++) {
            int start = line_at[top + row];
            int length = 0;

            while (text[start + length] && text[start + length] != '\n' &&
                   text[start + length] != '\r' && length < columns) {
                char character = text[start + length];
                /* A tab is drawn as a space rather than expanded. Expanding it
                   properly needs a column count this loop does not keep, and a
                   wrong tab stop is worse than none. */
                line[length] = (character == '\t') ? ' ' :
                               (isprint((unsigned char)character) ? character : '.');
                length++;
            }
            line[length] = 0;
            text_at(MARGIN, y + row * ROW_H, line, c_text);
        }
        koi_gfx_present();
        cursor_show(pointer.x, pointer.y);

        for (;;) {
            int moved = 0;
            int key = 0;

            koi_sleep(10);
            if (koi_keypressed()) key = koi_getchar();

            if (key == 27 || key == KOI_KEY_F1 + 9) {
                koi_free(text);
                return;
            }
            if (key == KOI_KEY_DOWN) { top++; moved = 1; }
            if (key == KOI_KEY_UP) { top--; moved = 1; }
            if (key == KOI_KEY_PAGE_DOWN) { top += rows; moved = 1; }
            if (key == KOI_KEY_PAGE_UP) { top -= rows; moved = 1; }
            if (key == KOI_KEY_HOME) { top = 0; moved = 1; }
            if (key == KOI_KEY_END) { top = lines - rows; moved = 1; }

            {
                int previous_x = pointer.x;
                int previous_y = pointer.y;

                koi_mouse(&pointer);
                if (pointer.scroll != last_scroll) {
                    top -= (pointer.scroll - last_scroll) * 3;
                    last_scroll = pointer.scroll;
                    moved = 1;
                }
                if (!moved && (pointer.x != previous_x ||
                               pointer.y != previous_y)) {
                    cursor_hide();
                    cursor_show(pointer.x, pointer.y);
                }
            }

            if (moved) {
                if (top > lines - rows) top = lines - rows;
                if (top < 0) top = 0;
                break;
            }
        }
    }
}

/* ---- Notepad -------------------------------------------------------------
 *
 * The other editor. `edit` is the system's - it ships with Koi-DOS, draws on
 * the console, and is there on a machine that has nothing else. This one comes
 * with Mizu, draws on the framebuffer, and can be pointed at: a click puts the
 * caret where the finger is and a drag selects, which is the whole reason a
 * graphical editor is worth having next to a console one.
 *
 * What it is not is a second implementation. Both are front ends over
 * editcore, which owns the buffer, the undo, the clipboard and the UTF-8
 * stepping - because a text buffer is where the off-by-ones live and two
 * copies written a week apart do not stay the same shape.
 */

#define NOTE_BYTES (256L * 1024L)

static EDITOR note;
static long note_top;
static long note_left;

/* The visible part of one line, and where in the buffer it starts.
 *
 * Horizontal scrolling counts characters, because that is what a column is;
 * everything after that counts bytes, because that is what is drawn. Keeping
 * the two apart is the difference between a caret that sits on the letter and
 * one that sits near it. */
static long note_row_text(long line, char* out, long limit, long* visible_from) {
    long start = edit_line_start(&note, line);
    long end = start + edit_line_length(&note, line);
    long at = start;
    long skipped = 0;
    long length = 0;

    while (skipped < note_left && at < end) {
        at++;
        while (at < end && ((unsigned char)note.text[at] & 0xC0) == 0x80) at++;
        skipped++;
    }
    if (visible_from) *visible_from = at;
    while (at < end && length < limit - 1) {
        char character = note.text[at++];
        out[length++] = (character == '\t') ? ' ' : character;
    }
    out[length] = 0;
    return length;
}

static void note_follow(int rows, int columns) {
    long line = edit_line_of(&note, note.cursor);
    long column = edit_column_of(&note, note.cursor);

    if (line < note_top) note_top = line;
    if (line >= note_top + rows) note_top = line - rows + 1;
    if (note_top < 0) note_top = 0;

    if (column < note_left) note_left = column;
    if (column >= note_left + columns) note_left = column - columns + 1;
    if (note_left < 0) note_left = 0;
}

static void note_draw(int rows, int columns, int text_y, const char* status) {
    char line[LINE_MAX];
    char label[LINE_MAX];
    long total = edit_lines(&note);
    long from;
    long to;

    edit_selection(&note, &from, &to);
    cursor_hide();
    koi_gfx_clear(c_desktop);

    koi_gfx_fill(0, 0, (int)screen.width, BAR_H, c_bar);
    koi_snprintf(label, sizeof(label), "Notepad  %s%s",
                 note.path[0] ? note.path : "(new file)",
                 note.modified ? "  *" : "");
    text_at(MARGIN, 5, label, c_bar_text);
    koi_snprintf(label, sizeof(label), "line %ld of %ld, col %ld",
                 edit_line_of(&note, note.cursor) + 1, total,
                 edit_column_of(&note, note.cursor) + 1);
    text_at((int)screen.width - MARGIN - (int)strlen(label) * CHAR_W, 5, label,
            c_bar_text);

    for (int row = 0; row < rows; row++) {
        long number = note_top + row;
        long visible_from;
        long length;
        int y = text_y + row * ROW_H;

        if (number >= total) break;
        length = note_row_text(number, line, LINE_MAX, &visible_from);

        /* The selection, as a block behind the text rather than a colour on
           it: a run of spaces is part of what was selected and has to look
           like it. */
        if (edit_has_selection(&note)) {
            long a = from - visible_from;
            long b = to - visible_from;

            if (a < 0) a = 0;
            if (b > length) b = length;
            /* A line wholly inside the selection ends with a newline that is
               selected too, and showing that as one extra cell is what tells
               somebody the line break went with it. */
            if (to > visible_from + length && from <= visible_from + length &&
                b == length && number + 1 < total)
                b = length + 1;
            if (b > a)
                koi_gfx_fill(MARGIN + (int)a * CHAR_W, y - 1,
                             (int)(b - a) * CHAR_W, CHAR_H + 2, c_select);
        }

        if (length > columns) line[columns] = 0;
        text_at(MARGIN, y, line, c_text);
    }

    /* The caret, steady rather than blinking. A blink means repainting the
       whole screen twice a second to move two pixels, and on a framebuffer
       that is a flicker in exchange for nothing: the caret is where the
       typing is going and the rest of the screen is not moving. */
    {
        long line_number = edit_line_of(&note, note.cursor);
        long visible_from;
        int row = (int)(line_number - note_top);

        if (row >= 0 && row < rows) {
            long length = note_row_text(line_number, line, LINE_MAX,
                                        &visible_from);
            long bytes = note.cursor - visible_from;

            if (bytes < 0) bytes = 0;
            if (bytes > length) bytes = length;
            koi_gfx_fill(MARGIN + (int)bytes * CHAR_W, text_y + row * ROW_H - 1,
                         2, CHAR_H + 2, c_text);
        }
    }

    koi_gfx_fill(0, (int)screen.height - BAR_H, (int)screen.width, BAR_H, c_bar);
    text_at(MARGIN, (int)screen.height - BAR_H + 5,
            status && status[0] ? status
            : "^S Save   ^Z Undo   ^C Copy  ^X Cut  ^V Paste   Esc Close",
            c_bar_text);
    koi_gfx_present();
}

/* Where in the buffer a point on the screen is. */
static long note_offset_at(int x, int y, int rows, int text_y) {
    char line[LINE_MAX];
    long visible_from;
    long length;
    long row = (y - text_y) / ROW_H;
    long column;
    long at;

    if (row < 0) row = 0;
    if (row >= rows) row = rows - 1;
    if (note_top + row >= edit_lines(&note)) row = edit_lines(&note) - 1 - note_top;
    if (row < 0) row = 0;

    length = note_row_text(note_top + row, line, LINE_MAX, &visible_from);
    column = (x - MARGIN + CHAR_W / 2) / CHAR_W;
    if (column < 0) column = 0;
    if (column > length) column = length;

    /* Landing in the middle of a UTF-8 character is landing between two halves
       of one letter, so step back to where the letter starts. */
    at = visible_from + column;
    while (at > visible_from &&
           ((unsigned char)note.text[at] & 0xC0) == 0x80) at--;
    return at;
}

/* Yes, no, or neither. Drawn where the key bar was, because that is where the
   eye already is and a dialogue box would need windows we do not have. */
static int note_confirm(const char* question) {
    text_at(MARGIN, (int)screen.height - BAR_H + 5, question, c_bar_key);
    koi_gfx_present();
    for (;;) {
        int key;
        koi_sleep(10);
        if (!koi_keypressed()) continue;
        key = koi_getchar();
        if (key == 'y' || key == 'Y') return 1;
        if (key == 'n' || key == 'N') return 0;
        if (key == 27) return -1;
    }
}

static void notepad(const char* path) {
    int rows;
    int columns;
    int text_y = BAR_H + MARGIN;
    int last_scroll;
    unsigned int last_press;
    int dragging = 0;
    char status[LINE_MAX];
    KOI_POINTER pointer;

    rows = ((int)screen.height - 2 * BAR_H - 2 * MARGIN) / ROW_H;
    columns = ((int)screen.width - 2 * MARGIN) / CHAR_W;
    if (columns > LINE_MAX - 1) columns = LINE_MAX - 1;

    /* A file that is not there yet is a new file with a name, not a refusal.
       That is what somebody means by opening one. */
    if (!edit_load(&note, path, NOTE_BYTES) && !edit_new(&note, NOTE_BYTES))
        return;
    if (!note.path[0]) {
        long at = 0;
        while (path[at] && at < EDIT_PATH_MAX - 1) { note.path[at] = path[at]; at++; }
        note.path[at] = 0;
    }
    note_top = 0;
    note_left = 0;
    status[0] = 0;

    koi_mouse(&pointer);
    last_scroll = pointer.scroll;
    last_press = pointer.presses[0];

    for (;;) {
        int key = 0;
        int marking;

        note_follow(rows, columns);
        note_draw(rows, columns, text_y, status);
        cursor_show(pointer.x, pointer.y);

        /* Wait for something to happen. Redrawing on a timer would repaint a
           still screen thirty times a second for nothing. */
        for (;;) {
            int previous_x = pointer.x;
            int previous_y = pointer.y;
            int acted = 0;

            koi_sleep(10);
            if (koi_keypressed()) { key = koi_getchar(); break; }

            koi_mouse(&pointer);
            if (pointer.scroll != last_scroll) {
                note_top -= (pointer.scroll - last_scroll) * 3;
                last_scroll = pointer.scroll;
                if (note_top > edit_lines(&note) - 1) note_top = edit_lines(&note) - 1;
                if (note_top < 0) note_top = 0;
                acted = 1;
            }
            if (pointer.presses[0] != last_press) {
                last_press = pointer.presses[0];
                if (pointer.y > BAR_H &&
                    pointer.y < (int)screen.height - BAR_H) {
                    edit_move_to(&note,
                                 note_offset_at(pointer.x, pointer.y, rows, text_y),
                                 0);
                    dragging = 1;
                    acted = 1;
                }
            }
            if (dragging && (pointer.buttons & KOI_BUTTON_LEFT) &&
                (pointer.x != previous_x || pointer.y != previous_y)) {
                edit_move_to(&note,
                             note_offset_at(pointer.x, pointer.y, rows, text_y), 1);
                acted = 1;
            }
            if (dragging && !(pointer.buttons & KOI_BUTTON_LEFT)) dragging = 0;

            if (acted) break;
            if (pointer.x != previous_x || pointer.y != previous_y) {
                cursor_hide();
                cursor_show(pointer.x, pointer.y);
            }
        }

        if (!key) { status[0] = 0; continue; }
        status[0] = 0;
        /* Shift is not in a character stream, so a selection made from the
           keyboard grows only while one is already going - which is what the
           mouse leaves behind after a drag. */
        marking = 0;

        if (key == 27) {
            if (note.modified) {
                int answer = note_confirm("Save changes before closing?  Y / N / Esc");
                if (answer < 0) continue;
                if (answer && !edit_save(&note, note.path)) {
                    koi_snprintf(status, sizeof(status), "Could not write %s",
                                 note.path);
                    continue;
                }
            }
            break;
        }
        if (key == 19) {            /* ^S */
            koi_snprintf(status, sizeof(status),
                         edit_save(&note, note.path) ? "Saved." : "Could not save.");
            continue;
        }
        if (key == 26) { edit_undo(&note); continue; }                  /* ^Z */
        if (key == 1) { edit_select_all(&note); continue; }             /* ^A */
        if (key == 3) {                                                 /* ^C */
            koi_snprintf(status, sizeof(status),
                         edit_copy(&note) > 0 ? "Copied." : "Nothing selected.");
            continue;
        }
        if (key == 24) {                                                /* ^X */
            koi_snprintf(status, sizeof(status),
                         edit_cut(&note) > 0 ? "Cut." : "Nothing selected.");
            continue;
        }
        if (key == 22) {                                                /* ^V */
            koi_snprintf(status, sizeof(status),
                         edit_paste(&note) > 0 ? "Pasted." : "The clipboard is empty.");
            continue;
        }

        switch (key) {
        case KOI_KEY_LEFT:  edit_move_by(&note, -1, marking); break;
        case KOI_KEY_RIGHT: edit_move_by(&note, 1, marking); break;
        case KOI_KEY_UP:    edit_move_lines(&note, -1, marking); break;
        case KOI_KEY_DOWN:  edit_move_lines(&note, 1, marking); break;
        case KOI_KEY_PAGE_UP:   edit_move_lines(&note, -rows, marking); break;
        case KOI_KEY_PAGE_DOWN: edit_move_lines(&note, rows, marking); break;
        case KOI_KEY_HOME:  edit_move_home(&note, marking); break;
        case KOI_KEY_END:   edit_move_end(&note, marking); break;
        case KOI_KEY_DELETE: edit_delete(&note); break;
        case '\b': edit_backspace(&note); break;
        case '\n': case '\r': edit_insert_char(&note, '\n'); break;
        case '\t': edit_insert(&note, "    ", 4); break;
        default:
            if (key >= ' ' && key < 0x100) edit_insert_char(&note, (char)key);
            break;
        }
    }

    edit_close(&note);
}

/* ---- Doing something with the selected entry ----------------------------- */

/* The command line that brings this program back where it was. */
static void own_command(char* out, koi_uint64 size) {
    char self[PATH_MAX];

    if (koi_systext(KOI_TEXT_PROGRAM_PATH, 0, self, sizeof(self)) <= 0)
        strcpy(self, "\\COMMANDER\\COMMANDER.EXE");
    /* Both letters, not one. A single letter came back and was applied to
       everything, so a panel that had been left on another drive returned
       showing that drive's path on this one - a listing of a directory that
       was never asked for, under a title that named the wrong disk. */
    koi_snprintf(out, size, "%s %s %s %d %d %d %c %c", self, panels[0].path,
                 panels[1].path, active, panels[0].selected,
                 panels[1].selected, (char)panels[0].letter,
                 (char)panels[1].letter);
}

/* Run a program: ask for it, ask for this shell after it, and leave.
 *
 * The order looks backwards and is not. Requests are honoured most recent
 * first, so the one asked for last is the one that runs first.
 *
 * The drive changes are the part that is easy to leave out and impossible to
 * notice missing until somebody plugs in a USB stick. koi_setdrive moves where
 * THIS program's paths point; it does not move the shell, and the shell is what
 * runs both of these command lines. So the program is reached on the drive
 * being browsed, and Mizu is reached on the drive Mizu lives on, and when those
 * differ the shell has to be walked from one to the other and back.
 *
 * Reading downwards, the requests run upwards:
 *
 *     browse:      change the shell to the drive being looked at
 *     the program  which is on that drive
 *     home:        change back to where Mizu lives
 *     MIZU ...     which is on that one
 */
static void run_entry(const char* path) {
    char resume[PATH_MAX * 3];
    char drive[8];
    /* The drive of the panel the file is in, which is not "the drive Mizu is
       on" now that the two panels can be on different ones. */
    int where = panels[active].letter ? panels[active].letter : home_letter;
    int elsewhere = where != home_letter;

    own_command(resume, sizeof(resume));
    koi_chain(resume);
    if (elsewhere) {
        koi_snprintf(drive, sizeof(drive), "%c:", (char)home_letter);
        koi_chain(drive);
    }
    koi_chain(path);
    if (elsewhere) {
        koi_snprintf(drive, sizeof(drive), "%c:", (char)where);
        koi_chain(drive);
    }
    cursor_hide();
    koi_gfx_leave();
    koi_exit(0);
}

static void open_selected(void) {
    PANEL* panel = &panels[active];
    const ENTRY* entry;
    char path[PATH_MAX];

    if (!panel->count) return;
    entry = &panel->items[panel->selected];

    if (is_directory(entry)) {
        if (strcmp(entry->name, "..") == 0) {
            /* Up: cut the last component off, and never past the root. */
            int end = (int)strlen(panel->path);
            while (end > 0 && panel->path[end - 1] != '\\') end--;
            if (end > 1) end--;
            if (end < 1) end = 1;
            panel->path[end] = 0;
            if (!panel->path[0]) strcpy(panel->path, "\\");
        } else {
            char joined[PATH_MAX];
            join_path(panel->path, entry->name, joined, sizeof(joined));
            if (strlen(joined) >= PATH_MAX - 1) return;
            strcpy(panel->path, joined);
        }
        panel->selected = 0;
        panel->top = 0;
        read_directory(panel);
        draw_all();
        return;
    }

    join_path(panel->path, entry->name, path, sizeof(path));
    if (is_runnable(entry)) {
        run_entry(path);
    } else {
        const char* opener = opener_for(entry);

        if (opener) {
            char command[PATH_MAX + 16];
            koi_snprintf(command, sizeof(command), "%s %s", opener, path);
            run_entry(command);
        } else {
            view_file(path);
            draw_all();
        }
    }
}

/* Change drive.
 *
 * This used to ask the shell to change drive and then restart Mizu - and a
 * program that does that is asking to be restarted from a drive it has just
 * left. It changed to the USB stick and could not find itself:
 *
 *     Bad command or file name: \MIZU\mizu.EXE \ \ 1 0 0
 *
 * Perfectly correct behaviour from the shell, and an impossible request. Now
 * koi_setdrive moves where this program's paths point, without restarting
 * anything and without moving the shell, so nothing has to be found twice. */
static void change_drive(void) {
    long count = koi_sysinfo(KOI_INFO_VOLUME_COUNT, 0);
    long here = -1;
    int letter;

    /* Where this panel is now, not where the program last happened to be. */
    browse_letter = panels[active].letter ? panels[active].letter : drive_now();
    if (count < 2) return;
    for (long index = 0; index < count; index++)
        if (koi_sysinfo(KOI_INFO_VOLUME_LETTER, index) == browse_letter)
            here = index;
    if (here < 0) return;

    /* Round the list until a volume with a letter turns up.
     *
     * Not every volume has one: the loader's partition deliberately has none,
     * so nothing can `del` its way into the files the machine starts from.
     * Stepping to the next entry and giving up when it had no letter is what
     * made the drive change one-way - Z: to Y: worked because Y: was next,
     * and Y: back to Z: did not because the letterless one was in between. */
    letter = 0;
    for (long step = 1; step <= count; step++) {
        int candidate = (int)koi_sysinfo(KOI_INFO_VOLUME_LETTER,
                                         (here + step) % count);
        if (candidate > 0 && candidate != browse_letter) {
            letter = candidate;
            break;
        }
    }
    if (letter <= 0 || koi_setdrive(letter) != 1) return;
    browse_letter = letter;

    /* Only the panel being looked at. The other one keeps its drive and its
       path, which is the entire point of having two: copying from one drive to
       another is impossible when both always show the same one. */
    {
        PANEL* panel = &panels[active];
        panel->letter = letter;
        strcpy(panel->path, "\\");
        panel->selected = 0;
        panel->top = 0;
        read_directory(panel);
    }
    draw_all();
}

/* ---- Input --------------------------------------------------------------- */

static void move_selection(int by) {
    PANEL* panel = &panels[active];

    if (!panel->count) return;
    panel->selected += by;
    if (panel->selected < 0) panel->selected = 0;
    if (panel->selected >= panel->count) panel->selected = panel->count - 1;
    follow_selection(panel);
}

static void act_on(int code) {
    switch (code) {
    case '\t':
        active = active ? 0 : 1;
        draw_all();
        break;
    case '\n':
        open_selected();
        break;
    case KOI_KEY_F1 + 2: {
        PANEL* panel = &panels[active];
        char path[PATH_MAX];
        if (!panel->count || is_directory(&panel->items[panel->selected])) break;
        join_path(panel->path, panel->items[panel->selected].name, path,
                  sizeof(path));
        view_file(path);
        draw_all();
        break;
    }
    case KOI_KEY_F1 + 3: {
        PANEL* panel = &panels[active];
        char path[PATH_MAX];
        if (!panel->count || is_directory(&panel->items[panel->selected])) break;
        join_path(panel->path, panel->items[panel->selected].name, path,
                  sizeof(path));
        notepad(path);
        /* The file may be a different size now, and the panel is showing what
           it was before. */
        read_directory(panel);
        draw_all();
        break;
    }
    case KOI_KEY_F1 + 8:
        change_drive();
        break;
    case KOI_KEY_F1 + 9:
        running = 0;
        break;
    default:
        break;
    }
}

/* Which panel is the pointer over, or -1. */
static int panel_under(int x, int y) {
    for (int index = 0; index < 2; index++) {
        PANEL* panel = &panels[index];
        if (x >= panel->x && x < panel->x + panel->w &&
            y >= panel->y && y < panel->y + panel->h) return index;
    }
    return -1;
}

/* Which row of a panel a point falls on, or -1. */
static int row_under(const PANEL* panel, int y) {
    int first = panel->y + FRAME + CHAR_H + 6;
    int row;

    if (y < first) return -1;
    row = (y - first) / ROW_H;
    if (row < 0 || row >= panel->rows) return -1;
    if (panel->top + row >= panel->count) return -1;
    return panel->top + row;
}

int main(void) {
    KOI_POINTER pointer;
    int last_scroll;
    unsigned int last_presses = 0;
    int last_x = -1;
    int last_y = -1;
    koi_uint64 last_click = 0;
    int last_clicked_row = -1;
    long last_clock = -1;

    /* Where it was, if it has been here before. The shell has just restarted
       this program after running something else, and everything it knew is in
       these arguments. */
    strcpy(panels[0].path, "\\");
    strcpy(panels[1].path, "\\");
    /* The drive this program came from, before anything moves. Every later
       command line aimed at it has to be run on this one. */
    home_letter = drive_now();
    browse_letter = home_letter;

    /* The first time, ask the questions before showing anything.
     *
     * A package that arrives over the wire and then sits waiting to be typed
     * has installed half of itself; the other half is the handful of decisions
     * only the person at the machine can make, and the moment for them is the
     * first time it is started. Asked once: the answer is a line in the
     * settings file, and CMDRCFG can be run again by anybody who wants to
     * change it.
     *
     * Done by asking the shell to run both - the configuration first, this
     * program after it - because one program runs at a time and this one has
     * not taken the screen yet. */
    {
        char configured[16];

        if (!koi_arguments()[0] &&
            (!settings_get("CMDR", "configured", configured,
                           sizeof(configured)) || configured[0] != '1')) {
            char self[PATH_MAX];
            char config[PATH_MAX];

            if (koi_systext(KOI_TEXT_PROGRAM_PATH, 0, self, sizeof(self)) <= 0)
                strcpy(self, "\\COMMANDER\\COMMANDER.EXE");
            strcpy(config, self);
            {
                /* CMDRCFG.EXE, beside whatever this turned out to be called. */
                int cut = 0;
                for (int index = 0; config[index]; index++)
                    if (config[index] == '\\') cut = index + 1;
                config[cut] = 0;
                strcpy(config + cut, "CMDRCFG.EXE");
            }
            koi_chain(self);
            koi_chain(config);
            return 0;
        }
    }
    {
        const char* arguments = koi_arguments();
        char word[PATH_MAX];
        int field = 0;

        while (*arguments && field < 7) {
            int length = 0;
            while (*arguments == ' ') arguments++;
            if (!*arguments) break;
            while (*arguments && *arguments != ' ' && length < PATH_MAX - 1)
                word[length++] = *arguments++;
            word[length] = 0;

            if (field == 0) strcpy(panels[0].path, word);
            else if (field == 1) strcpy(panels[1].path, word);
            else if (field == 2) active = atoi(word) ? 1 : 0;
            else if (field == 3) panels[0].selected = atoi(word);
            else if (field == 4) panels[1].selected = atoi(word);
            else if (field == 5 || field == 6) {
                /* Back to the drive each panel was looking at. If it has gone -
                   the stick was pulled out while a program was running - that
                   panel goes home rather than showing an empty screen for a
                   volume that is not there. */
                PANEL* panel = &panels[field - 5];
                int letter = toupper((unsigned char)word[0]);

                if (letter && koi_setdrive(letter) == 1) {
                    panel->letter = letter;
                } else {
                    panel->letter = home_letter;
                    strcpy(panel->path, "\\");
                    panel->selected = 0;
                }
            }
            field++;
        }
    }

    if (koi_gfx_enter(&screen) != 0) {
        koi_print("Mizu needs a screen it can draw on, and there is none.\n");
        return 1;
    }

    c_desktop = koi_gfx_color(0, 24, 64);
    c_panel = koi_gfx_color(0, 40, 104);
    c_frame = koi_gfx_color(90, 150, 200);
    c_frame_active = koi_gfx_color(150, 220, 255);
    c_text = koi_gfx_color(210, 230, 250);
    c_directory = koi_gfx_color(255, 235, 150);
    c_program = koi_gfx_color(150, 255, 190);
    c_select = koi_gfx_color(150, 220, 255);
    c_select_text = koi_gfx_color(0, 24, 64);
    c_bar = koi_gfx_color(0, 16, 40);
    c_bar_text = koi_gfx_color(200, 225, 245);
    c_bar_key = koi_gfx_color(255, 220, 120);
    c_shadow = koi_gfx_color(0, 32, 80);
    cursor_ink = koi_gfx_color(255, 255, 255);
    cursor_edge = koi_gfx_color(0, 0, 0);

    /* Whatever drive each panel was on when the last command line was built;
       the drive Mizu is standing on now for a panel that has no answer yet. */
    for (int index = 0; index < 2; index++)
        if (!panels[index].letter) panels[index].letter = browse_letter;

    layout();
    read_directory(&panels[0]);
    read_directory(&panels[1]);
    follow_selection(&panels[0]);
    follow_selection(&panels[1]);
    draw_all();

    if (!koi_mouse(&pointer)) {
        /* No pointer is not a reason to refuse. Everything here has a key, and
           on a machine with no touchpad and no mouse that is the whole of the
           interface rather than a degraded version of one. */
        pointer.x = (int)screen.width / 2;
        pointer.y = (int)screen.height / 2;
        pointer.scroll = 0;
        pointer.presses[0] = 0;
    } else {
        koi_mouse_place((int)screen.width / 2, (int)screen.height / 2);
        koi_mouse(&pointer);
        cursor_show(pointer.x, pointer.y);
    }
    last_scroll = pointer.scroll;
    last_presses = pointer.presses[0];
    last_x = pointer.x;
    last_y = pointer.y;

    while (running) {
        int redraw = 0;

        koi_sleep(8);

        while (koi_keypressed()) {
            int key = koi_getchar();

            /* Escape does not leave. F10 does, and it says so along the
               bottom of the screen; Escape sat next to it as an undocumented
               second way out, which is a key people press to get out of
               something smaller and then find they are back at the prompt. It
               still closes the viewer and the editor, where getting out of
               something smaller is exactly what it means. */
            if (key == KOI_KEY_UP) { move_selection(-1); redraw = 1; }
            else if (key == KOI_KEY_DOWN) { move_selection(1); redraw = 1; }
            else if (key == KOI_KEY_PAGE_UP) {
                move_selection(-panels[active].rows);
                redraw = 1;
            } else if (key == KOI_KEY_PAGE_DOWN) {
                move_selection(panels[active].rows);
                redraw = 1;
            } else if (key == KOI_KEY_HOME) {
                panels[active].selected = 0;
                follow_selection(&panels[active]);
                redraw = 1;
            } else if (key == KOI_KEY_END) {
                panels[active].selected = panels[active].count - 1;
                follow_selection(&panels[active]);
                redraw = 1;
            } else if (key == KOI_KEY_LEFT && active == 1) {
                active = 0;
                redraw = 1;
            } else if (key == KOI_KEY_RIGHT && active == 0) {
                active = 1;
                redraw = 1;
            } else {
                act_on(key);
            }
        }
        if (!running) break;

        koi_mouse(&pointer);

        /* The wheel moves whichever panel is under the pointer, not the active
           one. Scrolling a list you are not pointing at is nobody's idea of
           what a wheel does. */
        if (pointer.scroll != last_scroll) {
            int over = panel_under(pointer.x, pointer.y);
            PANEL* panel = &panels[over < 0 ? active : over];
            int by = (pointer.scroll - last_scroll) * 3;

            last_scroll = pointer.scroll;
            panel->top -= by;
            if (panel->top > panel->count - panel->rows)
                panel->top = panel->count - panel->rows;
            if (panel->top < 0) panel->top = 0;
            redraw = 1;
        }

        /* Presses, not the button's current state.
         *
         * Watching the state means holding the button does one thing per pass
         * round this loop, and - the half that actually bites - a click shorter
         * than the gap between two passes is never seen at all. The count is
         * kept by the driver as the packets arrive, so a click that happened
         * between two looks is still there to be found. */
        if (pointer.presses[0] != last_presses) {
            int over = panel_under(pointer.x, pointer.y);
            /* How many, not whether. Two clicks fast enough to land between the
               same pair of looks arrive as one jump of two, and collapsing that
               into a single click is a double click that only works when it is
               done slowly. */
            unsigned int clicks = pointer.presses[0] - last_presses;

            last_presses = pointer.presses[0];

            if (over >= 0) {
                int row = row_under(&panels[over], pointer.y);

                if (row >= 0) {
                    koi_uint64 now = koi_uptime();
                    int again = clicks > 1 ||
                                (over == active && row == last_clicked_row &&
                                 now - last_click < 400);

                    active = over;
                    panels[over].selected = row;
                    last_click = now;
                    last_clicked_row = row;
                    redraw = 1;
                    if (again) {
                        /* Twice, quickly, on the same row - which is how
                           everything with a pointer has meant "open" since
                           before this system existed. */
                        open_selected();
                        redraw = 0;
                        last_clicked_row = -1;
                    }
                } else if (over != active) {
                    active = over;
                    redraw = 1;
                }
            } else if (pointer.y >= (int)screen.height - BAR_H) {
                for (int index = 0; index < BUTTON_COUNT; index++) {
                    if (pointer.x >= buttons[index].x &&
                        pointer.x < buttons[index].x + buttons[index].w) {
                        act_on(buttons[index].code);
                        redraw = 1;
                        break;
                    }
                }
            }
        }
        /* One second passing is an event like any other. Only the bar is
           redrawn and only the bar is presented - a clock that repainted two
           panels every second would be a clock that made the machine feel
           slow, which is the opposite of what it is for. */
        if (!redraw) {
            long now = koi_sysinfo(KOI_INFO_TIME, 0);

            if (now != last_clock) {
                last_clock = now;
                cursor_hide();
                draw_bars();
                koi_gfx_present_rect(0, 0, (int)screen.width, BAR_H);
                cursor_show(pointer.x, pointer.y);
                last_x = pointer.x;
                last_y = pointer.y;
            }
        }

        if (redraw) {
            draw_all();
            cursor_show(pointer.x, pointer.y);
            last_x = pointer.x;
            last_y = pointer.y;
        } else if (pointer.x != last_x || pointer.y != last_y) {
            cursor_hide();
            cursor_show(pointer.x, pointer.y);
            last_x = pointer.x;
            last_y = pointer.y;
        }
    }

    cursor_hide();
    koi_gfx_leave();
    return 0;
}
