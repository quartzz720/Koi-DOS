#include "koi.h"
#include "window.h"
#include "editcore.h"
#include "settings.h"
#include "language.h"

/* Mizu 0.5 - the desktop.
 *
 * The name was the file manager's while that was the only graphical thing
 * here. The file manager is finished and is called Koi-Commander now; this is
 * what the name was being kept for.
 *
 * Windows 3.0's shape, water's colours. What it is not yet is honest to say
 * plainly: the windows here are parts of this program, because Koi-DOS holds
 * one program in memory at a time. Windows 1.0 through 3.0 in real mode were
 * exactly that too, and their bundled applications were parts of one image for
 * the same reason. When a second program can be resident, the frames and the
 * ordering below do not change - only who supplies the paint.
 */

#define MENU_ABOUT 1
#define MENU_EXIT 2
#define MENU_CONTROL 3
#define MENU_CLOCK 4
#define MENU_COMMANDER 5
#define MENU_TILE 6
#define MENU_NOTE 7
#define MENU_BOLD 8
#define MENU_ITALIC 9
#define MENU_UNDERLINE 10
#define MENU_PLAIN 11
#define MENU_SAVE 12

static WINDOW* control_window;
static WINDOW* clock_window;
static WINDOW* about_window;
static WINDOW* note_window;


/* ---- The control panel ---------------------------------------------------
 *
 * Program Manager's grid, which is the right shape for a machine with no
 * overlapping-window habits yet: a page of things you can start, each one a
 * picture and a word.
 */
typedef struct {
    const char* name;
    const char* note;
    koi_uint32 (*tint)(void);
} ENTRY;

static koi_uint32 tint_setup(void) { return koi_gfx_color(0x4A, 0x8F, 0xB8); }
static koi_uint32 tint_files(void) { return koi_gfx_color(0x35, 0xA6, 0xC4); }
static koi_uint32 tint_tools(void) { return koi_gfx_color(0x58, 0xB0, 0xA8); }

static ENTRY entries[4];

static void name_entries(void) {
    entries[0] = (ENTRY){ say(SAY_COMMANDER), say(SAY_TWO_PANELS), tint_files };
    entries[1] = (ENTRY){ say(SAY_NOTEEDIT), say(SAY_WRITE_TEXT), tint_tools };
    entries[2] = (ENTRY){ say(SAY_CLOCK), say(SAY_AND_A_DATE), tint_setup };
    entries[3] = (ENTRY){ say(SAY_ABOUT), say(SAY_THIS_SYSTEM), tint_tools };
}
#define ENTRY_COUNT 4

#define ICON_W 120
#define ICON_H 76

/* An icon, drawn rather than loaded. A picture would be a file to ship and a
   format to decode; a rounded tile with a drop in it is three rectangles and
   says the same thing at this size. */
static void draw_icon(int x, int y, koi_uint32 tint) {
    koi_gfx_fill(x + 14, y + 6, 36, 30, tint);
    koi_gfx_fill(x + 14, y + 6, 36, 6,
                 koi_gfx_color(0xFF, 0xFF, 0xFF));
    koi_gfx_rect(x + 14, y + 6, 36, 30, window_shadow);
    /* The drop: a small square with its top corners taken off, which at eight
       pixels is as much water as anybody can see. */
    koi_gfx_fill(x + 28, y + 16, 8, 10, window_client_paper);
    koi_gfx_line(x + 31, y + 13, x + 31, y + 15, window_client_paper);
    koi_gfx_rect(x + 28, y + 16, 8, 10, tint);
}

