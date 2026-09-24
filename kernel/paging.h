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
/* Let ring 3 reach [base, base + size), and only that. Splits the 2 MiB
   leaves it needs into 4 KiB pages so that a program is not handed whatever
   else shares its neighbourhood. Returns 0 if the tables could not be grown.
 *
 * There is no way back yet, and it is honest to say so: the bit is set and
 * stays set until the tables are rebuilt. When there is a space per
 * application - which is the plan - this becomes "map it there" instead. */
int paging_allow_user(boot_uint64_t base, boot_uint64_t size);

/* ---- An address space of its own ------------------------------------------
 *
 * Ring 3 keeps a program out of the kernel; it does not keep it out of another
 * program. A space per program is what makes "its own memory" true.
 *
 * A space begins as a copy of the kernel's top level, so everything is mapped
 * where the kernel has it - which is what lets an interrupt arrive while a
 * program is running - and none of it is reachable from ring 3. Marking a
 * range user-accessible clones every table on the way down to it first, so the
 * bit lands where nobody else is looking. Four or five pages per program. */
#define PROGRAM_SPACES_MAX 8

typedef struct PAGING_SPACE PAGING_SPACE;

PAGING_SPACE* paging_space_create(void);
void paging_space_destroy(PAGING_SPACE* space);

/* Load this space's tables, or the kernel's when given nothing. */
void paging_space_enter(const PAGING_SPACE* space);

/* Whether a range is already reachable from ring 3 - the question to ask
   before running a thread on memory a program named. */
int paging_is_user(boot_uint64_t base, boot_uint64_t size);
int paging_space_is_user(PAGING_SPACE* space, boot_uint64_t base,
                         boot_uint64_t size);

int paging_space_allow_user(PAGING_SPACE* space, boot_uint64_t base,
                            boot_uint64_t size);

int paging_map_device(boot_uint64_t base, boot_uint64_t size);

/* Whether the framebuffer ended up write-combining rather than write-through.
   Worth reporting: it is the difference between a screen that can be redrawn
   sixty times a second and one that cannot, and a machine where it failed
   will otherwise only be noticeable as everything being mysteriously slow. */
int paging_write_combining(void);

#endif
