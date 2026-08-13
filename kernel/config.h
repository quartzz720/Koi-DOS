#ifndef KERNEL_CONFIG_H
#define KERNEL_CONFIG_H

#include "partition.h"

/* User settings, read once at boot from a file on the boot volume.
 *
 * The file is written by programs - `color.exe` is the first - and read here.
 * That direction matters: nothing in the kernel needs to know how a program
 * chose to phrase a setting, and nothing in a program needs to reach into
 * kernel state to make a change stick.
 *
 * The format is deliberately dull. One `key = value` per line, a line whose
 * first character is `#` or `;` is a comment, unknown keys are ignored rather
 * than rejected, and a missing file is not an error. A configuration file that
 * refuses to boot the system it configures would be a poor trade.
 *
 * The whole line, not the rest of one: `;` also separates the entries of the
 * program search path, and a comment that could start in the middle of a value
 * ate everything after the first entry. */
/* One file per owner, in a directory of them. Two programs that never open the
   same file cannot overwrite each other's keys, which is a property of the
   arrangement rather than of everybody remembering a rule. */
#define CONFIG_DIRECTORY "\\BOOT\\CONFIG"

/* The single file everything used to share. Still read, so a machine that has
   one keeps its settings; nothing writes it any more, and anything in the
   directory above wins over it. */
#define CONFIG_LEGACY_PATH "\\BOOT\\userspace.cfg"

void config_load(VOLUME* volume);

/* Extra directories the shell searches for programs, in PATH order. */
const char* config_program_path(void);

/* Put a directory on that path and write it back to the settings file, so it
 * is still there after a reboot. Returns 1 when the path now contains it -
 * including when it already did, which is not a failure and is the ordinary
 * case when a package is installed a second time.
 *
 * This is the one setting the kernel writes rather than only reads, and it is
 * dosget that needs it: a package installed into a directory of its own is a
 * program that cannot be run until something knows to look there. Leaving that
 * to the person who installed it means every package ships with a line of
 * instructions saying how to finish installing it. */
int config_add_program_path(VOLUME* volume, const char* directory);

/* And take it off again, for a package that has been removed. Returns 1 when
   the path no longer contains it, including when it never did. A directory
   left on the search path after its programs have gone is a shell that looks
   somewhere pointless before every command it runs. */
int config_remove_program_path(VOLUME* volume, const char* directory);

/* Look up a colour by name or by number, 0-15. Returns -1 when it is neither.
   Shared with anything else that has to read a colour out of text. */
int config_parse_color(const char* text);

/* The canonical name of a colour, for writing settings back out. */
const char* config_color_name(int color);

#endif
