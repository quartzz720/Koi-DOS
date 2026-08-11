#ifndef DIALOG_H
#define DIALOG_H

#include "koi.h"

/* Boxes on the console, in the shape everybody already knows.
 *
 * A framed box in the middle of a coloured field, a title in it, and one row
 * of buttons at the bottom. DOS installers looked like this, and so does
 * whiptail on a Linux box today, because the shape solves a real problem: a
 * question asked in the middle of a screen that has been cleared for it cannot
 * be missed, and a question printed after forty lines of output can.
 *
 * It draws with the console calls a program already has - no graphics mode, no
 * framebuffer - so it works before anything has been configured, over a serial
 * line, and on a machine whose graphical shell is the thing being installed.
 * That last one is the reason it exists: the first questions a system asks are
 * asked before there is anything to ask them with.
 *
 * Nothing here allocates. A dialogue is drawn, answered and gone.
 */

/* The field behind the boxes. Drawn once, by whatever is running the
   questions, so a run of several does not flash between them. */
void dialog_begin(const char* heading);
void dialog_end(void);

/* One message and an OK. Returns when it is dismissed. */
void dialog_message(const char* title, const char* text);

/* Yes or no. Returns 1 for yes, 0 for no, -1 if Esc was pressed - which is
   not the same as no, and a caller that treats them the same is a caller that
   cannot be escaped from. */
int dialog_yesno(const char* title, const char* text, int yes_by_default);

/* One of a list. `items` are the labels; `notes` may be NULL, or a matching
   list of second columns. Returns the index chosen, or -1 for Esc. */
int dialog_menu(const char* title, const char* text,
                const char* const* items,
                const char* const* notes,
                int count,
                int selected,
                void (*on_change)(int selected));

/* A line of text. `buffer` arrives holding the default and leaves holding the
   answer. Returns 1 when accepted, 0 for Esc. */
int dialog_input(const char* title, const char* text, char* buffer, int size);

#endif
