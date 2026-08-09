#include "editcore.h"

/* edit - the console editor, and the system's own.
 *
 * Mizu will have the other face of this; the buffer underneath is the same
 * file. This one exists because Mizu is a package that can be removed, and a
 * system where the only editor is optional is a system with no editor on a
 * fresh installation.
 *
 * Modelled on nano rather than on vi: the keys are written along the bottom
 * and there are no modes. Somebody who has to change one line of AUTOEXEC.BAT
 * on a machine they have never used should not first have to know something.
 *
 * Selecting is nano's mark rather than shift with the arrows, and that is a
 * limitation of what a character stream can say rather than a preference. By
 * the time a keystroke has become a character the shift key is gone from it -
 * `up` is `up` whether or not anything was held. The graphical face selects
 * with the pointer, which is what a pointer is for.
 */

#define BUFFER_BYTES (256 * 1024)
#define LINE_MAX 512

static EDITOR editor;
static int columns;
static int rows;
static int text_rows;
static long top_line;
static long left_column;
static char message[LINE_MAX];

#define KEY_CONTROL(letter) ((letter) - 'A' + 1)

static void status(const char* text) {
    strncpy(message, text, LINE_MAX - 1);
    message[LINE_MAX - 1] = 0;
}

/* One row of the file, as characters rather than bytes, from `left_column`. */
static long take_line(long line, char* out, long limit) {
    long start = edit_line_start(&editor, line);
    long end = start + edit_line_length(&editor, line);
    long at = start;
    long skipped = 0;
    long length = 0;

    while (skipped < left_column && at < end) {
        at++;
        while (at < end && ((unsigned char)editor.text[at] & 0xC0) == 0x80) at++;
        skipped++;
    }
    while (at < end && length < limit - 1) out[length++] = editor.text[at++];
    out[length] = 0;
    return at;
}

/* Keep the caret on the screen, horizontally as well - a long line that runs
   off the right edge is the case a first editor always forgets. */
static void follow_cursor(void) {
    long line = edit_line_of(&editor, editor.cursor);
    long column = edit_column_of(&editor, editor.cursor);

    if (line < top_line) top_line = line;
    if (line >= top_line + text_rows) top_line = line - text_rows + 1;
    if (top_line < 0) top_line = 0;

    if (column < left_column) left_column = column;
    if (column >= left_column + columns) left_column = column - columns + 1;
    if (left_column < 0) left_column = 0;
}

static void draw_bar(int row, const char* left, const char* right) {
    char line[LINE_MAX];
    long at = 0;
    long tail = (long)strlen(right);

    while (left[at] && at < columns - tail - 1) { line[at] = left[at]; at++; }
    while (at < columns - tail) line[at++] = ' ';
    for (long index = 0; index < tail && at < columns; index++)
        line[at++] = right[index];
    line[at] = 0;

    koi_gotoxy(0, row);
    koi_color(KOI_BLACK, KOI_LIGHT_GRAY);
    koi_print(line);
    koi_color(KOI_LIGHT_GRAY, KOI_BLUE);
}

