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

/* Called by SYS_EXIT. Unwinds straight back into program_run(). */
__attribute__((noreturn)) void program_exit(int code);

#endif
