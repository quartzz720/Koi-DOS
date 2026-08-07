#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include "../include/bootinfo.h"

/* A desktop board with several NVMe slots, two or three USB controllers, audio,
   network and a GPU passes thirty functions without trying. The table used to
   hold 32 and drop the rest in silence, which presents as a driver simply not
   finding its device. */
#define PCI_MAX_DEVICES 64

/* Class codes the kernel cares about, now and soon. */
#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA 0x06
#define PCI_PROGIF_AHCI 0x01
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROGIF_NVME 0x02
#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03
#define PCI_PROGIF_XHCI 0x30
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_HD_AUDIO 0x03

#define PCI_ANY 0xFF

typedef struct {
    boot_uint8_t bus;
    boot_uint8_t device;
    boot_uint8_t function;
    boot_uint16_t vendor_id;
    boot_uint16_t device_id;
    boot_uint8_t class_code;
    boot_uint8_t subclass;
    boot_uint8_t programming_interface;
    boot_uint32_t bar[6];
} PCI_DEVICE;

/* Walk configuration space once and remember what is there. Drivers then look
   themselves up, instead of each one re-scanning the whole bus for its own
   class the way pci_find_ahci() used to. */
void pci_scan(void);

boot_uint32_t pci_device_count(void);
const PCI_DEVICE* pci_device(boot_uint32_t index);

/* How many functions the scan actually saw. Larger than pci_device_count()
   means the table filled up and devices were dropped - which is worth saying
   out loud rather than leaving a driver to report "not found". */
boot_uint32_t pci_devices_seen(void);

/* First device matching class/subclass/programming interface at or after
   index `from`, or NULL. PCI_ANY in a field accepts any value. */
const PCI_DEVICE* pci_find(boot_uint8_t class_code, boot_uint8_t subclass,
                           boot_uint8_t programming_interface,
                           boot_uint32_t from);

void pci_enable_bus_mastering(const PCI_DEVICE* device);

/* Resolve a memory BAR to its base address, joining the two halves of a
   64-bit BAR. Returns 0 for an I/O-space BAR. */
boot_uint64_t pci_bar_address(const PCI_DEVICE* device, boot_uint8_t index);

#endif
