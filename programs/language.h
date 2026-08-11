#ifndef LANGUAGE_H
#define LANGUAGE_H

#include "koi.h"

/* What the system says, in the language it was asked to say it in.
 *
 * A table rather than files of translations on disk: this is a shell with
 * thirty phrases in it, and a loader, a format and a missing-file case would
 * all be larger than the thing they carry. When there are three hundred
 * phrases and somebody outside this repository wants to add a fifth language,
 * this is the file that changes and nothing else is.
 *
 * The choice lives in the settings under SYSTEM, not under any one program's
 * section, because a machine is in one language and not one language per
 * application.
 *
 * Every string is UTF-8. Nothing here is padded to a width or assumed to be
 * one byte per letter - a Ukrainian, Russian, or Greek label is twice the bytes of an English one
 * and the same number of columns, and code that confuses the two draws boxes
 * of the wrong size.
 */

#define LANGUAGE_EN 0
#define LANGUAGE_RU 1
#define LANGUAGE_UK 2
#define LANGUAGE_EL 3
#define LANGUAGE_COUNT 4

/* Read the choice from the settings. Falls back to English, which is also what
   a machine that has never been asked gets. */
void language_load(void);
int language_current(void);
void language_set(int language);      /* for the moment of choosing */
const char* language_name(int language);

/* Changes what say() returns without persisting it. For letting someone
   see a language before committing to it - Esc must be able to undo this,
   and a setting written to disk cannot be undone by walking away. */
void language_preview(int language);

/* The phrases. Kept as an enum rather than as strings looked up by name: a
   misspelt name is a run-time hole, and a misspelt enum does not build. */
enum {
    SAY_DESKTOP_TITLE, DIALOG_YES, DIALOG_NO, DIALOG_OK, DIALOG_CANCEL,
    SAY_MENU_SYSTEM, SAY_MENU_RUN, SAY_MENU_VIEW, SAY_MENU_FILE,
    SAY_MENU_FORMAT, SAY_MENU_OPTIONS,
    SAY_ABOUT, SAY_EXIT, SAY_CONTROL_PANEL, SAY_CLOCK, SAY_COMMANDER,
    SAY_TILE, SAY_NOTEEDIT, SAY_SAVE, SAY_CLOSE,
    SAY_BOLD, SAY_ITALIC, SAY_UNDERLINE, SAY_PLAIN,
    SAY_ONE_AT_A_TIME_1, SAY_ONE_AT_A_TIME_2, SAY_FREE,
    SAY_COULD_NOT_SAVE,
    SAY_COUNT
};

const char* say(int phrase);

/* The calendar's own words. Two-letter weekday headings, because that is what
   fits a column, and month names in whatever case the language puts a date in -
   "9 августа", not "9 август". A month name is not a word looked up in a
   dictionary; it is a word in a sentence. */
const char* language_weekday(int index);      /* 0 = Sunday */
const char* language_month(int month);        /* 1 = January */

/* How many columns a UTF-8 string occupies. Not its length in bytes, which is
   what every one of these strings was measured by until there was a second
   alphabet to measure. */
int language_columns(const char* text);

#endif