static void paint_control(WINDOW* window, int x, int y, int width, int height) {
    (void)window;
    (void)height;
    for (int index = 0; index < ENTRY_COUNT; index++) {
        int column = index % (width / ICON_W ? width / ICON_W : 1);
        int row = index / (width / ICON_W ? width / ICON_W : 1);
        int ix = x + 8 + column * ICON_W;
        int iy = y + 8 + row * ICON_H;
        int text_x;

        draw_icon(ix + (ICON_W - 8) / 2 - 32, iy, entries[index].tint());
        /* Centred in the cell and clipped to it. A translated label is longer
           than the English one it replaced - "Панель керування" against
           "Control Panel" - and a label measured against the icon rather than
           against the cell runs into its neighbour. */
        text_x = ix + (ICON_W - 8 - language_columns(entries[index].name) *
                       WINDOW_CHAR_W) / 2;
        if (text_x < ix) text_x = ix;
        window_label(text_x, iy + 40, entries[index].name, window_text);
        text_x = ix + (ICON_W - 8 - language_columns(entries[index].note) *
                       WINDOW_CHAR_W) / 2;
        if (text_x < ix) text_x = ix;
        window_label(text_x, iy + 56, entries[index].note, window_shadow);
    }
}

static void open_about(void);
static void open_clock(void);
static void open_note(void);
static void start_commander(void);

static void click_control(WINDOW* window, int x, int y, int clicks) {
    int columns;
    int index;
    int client_x, client_y, client_w, client_h;

    (void)window;
    window_client(control_window, &client_x, &client_y, &client_w, &client_h);
    columns = client_w / ICON_W;
    if (columns < 1) columns = 1;
    index = (y - 8) / ICON_H * columns + (x - 8) / ICON_W;
    if (index < 0 || index >= ENTRY_COUNT) return;
    /* Twice, as Program Manager had it: one click to point at a thing and two
       to set it going, so a hand resting on the button does not launch it. */
    if (clicks < 2) return;

    if (index == 0) start_commander();
    else if (index == 1) open_note();
    else if (index == 2) open_clock();
    else open_about();
}

/* ---- The clock ----------------------------------------------------------- */

/* Which weekday the first of a month falls on, 0 Sunday. Zeller's, because a
   table of month lengths and a running count is the same arithmetic written
   out longer and wrong in February. */
static int first_weekday(int year, int month) {
    int shift_month = month;
    int shift_year = year;
    int century;

    if (shift_month < 3) { shift_month += 12; shift_year--; }
    century = shift_year / 100;
    shift_year %= 100;
    return (1 + (13 * (shift_month + 1)) / 5 + shift_year + shift_year / 4 +
            century / 4 + 5 * century) % 7;
}

