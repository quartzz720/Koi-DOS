#include "pci.h"
#include "timer.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8U
#define PCI_CONFIG_DATA 0xCFCU

#define BAR_IO_SPACE 0x1U
#define BAR_TYPE_MASK 0x6U
#define BAR_TYPE_64BIT 0x4U

static PCI_DEVICE devices[PCI_MAX_DEVICES];
static boot_uint32_t device_count;
static boot_uint32_t devices_seen;

static boot_uint32_t pci_read32(boot_uint8_t bus, boot_uint8_t device,
                                boot_uint8_t function, boot_uint8_t offset) {
    boot_uint32_t address = 0x80000000U | ((boot_uint32_t)bus << 16) |
        ((boot_uint32_t)device << 11) | ((boot_uint32_t)function << 8) |
        (offset & 0xFCU);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(boot_uint8_t bus, boot_uint8_t device,
                        boot_uint8_t function, boot_uint8_t offset,
                        boot_uint32_t value) {
    boot_uint32_t address = 0x80000000U | ((boot_uint32_t)bus << 16) |
        ((boot_uint32_t)device << 11) | ((boot_uint32_t)function << 8) |
        (offset & 0xFCU);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static void record(boot_uint8_t bus, boot_uint8_t device, boot_uint8_t function,
                   boot_uint32_t id, boot_uint32_t class_info) {
    PCI_DEVICE* entry;

    devices_seen++;
    if (device_count >= PCI_MAX_DEVICES) return;
    entry = &devices[device_count++];
    entry->bus = bus;
    entry->device = device;
    entry->function = function;
    entry->vendor_id = (boot_uint16_t)id;
    entry->device_id = (boot_uint16_t)(id >> 16);
    entry->class_code = (boot_uint8_t)(class_info >> 24);
    entry->subclass = (boot_uint8_t)(class_info >> 16);
    entry->programming_interface = (boot_uint8_t)(class_info >> 8);
    for (boot_uint8_t index = 0; index < 6; index++)
        entry->bar[index] = pci_read32(bus, device, function,
                                       (boot_uint8_t)(0x10 + index * 4));
}

void pci_scan(void) {
    device_count = 0;
    devices_seen = 0;
    for (boot_uint16_t bus = 0; bus < 256; bus++) {
        for (boot_uint8_t device = 0; device < 32; device++) {
            boot_uint32_t id = pci_read32((boot_uint8_t)bus, device, 0, 0x00);
            boot_uint32_t header;
            boot_uint8_t functions;
            if ((id & 0xFFFFU) == 0xFFFFU) continue;
            header = pci_read32((boot_uint8_t)bus, device, 0, 0x0C);
            /* Bit 23 of the header type says the device is multi-function;
               without it only function 0 exists and probing the rest wastes
               time on aliased reads. */
            functions = (header & 0x00800000U) ? 8 : 1;
            for (boot_uint8_t function = 0; function < functions; function++) {
                boot_uint32_t function_id =
                    pci_read32((boot_uint8_t)bus, device, function, 0x00);
                boot_uint32_t class_info;
                if ((function_id & 0xFFFFU) == 0xFFFFU) continue;
                class_info = pci_read32((boot_uint8_t)bus, device, function, 0x08);
                record((boot_uint8_t)bus, device, function, function_id, class_info);
            }
        }
    }
}

boot_uint32_t pci_device_count(void) {
    return device_count;
}

boot_uint32_t pci_devices_seen(void) {
    return devices_seen;
}

const PCI_DEVICE* pci_device(boot_uint32_t index) {
    return index < device_count ? &devices[index] : (const PCI_DEVICE*)0;
}

const PCI_DEVICE* pci_find(boot_uint8_t class_code, boot_uint8_t subclass,
                           boot_uint8_t programming_interface,
                           boot_uint32_t from) {
    for (boot_uint32_t index = from; index < device_count; index++) {
        const PCI_DEVICE* entry = &devices[index];
        if (class_code != PCI_ANY && entry->class_code != class_code) continue;
        if (subclass != PCI_ANY && entry->subclass != subclass) continue;
        if (programming_interface != PCI_ANY &&
            entry->programming_interface != programming_interface) continue;
        return entry;
    }
    return (const PCI_DEVICE*)0;
}

void pci_enable_bus_mastering(const PCI_DEVICE* device) {
    boot_uint32_t command_status = pci_read32(device->bus, device->device,
                                               device->function, 0x04);
    command_status |= 0x00000006U; /* memory-space + bus-master enable */
    pci_write32(device->bus, device->device, device->function, 0x04,
                command_status);
}

/* A genuine byte-wide access, not a dword read-modify-write.
 *
 * The difference matters: the registers this exists for are single bytes
 * sitting beside other single bytes, and writing the enclosing dword back
 * writes its neighbours too. One of those neighbours being write-one-to-clear
 * or reserved-must-be-zero is exactly the kind of thing that works on one
 * chipset and does something else on another. The address port takes the
 * aligned dword; the data port is addressed at byte resolution. */
boot_uint8_t pci_config_read8(const PCI_DEVICE* device, boot_uint8_t offset) {
    boot_uint32_t address = 0x80000000U | ((boot_uint32_t)device->bus << 16) |
        ((boot_uint32_t)device->device << 11) |
        ((boot_uint32_t)device->function << 8) | (offset & 0xFCU);
    outl(PCI_CONFIG_ADDRESS, address);
    return inb((boot_uint16_t)(PCI_CONFIG_DATA + (offset & 3)));
}

void pci_config_write8(const PCI_DEVICE* device, boot_uint8_t offset,
                       boot_uint8_t value) {
    boot_uint32_t address = 0x80000000U | ((boot_uint32_t)device->bus << 16) |
        ((boot_uint32_t)device->device << 11) |
        ((boot_uint32_t)device->function << 8) | (offset & 0xFCU);
    outl(PCI_CONFIG_ADDRESS, address);
    outb((boot_uint16_t)(PCI_CONFIG_DATA + (offset & 3)), value);
}

boot_uint32_t pci_config_read(const PCI_DEVICE* device, boot_uint8_t offset) {
    return pci_read32(device->bus, device->device, device->function, offset);
}

void pci_config_write(const PCI_DEVICE* device, boot_uint8_t offset,
                      boot_uint32_t value) {
    pci_write32(device->bus, device->device, device->function, offset, value);
}

/* The capability list: a chain through configuration space, each entry saying
   what it is and where the next one starts. Bounded by a step count as well as
   by the terminator, because a device that describes a loop would otherwise
   take the kernel with it. */
static boot_uint8_t find_capability(const PCI_DEVICE* device, boot_uint8_t id) {
    boot_uint32_t status = pci_config_read(device, 0x04);
    boot_uint8_t offset;
    int steps;

    if (!(status & 0x00100000U)) return 0;   /* no capability list at all */
    offset = (boot_uint8_t)(pci_config_read(device, 0x34) & 0xFC);
    for (steps = 0; offset && steps < 48; steps++) {
        boot_uint32_t header = pci_config_read(device, offset);
        if ((header & 0xFF) == id) return offset;
        offset = (boot_uint8_t)((header >> 8) & 0xFC);
    }
    return 0;
}

int pci_power_on(const PCI_DEVICE* device) {
    boot_uint8_t capability = find_capability(device, 0x01);
    boot_uint32_t control;

    /* No power management capability means the device cannot be anywhere but
       D0, which is the answer the caller wants. */
    if (!capability) return 1;

    control = pci_config_read(device, (boot_uint8_t)(capability + 4));
    if (!(control & 0x3)) return 1;

    /* Writing D0 also clears the two write-one-to-clear status bits in the top
       half of the same register if they are set, which is harmless and is what
       every other driver does. */
    pci_config_write(device, (boot_uint8_t)(capability + 4), control & ~0x3U);
    /* The device is allowed 10 ms to become usable, and reading its registers
       before then is exactly the mistake this call exists to prevent. */
    timer_wait(10);
    control = pci_config_read(device, (boot_uint8_t)(capability + 4));
    return (control & 0x3) == 0;
}

boot_uint64_t pci_bar_address(const PCI_DEVICE* device, boot_uint8_t index) {
    boot_uint32_t low;

    if (index >= 6) return 0;
    low = device->bar[index];
    if (low & BAR_IO_SPACE) return 0;
    /* A 64-bit BAR occupies this slot and the next one; the upper half is the
       whole of the following register. */
    if ((low & BAR_TYPE_MASK) == BAR_TYPE_64BIT && index < 5)
        return ((boot_uint64_t)device->bar[index + 1] << 32) |
               (low & ~0xFULL);
    return low & ~0xFULL;
}
