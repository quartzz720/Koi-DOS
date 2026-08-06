#ifndef KERNEL_IO_H
#define KERNEL_IO_H

#include "../include/bootinfo.h"

/* x86 port I/O. Shared by every driver that talks to a legacy device:
   the PIT (timer.c), PCI configuration space (pci.c), COM1 (serial.c)
   and later the 8042 keyboard controller. */

static inline void outb(boot_uint16_t port, boot_uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline boot_uint8_t inb(boot_uint16_t port) {
    boot_uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(boot_uint16_t port, boot_uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline boot_uint32_t inl(boot_uint16_t port) {
    boot_uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Write to an unused port to burn one bus cycle. Some legacy chips need a
   short settling delay between consecutive writes. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
