#include "editcore.h"

/* See editcore.h for why this exists at all. What follows is the buffer and
 * nothing else: no drawing, no keys, no screen size.
 *
 * Positions are byte offsets, and movement is by character. Those are two
 * different things the moment a file contains anything but ASCII, and getting
 * it wrong does not fail - it cuts a letter in half and writes the halves to
 * disk. The text is UTF-8, so a character is one lead byte followed by any
 * number of bytes with the top bits 10, and stepping over one means stepping
 * over its continuations too. Built in from the start because retrofitting it
 * means auditing every arithmetic expression in the file.
 */

/* Is this byte the middle of a character rather than the start of one? */
static int is_continuation(char byte) {
    return ((unsigned char)byte & 0xC0) == 0x80;
}

static long step_forward(const EDITOR* editor, long offset) {
    if (offset >= editor->length) return editor->length;
    offset++;
    while (offset < editor->length && is_continuation(editor->text[offset]))
        offset++;
    return offset;
}

static long step_back(const EDITOR* editor, long offset) {
    if (offset <= 0) return 0;
    offset--;
    while (offset > 0 && is_continuation(editor->text[offset])) offset--;
    return offset;
}

/* ---- Making and unmaking ------------------------------------------------- */

int edit_new(EDITOR* editor, long capacity) {
    memset(editor, 0, sizeof(*editor));
    editor->text = (char*)koi_alloc(capacity + 1);
    if (!editor->text) return 0;
    editor->undo = (char*)koi_alloc(capacity + 1);
    if (!editor->undo) { koi_free(editor->text); editor->text = KOI_NULL; return 0; }
    editor->capacity = capacity;
    editor->text[0] = 0;
    editor->anchor = -1;
    editor->wants_column = -1;
    return 1;
}

int edit_load(EDITOR* editor, const char* path, long capacity) {
    long handle;
    long size;
    long got = 0;

    if (!edit_new(editor, capacity)) return 0;
    strncpy(editor->path, path, EDIT_PATH_MAX - 1);

    handle = koi_open(path, OPEN_READ);
    if (handle < 0) return 1;          /* a new file with a name, not a failure */

    size = koi_filesize(handle);
    if (size > capacity) size = capacity;
    while (got < size) {
        long step = koi_read(handle, editor->text + got, size - got);
        if (step <= 0) break;
        got += step;
    }
    koi_close(handle);

    /* Carriage returns are dropped on the way in and put back on the way out.
       Editing a file with them left in means every line ends with a character
       the cursor can sit on and nothing can see, which is a bug report nobody
       can describe. */
    {
        long out = 0;
        for (long index = 0; index < got; index++)
            if (editor->text[index] != '\r') editor->text[out++] = editor->text[index];
        got = out;
    }

    editor->length = got;
    editor->text[got] = 0;
    editor->index_valid = 0;
    return 1;
}

int edit_save(EDITOR* editor, const char* path) {
    long handle;
    const char* where = path && path[0] ? path : editor->path;

    if (!where[0]) return 0;
    handle = koi_open(where, OPEN_WRITE);
    if (handle < 0) return 0;

    /* Written back with carriage returns, because this is a DOS-shaped system
       and its own files - AUTOEXEC.BAT, the configuration - are read by things
       that expect them. */
    for (long index = 0; index < editor->length; ) {
        long run = index;
        long step;

        while (run < editor->length && editor->text[run] != '\n') run++;
        if (run > index) {
            step = koi_write(handle, editor->text + index, run - index);
            if (step <= 0) { koi_close(handle); return 0; }
        }
        if (run < editor->length) {
            if (koi_write(handle, "\r\n", 2) != 2) { koi_close(handle); return 0; }
            run++;
        }
        index = run;
    }
    koi_close(handle);

    if (where != editor->path) strncpy(editor->path, where, EDIT_PATH_MAX - 1);
    editor->modified = 0;
    return 1;
}

void edit_close(EDITOR* editor) {
    if (editor->text) koi_free(editor->text);
    if (editor->undo) koi_free(editor->undo);
    editor->text = KOI_NULL;
    editor->undo = KOI_NULL;
}

/* ---- Lines --------------------------------------------------------------- */

static void rebuild_index(EDITOR* editor) {
    editor->lines = 0;
    editor->line_start[editor->lines++] = 0;
    for (long index = 0; index < editor->length; index++) {
        if (editor->text[index] != '\n') continue;
        if (editor->lines >= EDIT_LINES_MAX) break;
        editor->line_start[editor->lines++] = index + 1;
    }
    editor->index_valid = 1;
}

