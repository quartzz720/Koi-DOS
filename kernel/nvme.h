#ifndef KERNEL_NVME_H
#define KERNEL_NVME_H

#include "pci.h"

/* Bring up one NVMe controller and register every namespace on it with the
   block layer. Returns 1 when at least one was taken.
 *
 * Both halves used to be singular: one controller, and namespace 1 on it. Two
 * M.2 sockets is an ordinary desktop, and the second drive was not failing or
 * being reported - it was never looked at. */
int nvme_init(const PCI_DEVICE* controller);

/* How many namespaces are registered, across every controller. */
boot_uint32_t nvme_namespace_count(void);

/* The size of one of them, by the same numbering the block layer uses. */
boot_uint64_t nvme_sector_count(boot_uint32_t index);
boot_uint32_t nvme_sector_size(boot_uint32_t index);

#endif
