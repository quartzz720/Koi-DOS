#include "dialog.h"
#include "language.h"

/* CP437 double-line box drawing, which is the frame every DOS installer used.
   Single lines exist too and are the wrong weight here: the point of the frame
   is that the eye finds it before it has finished looking. */
#define BOX_TOP_LEFT     "\xC9"
#define BOX_TOP_RIGHT    "\xBB"
#define BOX_BOTTOM_LEFT  "\xC8"
#define BOX_BOTTOM_RIGHT "\xBC"
#define BOX_HORIZONTAL   '\xCD'
#define BOX_VERTICAL     "\xBA"

#define FIELD_INK KOI_LIGHT_GRAY
#define FIELD_PAPER KOI_BLUE
#define BOX_INK KOI_BLACK
#define BOX_PAPER KOI_LIGHT_GRAY
#define TITLE_INK KOI_RED
#define PICK_INK KOI_WHITE
#define PICK_PAPER KOI_BLUE

#define DIALOG_LINE_MAX 128

static int columns;
static int rows;

static void size(void) {
    columns = (int)koi_sysinfo(KOI_INFO_TEXT_COLUMNS, 0);
    rows = (int)koi_sysinfo(KOI_INFO_TEXT_ROWS, 0);
    if (columns < 40) columns = 80;
    if (rows < 10) rows = 25;
}

/* In columns. A byte count is not a width once a letter can be two bytes:
   boxes sized by one and drawn by the other come out the wrong shape. */
static int text_length(const char* text) {
    int columns = 0;
    for (int index = 0; text[index]; index++)
        if (((unsigned char)text[index] & 0xC0) != 0x80) columns++;
    return columns;
}

static void at(int column, int row, const char* text) {
    koi_gotoxy(column, row);
    koi_print(text);
}

static void repeat(int column, int row, char character, int count) {
    char line[DIALOG_LINE_MAX];
    int index = 0;

    if (count > DIALOG_LINE_MAX - 1) count = DIALOG_LINE_MAX - 1;
    while (index < count) line[index++] = character;
    line[index] = 0;
    at(column, row, line);
}

void dialog_begin(const char* heading) {
    size();
    koi_cursor(0);
    koi_color(FIELD_INK, FIELD_PAPER);
    koi_cls();
    if (heading && heading[0]) {
        koi_color(KOI_BLACK, KOI_LIGHT_GRAY);
        repeat(0, 0, ' ', columns);
        at(1, 0, heading);
    }
    koi_color(FIELD_INK, FIELD_PAPER);
}

void dialog_end(void) {
    /* Back to the colours this machine was configured with, not to a pair
     * picked here.
     *
     * Leaving grey on black looked like the right answer and was not: a
     * machine whose owner had chosen its colours came back from a dialogue
     * wearing somebody else's, and the field the dialogue had painted stayed
     * behind under the prompt. The shell knows what the colours are - it read
     * them out of the configuration at startup - so ask it rather than
     * guessing, and a machine that configured nothing gets the DOS pair from
     * the same place. */
    long theme = koi_theme(-1, -1, -1, -1);

    koi_color(KOI_THEME_FOREGROUND(theme), KOI_THEME_BACKGROUND(theme));
    koi_cls();
    koi_cursor(1);
}

/* The frame, and the space inside it cleared. Everything else is drawn on top
   of what this leaves. */
static void frame(int x, int y, int width, int height, const char* title) {
    koi_color(BOX_INK, BOX_PAPER);
    at(x, y, BOX_TOP_LEFT);
    repeat(x + 1, y, BOX_HORIZONTAL, width - 2);
    at(x + width - 1, y, BOX_TOP_RIGHT);

    for (int row = 1; row < height - 1; row++) {
        at(x, y + row, BOX_VERTICAL);
        repeat(x + 1, y + row, ' ', width - 2);
        at(x + width - 1, y + row, BOX_VERTICAL);
    }

    at(x, y + height - 1, BOX_BOTTOM_LEFT);
    repeat(x + 1, y + height - 1, BOX_HORIZONTAL, width - 2);
    at(x + width - 1, y + height - 1, BOX_BOTTOM_RIGHT);

    if (title && title[0]) {
        int length = text_length(title);
        int start = x + (width - length - 2) / 2;
        koi_color(TITLE_INK, BOX_PAPER);
        at(start, y, " ");
        koi_print(title);
        koi_print(" ");
        koi_color(BOX_INK, BOX_PAPER);
    }
}

/* A row of buttons, one of them chosen. Drawn as <Ok> rather than in a raised
   box, because a console has no raised anything and the angle brackets are
   what DOS used to mean "this is pressable". */
