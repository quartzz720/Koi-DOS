#ifndef SETTINGS_H
#define SETTINGS_H

#include "koi.h"

/* The user's settings file, as a program sees it.
 *
 * The kernel reads them once at boot and programs write them - that direction
 * is deliberate and stays. What changed is that there is no longer one file
 * for everybody: `color` used to rewrite the whole thing from what `color`
 * knew about, and any key written by anything else disappeared the next time
 * somebody changed a colour. One writer is fine until there are two.
 *
 * So: a file per section, and within one, read or set a single key with every
 * other line left exactly as it was. Two programs that never open the same
 * file cannot collide however carelessly they are written, which is worth more
 * than a rule everybody has to remember.
 */
/* One file per section, so two programs never open the same one. `section` is
   a short name - CONSOLE, SOUND, CMDR - and becomes \BOOT\CONFIG\<section>.CFG.
   The directory is made on the first write. */
#define SETTINGS_DIRECTORY "\\BOOT\\CONFIG"

/* The value of `key`, or 0 when it is not there. `into` is always terminated. */
int settings_get(const char* section, const char* key, char* into, long size);

/* Set `key`, keeping every other line of that section's file. Returns 0 if it
   could not be written - a setting that did not stick must not be reported as
   one that did. */
int settings_set(const char* section, const char* key, const char* value);

#endif
