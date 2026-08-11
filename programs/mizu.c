#include "koi.h"
#include "window.h"
#include "editcore.h"
#include "wav.h"
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
#define MENU_PLAYER 13
#define MENU_STOP 14
#define MENU_PAUSE 15

static WINDOW* control_window;
static WINDOW* clock_window;
static WINDOW* about_window;
static WINDOW* note_window;
static WINDOW* player_window;

static int current_volume_index(void) {
    long count = koi_sysinfo(KOI_INFO_VOLUME_COUNT, 0);

    for (long index = 0; index < count; index++)
        if (koi_sysinfo(KOI_INFO_VOLUME_IS_CURRENT, index) == 1)
            return (int)index;
    return -1;
}


/* ---- The control panel ---------------------------------------------------
 *
 * Program Manager's grid, which is the right shape for a machine with no
 * overlapping-window habits yet: a page of things you can start, each one a
 * picture and a word.
 */
typedef struct {
    const char* name;
    koi_uint32 (*tint)(void);
} ENTRY;

static koi_uint32 tint_setup(void) { return koi_gfx_color(0x4A, 0x8F, 0xB8); }
static koi_uint32 tint_files(void) { return koi_gfx_color(0x35, 0xA6, 0xC4); }
static koi_uint32 tint_tools(void) { return koi_gfx_color(0x58, 0xB0, 0xA8); }

static ENTRY entries[5];

static void name_entries(void) {
    entries[0] = (ENTRY){ say(SAY_COMMANDER), tint_files };
    entries[1] = (ENTRY){ say(SAY_NOTEEDIT), tint_tools };
    entries[2] = (ENTRY){ say(SAY_CLOCK), tint_setup };
    entries[3] = (ENTRY){ "Player", tint_setup };
    entries[4] = (ENTRY){ say(SAY_ABOUT), tint_tools };
}
#define ENTRY_COUNT 5

#define ICON_W 120
#define ICON_H 96

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

/* A label under an icon, wrapped over as many lines as it needs and every
 * line centred.
 *
 * There used to be a second line of description under the name, and in
 * English it fitted. "Control Panel" became "Панель керування" and the
 * description became the thing that ran into the neighbouring icon - so the
 * description is gone and the name gets the room instead. A caption that only
 * fits in the language it was written in is a caption that was never measured.
 *
 * Broken at spaces; a single word longer than the cell is left to overhang,
 * because breaking a word mid-letter reads worse than a wide one.
 */
#define LABEL_LINES 3

static void label_in_cell(int x, int y, const char* text) {
    int cell = ICON_W - 8;
    int fits = cell / WINDOW_CHAR_W;
    char line[64];
    int line_count = 0;

    while (*text && line_count < LABEL_LINES) {
        int bytes = 0;
        int columns = 0;
        int last_space = -1;
        int width;

        while (text[bytes] && columns < fits) {
            if (text[bytes] == ' ') last_space = bytes;
            if (((unsigned char)text[bytes] & 0xC0) != 0x80) columns++;
            bytes++;
        }
        if (text[bytes] && last_space > 0) bytes = last_space;
        if (bytes > (int)sizeof(line) - 1) bytes = (int)sizeof(line) - 1;
        memcpy(line, text, (koi_uint64)bytes);
        line[bytes] = 0;

        width = language_columns(line) * WINDOW_CHAR_W;
        window_label(x + (cell - width) / 2, y + line_count * WINDOW_CHAR_H,
                     line, window_text);
        line_count++;

        text += bytes;
        while (*text == ' ') text++;
    }
}

static void paint_control(WINDOW* window, int x, int y, int width, int height) {
    (void)window;
    (void)height;
    for (int index = 0; index < ENTRY_COUNT; index++) {
        int column = index % (width / ICON_W ? width / ICON_W : 1);
        int row = index / (width / ICON_W ? width / ICON_W : 1);
        int ix = x + 8 + column * ICON_W;
        int iy = y + 8 + row * ICON_H;

        draw_icon(ix + (ICON_W - 8) / 2 - 32, iy, entries[index].tint());
        label_in_cell(ix, iy + 40, entries[index].name);
    }
}

