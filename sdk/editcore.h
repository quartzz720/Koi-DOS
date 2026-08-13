#ifndef EDITCORE_H
#define EDITCORE_H

#include "koi.h"

/* The editing that both editors do.
 *
 * There are two front ends - `edit` draws it on the console, Mizu draws it on
 * the framebuffer - and there is one of everything else. That split is not
 * tidiness: a text buffer is where the off-by-ones live, and two copies of an
 * insert-at-cursor written a week apart do not stay the same shape. Rendering
 * is the easy half and is the half that differs.
 *
 * Nothing here draws, reads a key, or knows how large a screen is.
 *
 * The buffer is flat and moves its tail on every insert. A gap buffer would be
 * faster and is not needed: at a quarter of a megabyte, moving the tail is
 * measured in microseconds, and nobody types quickly enough to notice. When
 * that stops being true this is the file that changes and nothing else is.
 */

#define EDIT_PATH_MAX 128
#define EDIT_LINES_MAX 16384

typedef struct {
    char* text;
    long length;
    long capacity;

    long cursor;          /* byte offset of the caret */
    long anchor;          /* where a selection began, or -1 for none */

    int modified;         /* changed since it was last saved */
    int wants_column;     /* the column to aim for when moving up and down */

    /* One step of undo, taken before anything that destroys text. Typing is
       not snapshotted - backspace already undoes that - but a cut, a paste or
       a deleted selection removes work that is not otherwise recoverable, and
       those are the moments somebody needs it. */
    char* undo;
    long undo_length;
    long undo_cursor;
    int can_undo;

    /* Where each line starts. Rebuilt when the text changes rather than
       maintained, because maintaining it correctly through every edit is the
       kind of bookkeeping that is wrong in one case out of twenty. */
    long line_start[EDIT_LINES_MAX];
    long lines;
    int index_valid;

    char path[EDIT_PATH_MAX];
} EDITOR;

/* An empty buffer, or a file. Both return 0 when there was not enough memory;
   edit_load also returns 0 when the file could not be read, and the caller can
   tell them apart by asking whether the path exists. */
int edit_new(EDITOR* editor, long capacity);
int edit_load(EDITOR* editor, const char* path, long capacity);
int edit_save(EDITOR* editor, const char* path);
void edit_close(EDITOR* editor);

/* Lines, counted from zero. The index is rebuilt on demand, so these are cheap
   to call in a drawing loop. */
long edit_lines(EDITOR* editor);
long edit_line_start(EDITOR* editor, long line);
long edit_line_length(EDITOR* editor, long line);
long edit_line_of(EDITOR* editor, long offset);
long edit_column_of(EDITOR* editor, long offset);
long edit_offset_at(EDITOR* editor, long line, long column);

/* Changing the text. Each of these removes the selection first if there is
   one, which is what every editor does and what a caller would otherwise have
   to remember at each call site. */
void edit_insert(EDITOR* editor, const char* text, long length);
void edit_insert_char(EDITOR* editor, char character);
void edit_backspace(EDITOR* editor);
void edit_delete(EDITOR* editor);
void edit_undo(EDITOR* editor);

/* Moving. `extend` continues a selection rather than dropping it - which is
   what holding shift means, and the only difference between the two. */
void edit_move_to(EDITOR* editor, long offset, int extend);
void edit_move_by(EDITOR* editor, long characters, int extend);
void edit_move_lines(EDITOR* editor, long lines, int extend);
void edit_move_home(EDITOR* editor, int extend);
void edit_move_end(EDITOR* editor, int extend);

int edit_has_selection(const EDITOR* editor);
void edit_select_none(EDITOR* editor);
void edit_select_all(EDITOR* editor);
/* The selection, ordered, whichever end it was made from. */
void edit_selection(const EDITOR* editor, long* from, long* to);

/* The clipboard, which is the kernel's and outlives this program. Copy and cut
   return the number of bytes taken; paste the number put in. */
long edit_copy(EDITOR* editor);
long edit_cut(EDITOR* editor);
long edit_paste(EDITOR* editor);

/* The next occurrence at or after `from`, or -1. Case-insensitive, because
   somebody looking for a word does not usually mean its capitalisation. */
long edit_find(EDITOR* editor, const char* needle, long from);

#endif
