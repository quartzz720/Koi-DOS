#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

#include "../include/bootinfo.h"

/* A general-purpose allocator over the page allocator. The page allocator
   hands out 4 KiB at a time, which is the wrong granularity for path strings,
   directory entries and FAT buffers - all of which Stage 2 needs by the
   hundred. */

void heap_init(void);

void* kmalloc(boot_uint64_t size);
void* kcalloc(boot_uint64_t size);
void kfree(void* pointer);

/* Bytes currently handed out, and total arena size. For the `mem` command. */
boot_uint64_t heap_used(void);
boot_uint64_t heap_total(void);

/* Allocate, write, free and check that the space comes back. Returns 0 on
   failure. Nothing else uses the heap yet, so without this it would ship
   unexercised. */
int heap_self_test(void);

#endif