static void open_about(void);
static void open_clock(void);
static void open_note(void);
static void open_player(void);
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
    else if (index == 3) open_player();
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

    koi_snprintf(line, sizeof(line), "%02d:%02d:%02d",
                 KOI_TIME_HOUR(now), KOI_TIME_MINUTE(now), KOI_TIME_SECOND(now));
    window_label(x + (width - language_columns(line) * WINDOW_CHAR_W) / 2, y + 6,
                 line, window_text);
    /* "9 August 2026" rather than 2026-08-09: the numbers are for sorting and
       the words are for reading, and this window is for reading. */
    koi_snprintf(line, sizeof(line), "%d %s %d", day, language_month(month),
                 year);
    window_label(x + (width - language_columns(line) * WINDOW_CHAR_W) / 2,
                 y + 24, line, window_text);

    left = x + (width - 7 * cell) / 2;
    for (int index = 0; index < 7; index++)
        window_label(left + index * cell + 4, y + 46,
                     language_weekday(index), window_shadow);

    for (int number = 1; number <= length; number++) {
        int slot = start + number - 1;
        int cx = left + (slot % 7) * cell;
        int cy = y + 66 + (slot / 7) * 20;

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
        long column = edit_column_of(&note, note.cursor);
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

/* ---- The player ----------------------------------------------------------
 *
 * A list of the WAV files it can find, a bar, and the bar can be clicked.
 *
 * The bar is the whole point and it was impossible yesterday. The mixer walks
 * a sound in 32.32 fixed point so that a recording made at one rate can play
 * at another, and the whole part of that number is how far in it has got - it
 * always knew, and nothing had ever asked. Three calls later there is a
 * position, a length and a seek, and a progress bar is arithmetic.
 *
 * The samples stay in memory for as long as the sound plays, because the mixer
 * reads them where they are rather than copying them. Freeing the buffer while
 * a voice still points into it is the one way to make this crash, so the
 * buffer is freed when the voice is stopped and never before.
 */
#define PLAYER_FILES 64
#define PLAYER_PATH 96

/* Whole files, in memory, for as long as they play.
 *
 * The mixer reads the samples where they are rather than copying them, so a
 * track is resident from the moment it starts until it is stopped. There is no
 * streaming: nothing in the audio interface can ask a program for more samples
 * partway through, and inventing that is a bigger change than a player.
 *
 * So the limit is memory, and it used to be a made-up four megabytes - which
 * at CD rates is twenty-three seconds, and is the sort of number that gets
 * written once and then quietly decides what the software is for. SYS_ALLOC
 * goes straight to the page allocator, so what is actually available is most
 * of the machine. A file is now measured before it is read and given exactly
 * what it needs, up to half of what is free - half, so that starting a long
 * track cannot leave the rest of the system with nothing. */
static char tracks[PLAYER_FILES][PLAYER_PATH];
static char player_message[80];
static int track_count;
static int track_playing = -1;
static void* track_data;
static unsigned int track_data_at;
static int voice = -1;
static WAV_FORMAT voice_format;
static unsigned int voice_frames;
static unsigned int player_paused_frame;
static int player_paused;

static void player_sync_pause_label(void) {
    if (player_window) player_window->menus[0].items[0].label = player_paused ? "Resume" : "Pause";
}

static void player_stop(void) {
    if (voice >= 0) koi_sound_stop(voice);
    voice = -1;
    if (track_data) { koi_free(track_data); track_data = 0; }
    track_playing = -1;
    player_paused = 0;
    player_paused_frame = 0;
    player_sync_pause_label();
}

static int player_start_from(unsigned int start_frame) {
    if (start_frame >= voice_frames) return -1;
    voice = koi_sound_play_simple((const char*)track_data + track_data_at,
                                  voice_frames, voice_format.rate,
                                  voice_format.bits == 16 ? KOI_SOUND_S16
                                                          : KOI_SOUND_U8,
                                  voice_format.channels, 255);
    if (voice < 0) return -1;
    if (start_frame && koi_sound_seek(voice, start_frame) < 0) {
        koi_sound_stop(voice);
        voice = -1;
        return -1;
    }
    return 0;
}

static void player_pause(void) {
    if (voice < 0 || !koi_sound_active(voice) || !track_data) return;
    player_paused_frame = koi_sound_where(voice);
    if (player_paused_frame >= voice_frames && voice_frames)
        player_paused_frame = voice_frames - 1;
    koi_sound_stop(voice);
    voice = -1;
    player_paused = 1;
    player_message[0] = 0;
    player_sync_pause_label();
}

static void player_resume(void) {
    if (!player_paused || !track_data) return;
    if (player_start_from(player_paused_frame) < 0) {
        koi_snprintf(player_message, sizeof(player_message),
                     "Every voice is busy");
        return;
    }
    player_paused = 0;
    player_paused_frame = 0;
    player_message[0] = 0;
    player_sync_pause_label();
}

static void player_toggle_pause(void) {
    if (player_paused) player_resume();
    else player_pause();
}

/* The root and the two directories somebody would actually keep music in.
   Not a file browser: a player that can only see the root is a player nobody
   can put a song in front of, and one that browses the disk is a different
   program. */
static void player_scan_in(const char* directory) {
    KOI_FIND_DATA found;
    char pattern[PLAYER_PATH];
    long search;

    koi_snprintf(pattern, sizeof(pattern), "%s*.WAV", directory);
    search = koi_findfirst(pattern, &found);
    if (search < 0) return;
    do {
        if (track_count >= PLAYER_FILES) break;
        koi_snprintf(tracks[track_count], PLAYER_PATH, "%s%s", directory,
                     found.name);
        track_count++;
    } while (koi_findnext(search, &found) == 0);
    koi_findclose(search);
}

static void player_scan(void) {
    track_count = 0;
    player_scan_in("\\");
    player_scan_in("\\MUSIC\\");
    player_scan_in("\\WAV\\");
}

/* The last component of a path, which is what a list wants to show. */
static const char* basename_of(const char* path) {
    const char* last = path;
    for (int at = 0; path[at]; at++)
        if (path[at] == '\\') last = path + at + 1;
    return last;
}

static void player_play(int index) {
    long handle;
    long size;
    long got;
    long affordable;
    unsigned int data_at = 0;
    unsigned int data_size;
    const char* why;

    if (index < 0 || index >= track_count) return;
    player_stop();
    player_message[0] = 0;

    handle = koi_open(tracks[index], OPEN_READ);
    if (handle < 0) {
        koi_snprintf(player_message, sizeof(player_message),
                     "Could not open %s", basename_of(tracks[index]));
        return;
    }

    size = koi_filesize(handle);
    /* KOI_INFO_MEMORY_FREE is in KiB. Half of it, so that playing something
       long does not leave the machine with nothing for anything else. */
    affordable = koi_sysinfo(KOI_INFO_MEMORY_FREE, 0) / 2 * 1024;
    if (size <= 0) {
        koi_close(handle);
        koi_snprintf(player_message, sizeof(player_message), "%s is empty",
                     basename_of(tracks[index]));
        return;
    }
    if (size > affordable) {
        koi_close(handle);
        /* Said with both numbers. "Out of memory" leaves somebody guessing
           whether a slightly smaller file would have worked. */
        koi_snprintf(player_message, sizeof(player_message),
                     "%s is %ld KiB and only %ld KiB can be spared",
                     basename_of(tracks[index]), size / 1024,
                     affordable / 1024);
        return;
    }

    track_data = koi_alloc(size);
    if (!track_data) {
        koi_close(handle);
        koi_snprintf(player_message, sizeof(player_message),
                     "No room for %ld KiB", size / 1024);
        return;
    }
    got = koi_read(handle, track_data, size);
    koi_close(handle);
    if (got <= 0) { player_stop(); return; }

    data_size = wav_parse((const unsigned char*)track_data, (unsigned int)got,
                          &voice_format, &data_at, &why);
    if (!data_size) {
        player_stop();
        koi_snprintf(player_message, sizeof(player_message), "%s: %s",
                     basename_of(tracks[index]), why);
        return;
    }

    track_data_at = data_at;
    voice_frames = data_size /
        (unsigned int)(voice_format.channels * (voice_format.bits / 8));
    if (!voice_frames) {
        player_stop();
        koi_snprintf(player_message, sizeof(player_message),
                     "%s has no samples in it", basename_of(tracks[index]));
        return;
    }

    if (player_start_from(0) < 0) {
        player_stop();
        koi_snprintf(player_message, sizeof(player_message),
                     "Every voice is busy");
        return;
    }
    track_playing = index;
}

#define BAR_TOP 8
#define BAR_HEIGHT 18
#define LIST_TOP 56

static void clock_text(char* out, koi_uint64 size, unsigned int frames,
                       unsigned int rate) {
    unsigned int seconds = rate ? frames / rate : 0;
    koi_snprintf(out, size, "%u:%02u", seconds / 60, seconds % 60);
}

static void paint_player(WINDOW* window, int x, int y, int width, int height) {
    char line[64];
    char left[16];
    char right[16];
    unsigned int at = voice >= 0 ? koi_sound_where(voice)
                                 : (player_paused ? player_paused_frame : 0);
    int rows = (height - LIST_TOP) / WINDOW_CHAR_H;

    (void)window;

    /* The bar. Drawn even when nothing is playing, because a control that
       appears only once it is useful is a control nobody finds. */
    window_sunken(x + 8, y + BAR_TOP, width - 16, BAR_HEIGHT);
    if ((voice >= 0 || player_paused) && voice_frames) {
        int span = (int)((koi_uint64)(width - 18) * at / voice_frames);
        koi_gfx_fill(x + 9, y + BAR_TOP + 1, span, BAR_HEIGHT - 2, window_accent);
    }

    clock_text(left, sizeof(left), at, voice_format.rate);
    clock_text(right, sizeof(right), voice_frames, voice_format.rate);
    /* A message where the name goes, when there is one. A player that does
       nothing and says nothing is a player somebody thinks is broken. */
    if (player_message[0]) {
        window_label(x + 8, y + BAR_TOP + BAR_HEIGHT + 4, player_message,
                     window_shadow);
    } else {
        koi_snprintf(line, sizeof(line), "%s / %s   %s", left, right,
                     track_playing >= 0 ? basename_of(tracks[track_playing])
                                        : "");
        window_label(x + 8, y + BAR_TOP + BAR_HEIGHT + 4, line, window_text);
    }

    for (int index = 0; index < track_count && index < rows; index++) {
        int row = y + LIST_TOP + index * WINDOW_CHAR_H;
        if (index == track_playing) {
            koi_gfx_fill(x + 4, row, width - 8, WINDOW_CHAR_H, window_accent);
            window_label(x + 8, row, basename_of(tracks[index]),
                         window_client_paper);
        } else {
            window_label(x + 8, row, basename_of(tracks[index]), window_text);
        }
    }
    if (!track_count)
        window_label(x + 8, y + LIST_TOP,
                     "No .WAV files in \\, \\MUSIC or \\WAV.", window_shadow);
}

static void click_player(WINDOW* window, int x, int y, int clicks) {
    int client_x, client_y, client_w, client_h;

    (void)window;
    (void)clicks;
    window_client(player_window, &client_x, &client_y, &client_w, &client_h);
    /* On the bar: seek. One click, not two - a bar is a place, and asking for
       a place twice is not a different request. */
    if (y >= BAR_TOP && y < BAR_TOP + BAR_HEIGHT) {
        if ((voice >= 0 || player_paused) && voice_frames && client_w > 18) {
            koi_uint64 frame = (koi_uint64)(x - 9) * voice_frames /
                               (koi_uint64)(client_w - 18);
            if (x < 9) frame = 0;
            if (frame >= voice_frames) frame = voice_frames - 1;
            if (voice >= 0) koi_sound_seek(voice, (unsigned int)frame);
            else player_paused_frame = (unsigned int)frame;
            window_repaint();
        }
        return;
    }

    if (y >= LIST_TOP) {
        int index = (y - LIST_TOP) / WINDOW_CHAR_H;
        if (index >= 0 && index < track_count) {
            player_play(index);
            window_repaint();
        }
    }
}

/* ---- About --------------------------------------------------------------- */

static void paint_about(WINDOW* window, int x, int y, int width, int height) {
    char line[96];
    char cpu[64];
    int current = current_volume_index();
    long free_volume = current >= 0 ? koi_sysinfo(KOI_INFO_VOLUME_FREE_BYTES,
                                                  current) : -1;

    (void)window;
    (void)width;
    (void)height;
    window_label(x + 12, y + 10, say(SAY_DESKTOP_TITLE), window_text);
    koi_snprintf(line, sizeof(line), "Koi-DOS build %ld",
                 koi_sysinfo(KOI_INFO_BUILD_NUMBER, 0));
    window_label(x + 12, y + 34, line, window_text);
    if (koi_systext(KOI_TEXT_CPU_NAME, 0, cpu, sizeof(cpu)) <= 0)
        strcpy(cpu, "unknown CPU");
    koi_snprintf(line, sizeof(line), "CPU: %s", cpu);
    window_label(x + 12, y + 58, line, window_text);
    koi_snprintf(line, sizeof(line), "RAM: %ld / %ld %s",
                 koi_sysinfo(KOI_INFO_MEMORY_FREE, 0),
                 koi_sysinfo(KOI_INFO_MEMORY_TOTAL, 0), say(SAY_FREE));
    window_label(x + 12, y + 74, line, window_text);
    if (free_volume >= 0) {
        char volume_letter = (char)koi_sysinfo(KOI_INFO_VOLUME_LETTER, current);
        koi_snprintf(line, sizeof(line), "Disk %c: %ld %s", volume_letter,
                     free_volume, say(SAY_FREE));
        window_label(x + 12, y + 98, line, window_text);
    }
}

/* ---- Opening things ------------------------------------------------------ */

static void open_clock(void) {
    if (clock_window) { clock_window->minimised = 0; window_raise(clock_window); return; }
    clock_window = window_new(say(SAY_CLOCK), 620, 300, 300, 260);
    if (!clock_window) return;
    clock_window->paint = paint_clock;
    clock_window->repaint_ms = 1000;   /* a clock that does not tick is a date */
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

static void open_player(void) {
    static const WINDOW_MENU menus[] = {
        { "File", { { "Pause", MENU_PAUSE }, { "Stop", MENU_STOP }, { 0, 0 }, { "Close", MENU_EXIT } }, 4 }
    };

    if (player_window) { player_window->minimised = 0; window_raise(player_window); return; }
    player_scan();
    player_window = window_new("Player", 340, 200, 400, 300);
    if (!player_window) return;
    player_window->paint = paint_player;
    /* Four times a second: fast enough that the bar moves smoothly and slow
       enough that a desktop with a track playing is not repainting itself
       thirty times a second to move two pixels. */
    player_window->repaint_ms = 250;
    player_window->click = click_player;
    player_window->menu_count = 1;
    player_window->menus[0] = menus[0];
    player_sync_pause_label();
}

static void open_about(void) {
    if (about_window) { about_window->minimised = 0; window_raise(about_window); return; }
    about_window = window_new(say(SAY_ABOUT), 360, 380, 480, 180);
    if (!about_window) return;
    about_window->paint = paint_about;
}

/* Koi-Commander is another program, so it is started the way any program is
   started here: ask for it, ask for this desktop after it, and leave. The
   screen goes away and comes back, which is honest about what the machine can
   do rather than a window pretending otherwise. */
/* Run it and come back, rather than asking the shell to restart this
 * afterwards.
 *
 * Mizu used to give up its memory, chain the program, chain itself, and exit -
 * so everything on screen went away, the desktop was rebuilt from a command
 * line carrying its own state, and it was visibly a restart. It stays resident
 * now and gets control back where it left off.
 *
 * The screen is handed back first because the thing being started expects a
 * console, and taken again afterwards. That much is still visible and is
 * honest: two full-screen programs cannot both have the screen. */
static void start_commander(void) {
    koi_gfx_leave();
    koi_run("\\COMMANDER\\COMMANDER");
    if (window_reopen_desktop()) window_repaint();
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
        { { say(SAY_NOTEEDIT), MENU_NOTE }, { "Player", MENU_PLAYER },
          { say(SAY_COMMANDER), MENU_COMMANDER } }, 3 };
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
            if (event.window == player_window) {
                player_stop();
                player_window = (WINDOW*)0;
            }
            window_delete(event.window);
            continue;
        }
        if (event.type == WINDOW_EVENT_MENU) {
            switch (event.id) {
            case MENU_ABOUT: open_about(); break;
            case MENU_NOTE: open_note(); break;
            case MENU_PLAYER: open_player(); break;
            case MENU_PAUSE: player_toggle_pause(); window_repaint(); break;
            case MENU_STOP: player_stop(); window_repaint(); break;
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
            case MENU_TILE: window_tile(); break;
            case MENU_EXIT:
                /* "Close" in a window's own File menu closes that window;
                   "Exit to DOS" in the desktop's menu ends everything. */
                if (player_window && event.window == player_window) {
                    player_stop();
                    window_delete(player_window);
                    player_window = (WINDOW*)0;
                } else if (note_window && event.window == note_window) {
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
