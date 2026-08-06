#include "koi.h"

/* Every program's real entry point. Calls main() and turns its return value
   into an exit, so a program that simply returns still ends cleanly rather
   than running off the end of its own code. */

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
