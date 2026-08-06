#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include "../include/bootinfo.h"

/* Our own page tables.
 *
 * Until this runs the kernel is executing on the page tables the firmware
 * built. UEFI is required to identity-map memory while boot services are
 * alive, but nothing obliges the firmware to keep those tables valid - or to
 * have mapped anything above what it happened to need - once ExitBootServices
 * has returned. Owning the tables removes that dependency and makes memory
 * above 4 GiB addressable on our terms.
 *
 * Returns 0 if the tables could not be built, in which case the firmware
 * mapping is left untouched and still in force. */
int paging_init(const BOOT_INFO* info);

/* Total physical memory covered by the identity map. */
boot_uint64_t paging_mapped_bytes(void);

#endif
