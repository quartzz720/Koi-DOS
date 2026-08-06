#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include "../include/bootinfo.h"

/* GCC emits calls to these four even under -ffreestanding: it rewrites
   struct assignments, array initialisers and large local zeroing into them.
   Nothing has tripped that yet only because the kernel is still small, so
   they must exist before any real data structures appear. */

void* memset(void* destination, int value, boot_uint64_t size);
void* memcpy(void* destination, const void* source, boot_uint64_t size);
void* memmove(void* destination, const void* source, boot_uint64_t size);
int memcmp(const void* left, const void* right, boot_uint64_t size);

boot_uint64_t strlen(const char* text);
int strcmp(const char* left, const char* right);

/* Case-insensitive glob: `*` matches any run of characters, `?` exactly one.
   Shared by the shell and by the directory-enumeration system calls, so the
   two cannot disagree about what a pattern means. */
int glob_match(const char* pattern, const char* name);

#endif