long edit_lines(EDITOR* editor) {
    if (!editor->index_valid) rebuild_index(editor);
    return editor->lines;
}

long edit_line_start(EDITOR* editor, long line) {
    long count = edit_lines(editor);
    if (line < 0) line = 0;
    if (line >= count) line = count - 1;
    return editor->line_start[line];
}

long edit_line_length(EDITOR* editor, long line) {
    long count = edit_lines(editor);
    long start;
    long end;

    if (line < 0 || line >= count) return 0;
    start = editor->line_start[line];
    end = (line + 1 < count) ? editor->line_start[line + 1] - 1 : editor->length;
    return end - start;
}

long edit_line_of(EDITOR* editor, long offset) {
    long count = edit_lines(editor);
    long low = 0;
    long high = count - 1;

    while (low < high) {
        long middle = (low + high + 1) / 2;
        if (editor->line_start[middle] <= offset) low = middle;
        else high = middle - 1;
    }
    return low;
}

/* Columns count characters, not bytes - which is the number a person sees and
   the number that must be used to put a cursor anywhere. */
long edit_column_of(EDITOR* editor, long offset) {
    long start = edit_line_start(editor, edit_line_of(editor, offset));
    long column = 0;

    for (long at = start; at < offset; at = step_forward(editor, at)) column++;
    return column;
}

long edit_offset_at(EDITOR* editor, long line, long column) {
    long at = edit_line_start(editor, line);
    long end = at + edit_line_length(editor, line);

    while (column > 0 && at < end) { at = step_forward(editor, at); column--; }
    return at;
}

/* ---- Selection ----------------------------------------------------------- */

int edit_has_selection(const EDITOR* editor) {
    return editor->anchor >= 0 && editor->anchor != editor->cursor;
}

void edit_select_none(EDITOR* editor) { editor->anchor = -1; }

void edit_select_all(EDITOR* editor) {
    editor->anchor = 0;
    editor->cursor = editor->length;
}

void edit_selection(const EDITOR* editor, long* from, long* to) {
    long a = editor->anchor;
    long b = editor->cursor;

    if (a < 0) a = b;
    if (a <= b) { *from = a; *to = b; } else { *from = b; *to = a; }
}

/* ---- Changing the text --------------------------------------------------- */

static void snapshot(EDITOR* editor) {
    memcpy(editor->undo, editor->text, (koi_uint64)editor->length);
    editor->undo_length = editor->length;
    editor->undo_cursor = editor->cursor;
    editor->can_undo = 1;
}

static void remove_range(EDITOR* editor, long from, long to) {
    if (from < 0) from = 0;
    if (to > editor->length) to = editor->length;
    if (to <= from) return;

    memmove(editor->text + from, editor->text + to,
            (koi_uint64)(editor->length - to));
    editor->length -= to - from;
    editor->text[editor->length] = 0;
    editor->cursor = from;
    editor->anchor = -1;
    editor->modified = 1;
    editor->index_valid = 0;
}

/* Delete the selection if there is one. Returns whether anything went. */
static int drop_selection(EDITOR* editor) {
    long from;
    long to;

    if (!edit_has_selection(editor)) return 0;
    edit_selection(editor, &from, &to);
    remove_range(editor, from, to);
    return 1;
}

void edit_insert(EDITOR* editor, const char* text, long length) {
    if (length <= 0) return;
    if (edit_has_selection(editor)) { snapshot(editor); drop_selection(editor); }
    if (editor->length + length > editor->capacity) return;

    memmove(editor->text + editor->cursor + length, editor->text + editor->cursor,
            (koi_uint64)(editor->length - editor->cursor));
    memcpy(editor->text + editor->cursor, text, (koi_uint64)length);
    editor->length += length;
    editor->cursor += length;
    editor->text[editor->length] = 0;
    editor->modified = 1;
    editor->index_valid = 0;
    editor->wants_column = -1;
}

void edit_insert_char(EDITOR* editor, char character) {
    edit_insert(editor, &character, 1);
}

void edit_backspace(EDITOR* editor) {
    if (edit_has_selection(editor)) { snapshot(editor); drop_selection(editor); return; }
    if (!editor->cursor) return;
    remove_range(editor, step_back(editor, editor->cursor), editor->cursor);
    editor->wants_column = -1;
}

