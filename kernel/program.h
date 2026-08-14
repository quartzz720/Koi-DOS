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

/* The window is divided into slots so that more than one program can be
 * resident at once.
 *
 * DOS did this with EXEC: a program could run another, and got control back
 * when it ended, with everything it had in memory still there. That is what
 * this is for. It is not multitasking - only one of them is running at any
 * moment, and the caller is stopped inside the call - but it is the whole of
 * what "run this and come back" needs, and it is what SYS_CHAIN was standing
 * in for while the machine could hold one image.
 *
 * Programs are position-independent now, so a slot is only an address: the
 * loader adds the slot's base to everything the linker left relative. Four of
 * them, four megabytes each, the top 256 KiB of every one being its stack. */
/* Room at the top of a slot for the command line the program is given. In its
   own memory, because a program must be able to read it - which at ring 3 is
   a rule the processor enforces. DOS called this part of the PSP. */
#define PROGRAM_ARGUMENTS_MAX 256

#define PROGRAM_SLOTS 4
#define PROGRAM_SLOT_SIZE ((PROGRAM_LIMIT - PROGRAM_BASE) / PROGRAM_SLOTS)
#define PROGRAM_SLOT_BASE(n) (PROGRAM_BASE + (boot_uint64_t)(n) * PROGRAM_SLOT_SIZE)
#define PROGRAM_SLOT_TOP(n) (PROGRAM_SLOT_BASE(n) + PROGRAM_SLOT_SIZE)

/* Load and run `path`, passing `arguments` as its command line.
 *
 * Returns PROGRAM_OK, PROGRAM_NOT_LOADABLE or PROGRAM_REFUSED. The program's
 * own exit code goes into `exit_code`, which may be null when nobody cares,
 * and is untouched unless the program actually ran. */
/* How many programs are resident. Zero at the prompt, one inside a program,
   two inside a program a program started. */
int program_depth(void);

int program_run(VOLUME* volume, const char* path, const char* arguments,
                int* exit_code);

/* Run the next program at ring 3, in its own window of memory.
 *
 * The DOS contract is ring 0 and a flat machine, and that stays: a program
 * typed at the prompt can read the processor's name and write to the screen,
 * which is what programs written for this expect. This is the other contract,
 * for code that should not be able to take the machine with it - and the test
 * bench for the isolation Mizu's applications are going to live in. */
void program_run_next_at_ring3(void);

/* Load an image and stop there: segments copied, relocations applied, the
 * interface version checked, and the entry point handed back rather than
 * jumped to. Backs SYS_LOAD.
 *
 * The memory comes from the page allocator rather than from a program slot,
 * because the program doing the loading is in a slot itself and the next one
 * belongs to whatever it runs next. Returns PROGRAM_OK, PROGRAM_NOT_LOADABLE
 * or PROGRAM_REFUSED - and, as with program_run, REFUSED means the reason has
 * already been said out loud. */
int program_load(VOLUME* volume, const char* path, boot_uint64_t* base,
                 boot_uint64_t* size, boot_uint64_t* entry);

/* Give a loaded module's memory back. Returns 0 for a base that was never
   handed out, which is a refusal and not a failure: freeing an address a
   program invented would put somebody else's pages back in the allocator. */
int program_unload(boot_uint64_t base);

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
/* Four is the shape of the longest request a shell actually makes - change
   drive, run the program, change back, restart the shell - so the limit is
   eight, because a limit that exactly equals the known case leaves no room for
   the next one and fails by silently dropping a request. */
#define PROGRAM_CHAIN_DEPTH 8
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