static void draw(void) {
    char line[LINE_MAX];
    char place[64];
    long total = edit_lines(&editor);
    long from;
    long to;

    edit_selection(&editor, &from, &to);
    koi_cursor(0);

    {
        char title[LINE_MAX];
        koi_snprintf(title, sizeof(title), " %s%s",
                     editor.path[0] ? editor.path : "(new file)",
                     editor.modified ? " *" : "");
        koi_snprintf(place, sizeof(place), "line %ld of %ld, col %ld ",
                     edit_line_of(&editor, editor.cursor) + 1, total,
                     edit_column_of(&editor, editor.cursor) + 1);
        draw_bar(0, title, place);
    }

    for (int row = 0; row < text_rows; row++) {
        long line_number = top_line + row;
        long at;

        koi_gotoxy(0, row + 1);
        if (line_number >= total) {
            /* Past the end. Cleared rather than left with whatever the last
               file put there. */
            for (int index = 0; index < columns; index++) line[index] = ' ';
            line[columns] = 0;
            koi_print(line);
            continue;
        }

        at = take_line(line_number, line, LINE_MAX);

        if (edit_has_selection(&editor)) {
            /* Split the row where the selection starts and ends, so it can be
               seen. Three runs at most, and usually one. */
            long start = at - (long)strlen(line);
            long length = (long)strlen(line);
            long a = from - start;
            long b = to - start;
            long cut;

            if (a < 0) a = 0;
            if (b > length) b = length;

            if (b > a) {
                line[a] = line[a];       /* keeps the compiler quiet about a */
                cut = a;
                {
                    char part[LINE_MAX];
                    memcpy(part, line, (koi_uint64)cut); part[cut] = 0;
                    koi_print(part);
                    memcpy(part, line + a, (koi_uint64)(b - a)); part[b - a] = 0;
                    koi_color(KOI_BLUE, KOI_LIGHT_GRAY);
                    koi_print(part);
                    koi_color(KOI_LIGHT_GRAY, KOI_BLUE);
                    koi_print(line + b);
                }
                for (long index = length; index < columns; index++) koi_putchar(' ');
                continue;
            }
        }

        koi_print(line);
        for (long index = (long)strlen(line); index < columns; index++)
            koi_putchar(' ');
    }

    draw_bar(rows - 1, message[0] ? message :
             " ^S save  ^Q quit  ^B mark  ^C copy  ^X cut  ^V paste  ^Z undo  ^F find",
             "");
    message[0] = 0;

    koi_gotoxy((int)(edit_column_of(&editor, editor.cursor) - left_column),
               (int)(edit_line_of(&editor, editor.cursor) - top_line) + 1);
    koi_cursor(1);
}

/* Ask something on the status line. Returns 0 when the answer was empty. */
static int ask(const char* question, char* answer, long size) {
    char prompt[LINE_MAX];

    koi_snprintf(prompt, sizeof(prompt), " %s", question);
    draw_bar(rows - 1, prompt, "");
    koi_gotoxy((int)strlen(question) + 2, rows - 1);
    koi_cursor(1);
    answer[0] = 0;
    koi_readline(answer, size);
    return answer[0] != 0;
}

static int confirm_discard(void) {
    char answer[16];

    if (!editor.modified) return 1;
    if (!ask("Unsaved changes. Type yes to discard:", answer, sizeof(answer)))
        return 0;
    return toupper((unsigned char)answer[0]) == 'Y';
}

static void do_save(void) {
    char name[EDIT_PATH_MAX];

    if (!editor.path[0]) {
        if (!ask("Save as:", name, sizeof(name))) { status(" Not saved."); return; }
        strncpy(editor.path, name, EDIT_PATH_MAX - 1);
    }
    if (edit_save(&editor, editor.path)) status(" Saved.");
    else status(" Could not write that file.");
}

static void do_find(void) {
    static char needle[128];
    char asked[128];
    long found;

    if (ask("Find:", asked, sizeof(asked))) strcpy(needle, asked);
    if (!needle[0]) return;

    found = edit_find(&editor, needle, editor.cursor + 1);
    if (found < 0) found = edit_find(&editor, needle, 0);   /* round the end */
    if (found < 0) { status(" Not found."); return; }

    edit_move_to(&editor, found, 0);
    editor.anchor = found;
    editor.cursor = found + (long)strlen(needle);
}

