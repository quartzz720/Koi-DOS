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

/* What the tables themselves cost, in bytes. Worth showing next to the map
   they describe: 2 MiB leaves are chosen precisely to keep this small. */
boot_uint64_t paging_table_bytes(void);

/* Map a device's register window into the identity map, uncached.
 *
 * The boot-time map covers the first 4 GiB and every region the firmware
 * described, which is not the same as every region a device uses: a 64-bit BAR
 * can be assigned far above RAM - QEMU puts xHCI at 768 GiB - and mapping
 * everything up to there would mean mapping most of a terabyte of nothing.
 * So a driver asks for its own window before touching it.
 *
 * The mapping is cache-disabled, because device registers are not memory and
 * a cached read of a status register returns whatever it said last time.
 * Returns 0 if the tables could not be extended. */
int paging_map_device(boot_uint64_t base, boot_uint64_t size);

/* Whether the framebuffer ended up write-combining rather than write-through.
   Worth reporting: it is the difference between a screen that can be redrawn
   sixty times a second and one that cannot, and a machine where it failed
   will otherwise only be noticeable as everything being mysteriously slow. */
int paging_write_combining(void);

#endif
