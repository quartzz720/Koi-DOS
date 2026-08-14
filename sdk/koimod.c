#include "koi.h"

/* The entry point of a loadable module.
 *
 * A module is an ordinary Koi-DOS program image that nobody runs. Something
 * else loads it with SYS_LOAD, calls its entry point like any other function,
 * and keeps whatever comes back - so the two are in memory together and the
 * loader decides when the module's code runs. Windows 3.0 held its
 * applications this way, and a VxD's service table was the same idea one ring
 * down.
 *
 * This file is start.c's counterpart and differs from it in one respect that
 * is the whole point: it does not call koi_exit. A program's entry ends the
 * program; a module's entry returns to whoever called it, because that caller
 * is still running and is waiting for an answer.
 *
 * What the argument and the answer mean is not settled here. The kernel does
 * not know and does not need to: it copies segments, fixes up addresses and
 * hands back where the entry landed. The agreement is between the module and
 * the program that loads it - Mizu's is in mizu.h - which is why one call
 * covers every such arrangement rather than one per kind of plug-in.
 */

/* Which interface this was built against, at the load address, where the
   loader reads it before running a single instruction. The check is the same
   one programs get: a module built against a different interface is refused
   rather than called and quietly misunderstood. */
__attribute__((section(".koi_abi"), used))
const KOI_PROGRAM_HEADER koi_program_header = {
    KOI_PROGRAM_MAGIC, KOI_ABI_VERSION, { 0, 0 }
};

void* module_main(void* argument);

void* _start(void* argument);

void* _start(void* argument) {
    return module_main(argument);
}
