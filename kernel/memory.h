#ifndef MEMORY_H
#define MEMORY_H

#include "../include/bootinfo.h"

#define PAGE_SIZE 4096ULL

/* Build the physical-page bitmap after ExitBootServices. */
void memory_init(BOOT_INFO* info);
int memory_self_test(void);

/* Allocate or release exactly one 4 KiB physical page. */
void* alloc_page(void);
void free_page(void* page);

/* Allocate one page strictly below 4 GiB. Required for anything a device
   addresses with a 32-bit pointer - the AHCI command list, FIS area and
   command table all have 32-bit base registers. Freed with free_page(). */
void* alloc_page_low(void);

/* Allocate `count` physically contiguous pages. The kernel runs on the
   firmware's identity mapping, so a buffer larger than a page - the console
   back buffer, for one - has to be contiguous in physical memory to be
   contiguous at all. */
void* alloc_pages(boot_uint64_t count);
void free_pages(void* address, boot_uint64_t count);

/* Free page count, for the `mem` command and diagnostics. */
boot_uint64_t memory_free_pages(void);

/* Every page the firmware described, usable or not - the machine's memory
   rather than the part of it we are allowed to allocate from. */
boot_uint64_t memory_physical_pages(void);

/* Size of the loaded kernel image, `.bss` included. */
boot_uint64_t memory_kernel_bytes(void);

#endif
