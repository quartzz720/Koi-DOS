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
 * The format is deliberately dull. One `key = value` per line, `#` or `;`
 * starts a comment, unknown keys are ignored rather than rejected, and a
 * missing file is not an error. A configuration file that refuses to boot the
 * system it configures would be a poor trade. */
#define CONFIG_PATH "\\BOOT\\userspace.cfg"

void config_load(VOLUME* volume);

/* Look up a colour by name or by number, 0-15. Returns -1 when it is neither.
   Shared with anything else that has to read a colour out of text. */
int config_parse_color(const char* text);

/* The canonical name of a colour, for writing settings back out. */
const char* config_color_name(int color);

#endif