int main(void) {
    const char* name = koi_arguments();
    char path[EDIT_PATH_MAX];
    int marking = 0;
    long at = 0;

    while (*name == ' ') name++;
    while (name[at] && name[at] != ' ' && at < EDIT_PATH_MAX - 1) {
        path[at] = name[at];
        at++;
    }
    path[at] = 0;

    columns = (int)koi_sysinfo(KOI_INFO_TEXT_COLUMNS, 0);
    rows = (int)koi_sysinfo(KOI_INFO_TEXT_ROWS, 0);
    if (columns <= 0 || columns > LINE_MAX - 2) columns = 80;
    if (rows <= 3) rows = 25;
    text_rows = rows - 2;

    if (path[0]) {
        if (!edit_load(&editor, path, BUFFER_BYTES)) {
            koi_print("Not enough memory to open that file.\n");
            return 1;
        }
    } else if (!edit_new(&editor, BUFFER_BYTES)) {
        koi_print("Not enough memory.\n");
        return 1;
    }

    koi_color(KOI_LIGHT_GRAY, KOI_BLUE);
    koi_cls();

    for (;;) {
        int key;

        draw();
        key = koi_getchar();

        if (key == KEY_CONTROL('Q')) {
            if (confirm_discard()) break;
            continue;
        }
        if (key == KEY_CONTROL('S')) { do_save(); continue; }
        if (key == KEY_CONTROL('F')) { do_find(); continue; }
        if (key == KEY_CONTROL('Z')) { edit_undo(&editor); continue; }
        if (key == KEY_CONTROL('A')) { edit_select_all(&editor); continue; }
        if (key == KEY_CONTROL('C')) {
            status(edit_copy(&editor) > 0 ? " Copied." : " Nothing marked.");
            marking = 0;
            continue;
        }
        if (key == KEY_CONTROL('X')) {
            status(edit_cut(&editor) > 0 ? " Cut." : " Nothing marked.");
            marking = 0;
            continue;
        }
        if (key == KEY_CONTROL('V')) {
            status(edit_paste(&editor) > 0 ? " Pasted." : " The clipboard is empty.");
            continue;
        }
        if (key == KEY_CONTROL('B')) {
            /* The mark. Everything moved over from here is selected, until it
               is used or dropped - which is how a keyboard says "shift" when
               the shift key has already been thrown away. */
            if (marking) { edit_select_none(&editor); marking = 0; status(" Mark dropped."); }
            else { editor.anchor = editor.cursor; marking = 1; status(" Mark set."); }
            continue;
        }

        switch (key) {
        case KOI_KEY_LEFT:  edit_move_by(&editor, -1, marking); break;
        case KOI_KEY_RIGHT: edit_move_by(&editor, 1, marking); break;
        case KOI_KEY_UP:    edit_move_lines(&editor, -1, marking); break;
        case KOI_KEY_DOWN:  edit_move_lines(&editor, 1, marking); break;
        case KOI_KEY_PAGE_UP:   edit_move_lines(&editor, -text_rows, marking); break;
        case KOI_KEY_PAGE_DOWN: edit_move_lines(&editor, text_rows, marking); break;
        case KOI_KEY_HOME:  edit_move_home(&editor, marking); break;
        case KOI_KEY_END:   edit_move_end(&editor, marking); break;
        case KOI_KEY_DELETE: edit_delete(&editor); break;
        case '\b': edit_backspace(&editor); break;
        case '\n': case '\r': edit_insert_char(&editor, '\n'); break;
        case '\t':
            /* Four spaces rather than a tab character. Storing a tab means
               every part of this has to agree where the stops are, and two
               parts that disagree put the cursor somewhere the text is not. */
            edit_insert(&editor, "    ", 4);
            break;
        default:
            /* Anything printable, and anything above 127 - which is a UTF-8
               byte on its way through, and is passed along rather than
               judged. */
            if (key >= ' ' && key < 0x100) edit_insert_char(&editor, (char)key);
            break;
        }
        follow_cursor();
    }

    edit_close(&editor);
    koi_cursor(1);
    /* The screen back as it was found.
     *
     * Setting the colour is not enough: the field this editor painted stays
     * behind under the prompt, so the shell writes correctly coloured text
     * onto somebody else's blue. The colours come from the shell rather than
     * from a pair chosen here, so a machine whose owner picked its own gets
     * those and a machine that picked nothing gets the DOS ones. */
    {
        long theme = koi_theme(-1, -1, -1, -1);
        koi_color(KOI_THEME_FOREGROUND(theme), KOI_THEME_BACKGROUND(theme));
    }
    koi_cls();
    koi_cursor(1);
    return 0;
}