static int month_length(int year, int month) {
    static const int lengths[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return lengths[month - 1];
}

static void paint_clock(WINDOW* window, int x, int y, int width, int height) {
    static const char* days[] = { "Su","Mo","Tu","We","Th","Fr","Sa" };
    long now = koi_sysinfo(KOI_INFO_TIME, 0);
    long today = koi_sysinfo(KOI_INFO_DATE, 0);
    int year = KOI_DATE_YEAR(today);
    int month = KOI_DATE_MONTH(today);
    int day = KOI_DATE_DAY(today);
    int start = first_weekday(year, month);
    int length = month_length(year, month);
    char line[64];
    int cell = 24;
    int left;

    (void)window;
    (void)height;

    koi_snprintf(line, sizeof(line), "%02d:%02d:%02d   %04d-%02d-%02d",
                 KOI_TIME_HOUR(now), KOI_TIME_MINUTE(now), KOI_TIME_SECOND(now),
                 year, month, day);
    window_label(x + (width - (int)strlen(line) * WINDOW_CHAR_W) / 2, y + 6,
                 line, window_text);

    left = x + (width - 7 * cell) / 2;
    for (int index = 0; index < 7; index++)
        window_label(left + index * cell + 4, y + 30, days[index], window_shadow);

    for (int number = 1; number <= length; number++) {
        int slot = start + number - 1;
        int cx = left + (slot % 7) * cell;
        int cy = y + 50 + (slot / 7) * 20;

        koi_snprintf(line, sizeof(line), "%d", number);
        if (number == day) {
            koi_gfx_fill(cx, cy - 2, cell - 2, 18, window_accent);
            window_label(cx + (number < 10 ? 8 : 4), cy, line,
                         window_client_paper);
        } else {
            window_label(cx + (number < 10 ? 8 : 4), cy, line, window_text);
        }
    }
}

/* ---- NoteEdit ------------------------------------------------------------
 *
 * The same editing core the console editor uses, drawn into a window instead
 * of onto a terminal. One buffer implementation, two front ends, for the
 * reason it was split out in the first place: a text buffer is where the
 * off-by-ones live and two copies written a week apart do not stay the same
 * shape.
 *
 * The style is the whole document's, which is not a shortcut - it is what
 * Notepad did, and for the same reason. A style that varies inside the text
 * needs a second buffer running alongside it saying where each run begins and
 * ends, and a plain text file has nowhere to keep that. The moment there is a
 * format that can, this becomes MizuWriter and the runs go in it.
 */
#define NOTE_CAPACITY (64L * 1024L)
#define NOTE_PATH "\\NOTE.TXT"

static EDITOR note;
static int note_ready;
static int note_style;
static long note_top_line;

static void paint_note(WINDOW* window, int x, int y, int width, int height) {
    long total = edit_lines(&note);
    long caret_line = edit_line_of(&note, note.cursor);
    int rows = height / WINDOW_CHAR_H;
    int columns = width / WINDOW_CHAR_W;

    (void)window;
    if (rows < 1) rows = 1;

    /* Keep the caret in view before drawing anything, so the first frame after
       a keystroke already shows where it went. */
    if (caret_line < note_top_line) note_top_line = caret_line;
    if (caret_line >= note_top_line + rows) note_top_line = caret_line - rows + 1;
    if (note_top_line < 0) note_top_line = 0;

    for (int row = 0; row < rows && note_top_line + row < total; row++) {
        long number = note_top_line + row;
        long start = edit_line_start(&note, number);
        long length = edit_line_length(&note, number);
        char line[256];
        long copied = 0;

        while (copied < length && copied < columns && copied < 255) {
            char character = note.text[start + copied];
            line[copied] = (character == '\t') ? ' ' : character;
            copied++;
        }
        line[copied] = 0;
        window_label_styled(x + 2, y + row * WINDOW_CHAR_H, line, window_text,
                            note_style);
    }

    {
        int row = (int)(caret_line - note_top_line);
        long column = note.cursor - edit_line_start(&note, caret_line);
        if (row >= 0 && row < rows)
            koi_gfx_fill(x + 2 + (int)column * WINDOW_CHAR_W,
                         y + row * WINDOW_CHAR_H, 2, WINDOW_CHAR_H,
                         window_accent);
    }
}

static void key_note(WINDOW* window, int key) {
    (void)window;
    switch (key) {
    case KOI_KEY_LEFT:  edit_move_by(&note, -1, 0); break;
    case KOI_KEY_RIGHT: edit_move_by(&note, 1, 0); break;
    case KOI_KEY_UP:    edit_move_lines(&note, -1, 0); break;
    case KOI_KEY_DOWN:  edit_move_lines(&note, 1, 0); break;
    case KOI_KEY_HOME:  edit_move_home(&note, 0); break;
    case KOI_KEY_END:   edit_move_end(&note, 0); break;
    case KOI_KEY_DELETE: edit_delete(&note); break;
    case '\b': edit_backspace(&note); break;
    case '\n': case '\r': edit_insert_char(&note, '\n'); break;
    case '\t': edit_insert(&note, "    ", 4); break;
    default:
        if (key >= ' ' && key < 0x100) edit_insert_char(&note, (char)key);
        break;
    }
    window_repaint();
}

/* ---- About --------------------------------------------------------------- */

static void paint_about(WINDOW* window, int x, int y, int width, int height) {
    char line[80];

    (void)window;
    (void)width;
    (void)height;
    window_label(x + 12, y + 10, say(SAY_DESKTOP_TITLE), window_text);
    koi_snprintf(line, sizeof(line), "Koi-DOS build %ld",
                 koi_sysinfo(KOI_INFO_BUILD_NUMBER, 0));
    window_label(x + 12, y + 34, line, window_text);
    window_label(x + 12, y + 58, say(SAY_ONE_AT_A_TIME_1), window_shadow);
    window_label(x + 12, y + 74, say(SAY_ONE_AT_A_TIME_2), window_shadow);
    koi_snprintf(line, sizeof(line), "%ld %s",
                 koi_sysinfo(KOI_INFO_MEMORY_FREE, 0), say(SAY_FREE));
    window_label(x + 12, y + 98, line, window_text);
}

/* ---- Opening things ------------------------------------------------------ */

static void open_clock(void) {
    if (clock_window) { clock_window->minimised = 0; window_raise(clock_window); return; }
    clock_window = window_new(say(SAY_CLOCK), 640, 300, 280, 240);
    if (!clock_window) return;
    clock_window->paint = paint_clock;
}

static void name_note_menus(WINDOW_MENU* menus) {
    menus[0] = (WINDOW_MENU){ say(SAY_MENU_FILE),
        { { say(SAY_SAVE), MENU_SAVE }, { 0, 0 },
          { say(SAY_CLOSE), MENU_EXIT } }, 3 };
    menus[1] = (WINDOW_MENU){ say(SAY_MENU_FORMAT),
        { { say(SAY_BOLD), MENU_BOLD }, { say(SAY_ITALIC), MENU_ITALIC },
          { say(SAY_UNDERLINE), MENU_UNDERLINE }, { 0, 0 },
          { say(SAY_PLAIN), MENU_PLAIN } }, 5 };
}

static void open_note(void) {
    WINDOW_MENU menus[2];

    if (note_window) { note_window->minimised = 0; window_raise(note_window); return; }
    if (!note_ready) {
        if (!edit_load(&note, NOTE_PATH, NOTE_CAPACITY) &&
            !edit_new(&note, NOTE_CAPACITY)) return;
        if (!note.path[0]) strcpy(note.path, NOTE_PATH);
        note_ready = 1;
    }
    name_note_menus(menus);
    note_window = window_new("NoteEdit - NOTE.TXT", 300, 120, 520, 340);
    if (!note_window) return;
    note_window->paint = paint_note;
    note_window->key = key_note;
    note_window->menu_count = 2;
    note_window->menus[0] = menus[0];
    note_window->menus[1] = menus[1];
}

static void open_about(void) {
    if (about_window) { about_window->minimised = 0; window_raise(about_window); return; }
    about_window = window_new(say(SAY_ABOUT), 360, 380, 360, 180);
    if (!about_window) return;
    about_window->paint = paint_about;
}

/* Koi-Commander is another program, so it is started the way any program is
   started here: ask for it, ask for this desktop after it, and leave. The
   screen goes away and comes back, which is honest about what the machine can
   do rather than a window pretending otherwise. */
static void start_commander(void) {
    char self[128];

    if (koi_systext(KOI_TEXT_PROGRAM_PATH, 0, self, sizeof(self)) <= 0)
        strcpy(self, "\\MIZU\\MIZU.EXE");
    koi_chain(self);
    koi_chain("\\COMMANDER\\COMMANDER");
    window_close_desktop();
    koi_exit(0);
}

int main(void) {
    WINDOW_EVENT event;
    WINDOW_MENU desktop[3];
    WINDOW_MENU panel[2];

    /* The first time, ask the questions before drawing anything. Done by
       asking the shell to run the configuration and then this again, because
       one program runs at a time and this one has not taken the screen yet. */
    {
        char configured[16];

        if (!koi_arguments()[0] &&
            (!settings_get("MIZU", "configured", configured,
                           sizeof(configured)) || configured[0] != '1')) {
            char self[128];
            char config[128];
            int cut = 0;

            if (koi_systext(KOI_TEXT_PROGRAM_PATH, 0, self, sizeof(self)) <= 0)
                strcpy(self, "\\MIZU\\MIZU.EXE");
            strcpy(config, self);
            for (int index = 0; config[index]; index++)
                if (config[index] == '\\') cut = index + 1;
            strcpy(config + cut, "MIZUCFG.EXE");
            koi_chain(self);
            koi_chain(config);
            return 0;
        }
    }

    language_load();

    if (!window_open_desktop(say(SAY_DESKTOP_TITLE))) {
        koi_print("Mizu needs a framebuffer and could not get one.\n");
        return 1;
    }
    /* Built here rather than written as literals: a menu in three languages
       is three tables that drift apart, and one table filled in at startup is
       one. */
    desktop[0] = (WINDOW_MENU){ say(SAY_MENU_SYSTEM),
        { { say(SAY_ABOUT), MENU_ABOUT }, { 0, 0 }, { say(SAY_EXIT), MENU_EXIT } }, 3 };
    desktop[1] = (WINDOW_MENU){ say(SAY_MENU_RUN),
        { { say(SAY_NOTEEDIT), MENU_NOTE },
          { say(SAY_COMMANDER), MENU_COMMANDER } }, 2 };
    desktop[2] = (WINDOW_MENU){ say(SAY_MENU_VIEW),
        { { say(SAY_CONTROL_PANEL), MENU_CONTROL },
          { say(SAY_CLOCK), MENU_CLOCK }, { 0, 0 },
          { say(SAY_TILE), MENU_TILE } }, 4 };
    panel[0] = (WINDOW_MENU){ say(SAY_MENU_FILE),
        { { say(SAY_COMMANDER), MENU_COMMANDER }, { 0, 0 },
          { say(SAY_EXIT), MENU_EXIT } }, 3 };
    panel[1] = (WINDOW_MENU){ say(SAY_MENU_OPTIONS),
        { { say(SAY_ABOUT), MENU_ABOUT } }, 1 };

    window_desktop_menu(desktop, 3);

    name_entries();
    control_window = window_new(say(SAY_CONTROL_PANEL), 60, 70, 512, 300);
    if (control_window) {
        control_window->paint = paint_control;
        control_window->click = click_control;
        control_window->menu_count = 2;
        control_window->menus[0] = panel[0];
        control_window->menus[1] = panel[1];
    }
    open_clock();

    while (window_next(&event)) {
        if (event.type == WINDOW_EVENT_CLOSE) {
            if (event.window == control_window) { window_quit(); break; }
            if (event.window == clock_window) clock_window = (WINDOW*)0;
            if (event.window == about_window) about_window = (WINDOW*)0;
            if (event.window == note_window) note_window = (WINDOW*)0;
            window_delete(event.window);
            continue;
        }
        if (event.type == WINDOW_EVENT_MENU) {
            switch (event.id) {
            case MENU_ABOUT: open_about(); break;
            case MENU_NOTE: open_note(); break;
            case MENU_SAVE:
                if (note_ready) {
                    strcpy(note_window->title, edit_save(&note, note.path)
                           ? "NoteEdit - NOTE.TXT" : say(SAY_COULD_NOT_SAVE));
                    window_repaint();
                }
                break;
            case MENU_BOLD: note_style ^= KOI_TEXT_BOLD; window_repaint(); break;
            case MENU_ITALIC: note_style ^= KOI_TEXT_ITALIC; window_repaint(); break;
            case MENU_UNDERLINE:
                note_style ^= KOI_TEXT_UNDERLINE;
                window_repaint();
                break;
            case MENU_PLAIN: note_style = 0; window_repaint(); break;
            case MENU_CLOCK: open_clock(); break;
            case MENU_COMMANDER: start_commander(); break;
            case MENU_CONTROL:
                if (control_window) {
                    control_window->minimised = 0;
                    window_raise(control_window);
                }
                break;
            case MENU_TILE:
                /* Everything back where it started, for a desk that has been
                   shuffled into a pile. */
                if (control_window) { control_window->x = 60; control_window->y = 70; }
                if (clock_window) { clock_window->x = 640; clock_window->y = 300; }
                if (about_window) { about_window->x = 360; about_window->y = 380; }
                window_repaint();
                break;
            case MENU_EXIT:
                /* "Close" in a window's own File menu closes that window;
                   "Exit to DOS" in the desktop's menu ends everything. */
                if (note_window && event.window == note_window) {
                    window_delete(note_window);
                    note_window = (WINDOW*)0;
                } else {
                    window_quit();
                }
                break;
            default: break;
            }
            continue;
        }
        if (event.type == WINDOW_EVENT_KEY && event.id == 27) window_quit();
    }

    window_close_desktop();
    return 0;
}
