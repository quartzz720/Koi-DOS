#ifndef KERNEL_SERIAL_H
#define KERNEL_SERIAL_H

#include "../include/bootinfo.h"

/* COM1 debug output. This is the only channel that survives a failure which
   happens before the framebuffer console can draw anything, so it is brought
   up first in kernel_main() and every console_write() is mirrored to it.
   In QEMU it lands in the terminal via -serial stdio. */

void serial_init(void);
void serial_putchar(char character);
void serial_write(const char* text);

/* Print an unsigned value; used by the exception handlers and diagnostics. */
void serial_write_hex(boot_uint64_t value);
void serial_write_dec(boot_uint64_t value);

#endif
