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

/* GCC emits calls to these regardless of -ffreestanding, so a program that
   copies a struct or zeroes an array does not fail to link. */
void* memset(void* destination, int value, unsigned long long size);
void* memcpy(void* destination, const void* source, unsigned long long size);

void* memset(void* destination, int value, unsigned long long size) {
    unsigned char* bytes = (unsigned char*)destination;
    for (unsigned long long i = 0; i < size; i++) bytes[i] = (unsigned char)value;
    return destination;
}

void* memcpy(void* destination, const void* source, unsigned long long size) {
    unsigned char* to = (unsigned char*)destination;
    const unsigned char* from = (const unsigned char*)source;
    for (unsigned long long i = 0; i < size; i++) to[i] = from[i];
    return destination;
}
