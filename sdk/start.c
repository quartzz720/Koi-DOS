#include "koi.h"

/* Every program's real entry point. Calls main() and turns its return value
   into an exit, so a program that simply returns still ends cleanly rather
   than running off the end of its own code. */

/* Which interface this program was built against, stamped where the kernel
   looks for it. The linker script puts this section at the load address, so
   the check happens before the program runs rather than after it has already
   called something that has since changed meaning. */
__attribute__((section(".koi_abi"), used))
const KOI_PROGRAM_HEADER koi_program_header = {
    KOI_PROGRAM_MAGIC, KOI_ABI_VERSION, { 0, 0 }
};

int main(const char* arguments);

void _start(void);

void _start(void) {
    koi_exit(main(koi_arguments()));
}

/* memset and memcpy used to live here, because GCC emits calls to them
   regardless of -ffreestanding and a program that copies a struct would
   otherwise fail to link. They are in koilib.c now, along with the other two
   the compiler can ask for and everything else a program of any size needs. */
