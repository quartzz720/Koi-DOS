#ifndef KERNEL_PROGRAM_H
#define KERNEL_PROGRAM_H

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

/* Load and run `path`, passing `arguments` as its command line. Returns the
   program's exit code, or -1 when it could not be started. */
int program_run(VOLUME* volume, const char* path, const char* arguments);

/* The command line the running program was given. Backs SYS_ARGS. */
const char* program_arguments(void);

/* Called by SYS_EXIT. Unwinds straight back into program_run(). */
__attribute__((noreturn)) void program_exit(int code);

#endif