void edit_delete(EDITOR* editor) {
    if (edit_has_selection(editor)) { snapshot(editor); drop_selection(editor); return; }
    if (editor->cursor >= editor->length) return;
    remove_range(editor, editor->cursor, step_forward(editor, editor->cursor));
    editor->wants_column = -1;
}

void edit_undo(EDITOR* editor) {
    if (!editor->can_undo) return;
    memcpy(editor->text, editor->undo, (koi_uint64)editor->undo_length);
    editor->length = editor->undo_length;
    editor->text[editor->length] = 0;
    editor->cursor = editor->undo_cursor;
    if (editor->cursor > editor->length) editor->cursor = editor->length;
    editor->anchor = -1;
    editor->modified = 1;
    editor->index_valid = 0;
    /* One step, and it is spent. A stack of them is worth having and is not
       what somebody needs the first time they cut the wrong paragraph. */
    editor->can_undo = 0;
}

/* ---- Moving -------------------------------------------------------------- */

void edit_move_to(EDITOR* editor, long offset, int extend) {
    if (offset < 0) offset = 0;
    if (offset > editor->length) offset = editor->length;
    /* Never land inside a character. A click lands on a byte; the caret has to
       sit on a boundary. */
    while (offset > 0 && offset < editor->length &&
           is_continuation(editor->text[offset])) offset--;

    if (extend) {
        if (editor->anchor < 0) editor->anchor = editor->cursor;
    } else {
        editor->anchor = -1;
    }
    editor->cursor = offset;
}

void edit_move_by(EDITOR* editor, long characters, int extend) {
    long at = editor->cursor;

    while (characters > 0) { at = step_forward(editor, at); characters--; }
    while (characters < 0) { at = step_back(editor, at); characters++; }
    edit_move_to(editor, at, extend);
    editor->wants_column = -1;
}

/* Up and down remember the column they started from.
 *
 * Without that, moving down through a short line and back up lands somewhere
 * else than it started - the cursor slides left and stays there, which every
 * editor learned to avoid decades ago and which is instantly infuriating. */
void edit_move_lines(EDITOR* editor, long lines, int extend) {
    long line = edit_line_of(editor, editor->cursor);
    long column = editor->wants_column >= 0
        ? editor->wants_column : edit_column_of(editor, editor->cursor);
    long target = line + lines;
    long count = edit_lines(editor);

    if (target < 0) target = 0;
    if (target >= count) target = count - 1;

    edit_move_to(editor, edit_offset_at(editor, target, column), extend);
    editor->wants_column = (int)column;
}

void edit_move_home(EDITOR* editor, int extend) {
    edit_move_to(editor, edit_line_start(editor, edit_line_of(editor, editor->cursor)),
                 extend);
    editor->wants_column = -1;
}

void edit_move_end(EDITOR* editor, int extend) {
    long line = edit_line_of(editor, editor->cursor);
    edit_move_to(editor, edit_line_start(editor, line) + edit_line_length(editor, line),
                 extend);
    editor->wants_column = -1;
}

/* ---- The clipboard ------------------------------------------------------- */

long edit_copy(EDITOR* editor) {
    long from;
    long to;

    if (!edit_has_selection(editor)) return 0;
    edit_selection(editor, &from, &to);
    return koi_clip_put(editor->text + from, to - from);
}

long edit_cut(EDITOR* editor) {
    long taken = edit_copy(editor);

    if (taken > 0) { snapshot(editor); drop_selection(editor); }
    return taken;
}

long edit_paste(EDITOR* editor) {
    long length = koi_clip_get(KOI_NULL, 0);
    char* buffer;

    if (length <= 0) return 0;
    buffer = (char*)malloc((koi_uint64)length + 1);
    if (!buffer) return 0;

    length = koi_clip_get(buffer, length + 1);
    if (length > 0) { snapshot(editor); edit_insert(editor, buffer, length); }
    free(buffer);
    return length;
}

/* ---- Finding ------------------------------------------------------------- */

long edit_find(EDITOR* editor, const char* needle, long from) {
    long length = (long)strlen(needle);

    if (!length) return -1;
    if (from < 0) from = 0;

    for (long at = from; at + length <= editor->length; at++) {
        long index = 0;
        while (index < length &&
               toupper((unsigned char)editor->text[at + index]) ==
               toupper((unsigned char)needle[index])) index++;
        if (index == length) return at;
    }
    return -1;
}
