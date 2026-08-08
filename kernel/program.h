#ifndef KERNEL_PROGRAM_H
#define KERNEL_PROGRAM_H

/* What program_run() did, which is not the same question as what the program
 * returned.
 *
 * These used to share one value with the exit code, and a program exiting -1
 * was reported as "not a valid Koi-DOS program" - which is a perfectly
 * ordinary thing for a program to do when it fails, and DOOM does it. There is
 * no value that is safe to steal from a program's own return, so the two
 * answers are separate now.
 *
 * PROGRAM_REFUSED means the reason has already been printed; the caller should
 * not add a second, vaguer message on top of it. */
#define PROGRAM_OK 0
#define PROGRAM_NOT_LOADABLE 1
#define PROGRAM_REFUSED 2

#include "partition.h"

/* Where a program is loaded, and how much room it gets.
 *
 * Programs are ELF64 linked -no-pie at a fixed address, exactly like the
 * kernel: one program runs at a time, in ring 0, in the same address space, so
 * a fixed load address is both sufficient and the simplest thing that works.
 * DOS made the same trade for the same reason.
 *
 * memory_init() reserves this range, so the page allocator never hands out
 * memory a program is about to be loaded into. */
#define PROGRAM_BASE 0x1000000ULL              /* 16 MiB */
#define PROGRAM_LIMIT 0x2000000ULL             /* 32 MiB */
#define PROGRAM_STACK_SIZE 0x40000ULL          /* 256 KiB, at the top */

/* Load and run `path`, passing `arguments` as its command line.
 *
 * Returns PROGRAM_OK, PROGRAM_NOT_LOADABLE or PROGRAM_REFUSED. The program's
 * own exit code goes into `exit_code`, which may be null when nobody cares,
 * and is untouched unless the program actually ran. */
int program_run(VOLUME* volume, const char* path, const char* arguments,
                int* exit_code);

/* The command line the running program was given. Backs SYS_ARGS. */
const char* program_arguments(void);

/* Where the running program was loaded from, from the root of its drive. A
   program cannot work this out for itself, and one that asks to be run again
   after something else needs it. */
const char* program_path(void);

/* Called by SYS_EXIT. Unwinds straight back into program_run(). */
__attribute__((noreturn)) void program_exit(int code);

/* ---- Chaining ------------------------------------------------------------
 *
 * One program runs at a time, at a fixed address, in one address space. That is
 * the whole of the design and it is not going to change - so a program cannot
 * call another program the way a program on a larger system can. Which would
 * mean a graphical shell that cannot start anything, and a shell that cannot
 * start anything is a picture of a shell.
 *
 * So a program asks for something to be run AFTER it has exited. It is gone by
 * then, its memory is free, and the thing it asked for loads into the space it
 * was occupying. No nesting, no second program in memory, nothing saved and
 * restored.
 *
 * Requests are honoured most-recent-first, which is what makes "run this and
 * then bring me back" a pair of ordinary calls:
 *
 *     program_chain("MIZU Z:\\GAMES");   asked for second, runs second
 *     program_chain("DOOM");             asked for last, runs first
 *
 * Coming back is a fresh start, not a resume - so a shell hands itself whatever
 * it needs to pick up where it was, as arguments. That is a real constraint and
 * it is stated rather than hidden: an editor with unsaved work must save before
 * chaining, because nothing of it survives.
 *
 * This is not novel. Small DOS shells did exactly this, for exactly this
 * reason: exit, let the program have the memory, and reload afterwards.
 */
#define PROGRAM_CHAIN_DEPTH 4
#define PROGRAM_CHAIN_MAX 256

/* Ask for `command` to be run once the current program has exited. It is a
   command line, not a path: it goes back through the shell, so drive letters,
   the program search path, arguments and batch files all behave as if typed.
   Returns 1, or 0 when too many requests are already waiting. */
int program_chain(const char* command);

/* Take the next request, most recent first, into `command`. Returns 0 when
   there is nothing waiting. Taking removes it, so that a request cannot be run
   twice by a caller that loops. */
int program_chain_take(char* command, boot_uint64_t size);

/* Throw away every waiting request. */
void program_chain_clear(void);

#endif