static void buttons(int x, int y, int width, const char* const* labels,
                    int count, int chosen) {
    int total = 0;
    int gap;
    int place;

    for (int index = 0; index < count; index++)
        total += text_length(labels[index]) + 4;
    gap = (width - 2 - total) / (count + 1);
    if (gap < 1) gap = 1;
    place = x + 1 + gap;

    for (int index = 0; index < count; index++) {
        koi_color(index == chosen ? PICK_INK : BOX_INK,
                  index == chosen ? PICK_PAPER : BOX_PAPER);
        koi_gotoxy(place, y);
        koi_print(index == chosen ? "[<" : " <");
        koi_print(labels[index]);
        koi_print(index == chosen ? ">]" : "> ");
        place += text_length(labels[index]) + 4 + gap;
    }
    koi_color(BOX_INK, BOX_PAPER);
    /* The caret parked off the box. Hiding it stops it blinking but leaves the
       cell it was last drawn in looking pressed, and that cell was the button
       - which reads as a second highlight next to the real one. */
    koi_color(FIELD_INK, FIELD_PAPER);
    koi_gotoxy(0, rows - 1);
    koi_color(BOX_INK, BOX_PAPER);
}

/* Wrap `text` into `into` at `width` and return how many lines it took. Words
   are kept whole; a word longer than the box is broken, because the
   alternative is a line that runs out of the frame. */
static int wrap(const char* text, int width, char into[][DIALOG_LINE_MAX],
                int limit) {
    int line = 0;
    int at_char = 0;

    if (width > DIALOG_LINE_MAX - 1) width = DIALOG_LINE_MAX - 1;
    while (*text && line < limit) {
        int length = 0;
        int columns = 0;
        int last_space = -1;

        /* Advanced by bytes and counted in columns, because the limit is how
           much fits on a line and the copy is of whatever makes those
           letters. */
        while (text[length] && text[length] != '\n' && columns < width) {
            if (text[length] == ' ') last_space = length;
            if (((unsigned char)text[length] & 0xC0) != 0x80) columns++;
            length++;
        }
        if (text[length] && text[length] != '\n' && last_space > 0)
            length = last_space;

        for (at_char = 0; at_char < length; at_char++)
            into[line][at_char] = text[at_char];
        into[line][length] = 0;
        line++;

        text += length;
        while (*text == ' ') text++;
        if (*text == '\n') text++;
    }
    return line ? line : 1;
}

/* Every dialogue is this: a frame, wrapped text, a body the caller draws, and
   a row of buttons. Sharing it is what keeps them the same size and in the
   same place, so a run of questions does not jump about the screen. */
typedef struct {
    int x, y, width, height;
    int text_lines;
} LAYOUT;

static LAYOUT open_box(const char* title, const char* text, int body_rows,
                       char lines[][DIALOG_LINE_MAX]) {
    LAYOUT box;
    int inner = columns - 20;

    /* The field, repainted before every box.
     *
     * Boxes are not all the same size, so a smaller one drawn over a larger
     * one leaves the corners of the larger showing - two frames at once, and
     * the older one reading as part of the newer. Clearing first costs a
     * screenful of writing into a back buffer and nothing on the eye. */
    koi_color(FIELD_INK, FIELD_PAPER);
    for (int row = 1; row < rows; row++) repeat(0, row, ' ', columns);

    if (inner > 60) inner = 60;
    if (inner < 30) inner = 30;

    box.text_lines = wrap(text, inner, lines, 8);
    box.width = inner + 4;
    box.height = 2 + box.text_lines + 1 + body_rows + 2;
    box.x = (columns - box.width) / 2;
    box.y = (rows - box.height) / 2;
    if (box.y < 1) box.y = 1;

    frame(box.x, box.y, box.width, box.height, title);
    for (int index = 0; index < box.text_lines; index++)
        at(box.x + 2, box.y + 1 + index, lines[index]);
    return box;
}

/* Left, right, Tab and the arrows all move between buttons, because somebody
   reaching for a dialogue reaches for whichever of those they learned first. */
static int button_key(int key, int* chosen, int count) {
    if (key == KOI_KEY_LEFT || key == KOI_KEY_UP) {
        *chosen = (*chosen + count - 1) % count;
        return 1;
    }
    if (key == KOI_KEY_RIGHT || key == KOI_KEY_DOWN || key == '\t') {
        *chosen = (*chosen + 1) % count;
        return 1;
    }
    return 0;
}

void dialog_message(const char* title, const char* text) {
    const char* const labels[] = { say(DIALOG_OK) };
    char lines[8][DIALOG_LINE_MAX];
    LAYOUT box;

    size();
    box = open_box(title, text, 0, lines);
    buttons(box.x, box.y + box.height - 2, box.width, labels, 1, 0);
    for (;;) {
        int key;
        koi_cursor(0);
        key = koi_getchar();
        if (key == '\n' || key == '\r' || key == 27 || key == ' ') break;
    }
}

