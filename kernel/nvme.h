#ifndef KERNEL_NVME_H
#define KERNEL_NVME_H

#include "pci.h"

/* NVMe, the interface every solid-state drive made this decade speaks.
 *
 * Structurally the simplest of the three storage drivers, and by some way the
 * newest: no command FIS to assemble as with AHCI, no three-phase transport as
 * with USB. A command is 64 bytes written into a ring, a doorbell says how far
 * the ring has been filled, and a completion turns up in a second ring with a
 * phase bit marking whose it is. That is the whole model - and it is the same
 * model as the xHCI rings, which is why this arrives quickly after them.
 *
 * Registers through block.c like everything else, so the filesystem never
 * learns which kind of controller it is reading. */

/* Bring up one NVMe controller and register its first namespace as a block
   device. Returns 1 when a namespace is usable. */
int nvme_init(const PCI_DEVICE* controller);

/* Sectors on the namespace that was registered, and their size. Zero when no
   controller came up. */
boot_uint64_t nvme_sector_count(void);
boot_uint32_t nvme_sector_size(void);

#endif
