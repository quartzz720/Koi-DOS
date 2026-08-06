#ifndef KERNEL_AHCI_H
#define KERNEL_AHCI_H

#include "pci.h"

/* Bring up the first ATA disk on the controller and register it with the
   block layer. Returns 1 on success. */
int ahci_init(const PCI_DEVICE* controller);

/* Direct access to the configured port. The block layer wraps these; call
   those instead unless you are the wrapper. */
int disk_read(boot_uint64_t lba, boot_uint16_t count, void* buffer);
int disk_write(boot_uint64_t lba, boot_uint16_t count, const void* buffer);

/* Sector count reported by IDENTIFY DEVICE, or 0 if it could not be read. */
boot_uint64_t ahci_sector_count(void);

#endif