int dialog_yesno(const char* title, const char* text, int yes_by_default) {
    const char* const labels[] = { say(DIALOG_YES), say(DIALOG_NO) };
    char lines[8][DIALOG_LINE_MAX];
    LAYOUT box;
    int chosen = yes_by_default ? 0 : 1;

    size();
    box = open_box(title, text, 0, lines);
    for (;;) {
        int key;
        buttons(box.x, box.y + box.height - 2, box.width, labels, 2, chosen);
        koi_cursor(0);
        key = koi_getchar();
        if (button_key(key, &chosen, 2)) continue;
        if (key == '\n' || key == '\r') return chosen == 0;
        if (key == 27) return -1;
        if (key == 'y' || key == 'Y') return 1;
        if (key == 'n' || key == 'N') return 0;
    }
}

int dialog_menu(const char* title, const char* text,
                const char* const* items,
                const char* const* notes,
                int count,
                int selected,
                void (*on_change)(int selected)) {
    LAYOUT box;
    char lines[8][DIALOG_LINE_MAX];
    int chosen = 0;
    int widest = 0;

    size();
    if (count > 10) count = 10;
    if (selected < 0 || selected >= count) selected = 0;
    for (int index = 0; index < count; index++) {
        int length = text_length(items[index]);
        if (length > widest) widest = length;
    }

    box = open_box(title, text, count, lines);
    for (;;) {
        int key;
        const char* const labels[] = { say(DIALOG_OK), say(DIALOG_CANCEL) };

        for (int index = 0; index < count; index++) {
            int row = box.y + 1 + box.text_lines + index;
            koi_color(index == selected ? PICK_INK : BOX_INK,
                      index == selected ? PICK_PAPER : BOX_PAPER);
            koi_gotoxy(box.x + 3, row);
            /* The radio mark is what the eye reads, not the highlight: a
               highlight says where the cursor is and the mark says what the
               answer would be, and those are different questions. */
            koi_print(index == selected ? "(*) " : "( ) ");
            koi_print(items[index]);
            for (int pad = text_length(items[index]); pad < widest + 2; pad++)
                koi_print(" ");
            if (notes && notes[index]) koi_print(notes[index]);
            koi_print(" ");
            koi_color(BOX_INK, BOX_PAPER);
        }
        buttons(box.x, box.y + box.height - 2, box.width, labels, 2, chosen);

        koi_cursor(0);
        key = koi_getchar();
        if (key == KOI_KEY_UP) {
            selected = (selected + count - 1) % count;
            if (on_change) on_change(selected);
            continue;
        }
        if (key == KOI_KEY_DOWN) {
            selected = (selected + 1) % count;
            if (on_change) on_change(selected);
            continue;
        }
        if (key == '\t' || key == KOI_KEY_LEFT || key == KOI_KEY_RIGHT) {
            chosen = chosen ? 0 : 1;
            continue;
        }
        if (key == '\n' || key == '\r') return chosen == 0 ? selected : -1;
        if (key == 27) return -1;
    }
}

int dialog_input(const char* title, const char* text, char* buffer, int size_of) {
    const char* const labels[] = { say(DIALOG_OK), say(DIALOG_CANCEL) };
    char lines[8][DIALOG_LINE_MAX];
    LAYOUT box;
    int chosen = 0;
    int length = 0;
    int field;

    size();
    while (buffer[length] && length < size_of - 1) length++;
    buffer[length] = 0;
    box = open_box(title, text, 2, lines);
    field = box.width - 6;

    for (;;) {
        int key;
        int row = box.y + 1 + box.text_lines + 1;

        koi_color(PICK_INK, PICK_PAPER);
        koi_gotoxy(box.x + 3, row);
        for (int index = 0; index < field; index++)
            koi_print(index < length ? (char[]){ buffer[index], 0 } : " ");
        koi_color(BOX_INK, BOX_PAPER);
        buttons(box.x, box.y + box.height - 2, box.width, labels, 2, chosen);
        /* The caret, put where the next character will land. A field with no
           caret is a field somebody types into and cannot see. */
        koi_gotoxy(box.x + 3 + (length < field ? length : field - 1), row);
        koi_cursor(1);

        key = koi_getchar();
        koi_cursor(0);
        if (key == '\b') {
            if (length) buffer[--length] = 0;
            continue;
        }
        if (key == '\t') { chosen = chosen ? 0 : 1; continue; }
        if (key == '\n' || key == '\r') return chosen == 0;
        if (key == 27) return 0;
        if (key >= ' ' && key < 0x100 && length < size_of - 1 && length < field) {
            buffer[length++] = (char)key;
            buffer[length] = 0;
        }
    }
}
