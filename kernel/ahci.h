#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include "pci.h"

/* Bring up every ATA disk on one controller and register each with the block
   layer. Returns how many were taken.
 *
 * Plural on both counts, and it was singular on both: one controller, and the
 * first port on it with a disk. A desktop board has six ports and people fill
 * them, and a machine whose system disk happened to be wired to the second
 * socket looked to this driver like a machine with one disk that was not
 * bootable. */
int ahci_init(const PCI_DEVICE* controller);

/* How many disks have been taken across every controller. */
boot_uint32_t ahci_disk_count(void);

/* Sector count of one of them, as IDENTIFY DEVICE reported it, or 0. */
boot_uint64_t ahci_sector_count(boot_uint32_t index);

#endif
