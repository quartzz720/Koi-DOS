#ifndef KERNEL_ENVIRONMENT_H
#define KERNEL_ENVIRONMENT_H

#include "kernel.h"

/* The environment: named strings the shell and its programs share.
 *
 * DOS had one of these and it is what stops a system needing every path typed
 * out in full. PATH is the reason it exists here - a machine where running a
 * program means knowing which directory somebody's installer chose is a
 * machine nobody wants to use - and PROMPT is the reason it is worth having a
 * second one.
 *
 * Deliberately small and deliberately flat. There is one environment, not one
 * per program: with a single program running at a time and no isolation, a
 * copy per program would be a copy that has to be kept in step, which is more
 * machinery than a DOS has any use for. When programs become isolated the
 * copy-on-start rule is the one to adopt, and nothing above this line changes.
 *
 * Names are compared without case, as everything on this system is. Values are
 * stored exactly as given: a path is a path and trimming it would be a
 * surprise.
 */

#define ENVIRONMENT_MAX 24
#define ENVIRONMENT_NAME_MAX 32
#define ENVIRONMENT_VALUE_MAX 256

/* The value, or NULL when there is no such variable. Valid until the next
   environment_set(), which is long enough for every caller here and is why
   nothing keeps one. */
const char* environment_get(const char* name);

/* Set it, replacing whatever was there. An empty or NULL value removes the
 * variable rather than storing an empty one - which is what `SET NAME=` means
 * in DOS, and the distinction between "unset" and "set to nothing" is not one
 * a shell like this has any use for.
 *
 * Returns 0 when there is no room left, or the name or value is too long. */
int environment_set(const char* name, const char* value);

/* Walk the lot, for `SET` with no arguments. `index` runs from zero; returns 0
   when there are no more. Order is insertion order, which is the order
   somebody's AUTOEXEC.BAT set them in and therefore the least surprising. */
int environment_at(int index, const char** name, const char** value);

/* Expand %NAME% in `input` into `output`.
 *
 * An unknown name expands to nothing, exactly as DOS did - and a lone `%` is
 * kept as itself rather than swallowed, because a percent sign in a file name
 * is legal and a shell that eats it is a shell that cannot open the file. */
void environment_expand(const char* input, char* output, boot_uint64_t size);

#endif
