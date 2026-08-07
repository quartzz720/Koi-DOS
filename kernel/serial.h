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

/* Everything that went to COM1, kept in memory as well.
 *
 * The serial port is where every driver says what it found and why it gave up,
 * and it is the one channel that survives a failure before anything can be
 * drawn. It is also absent from every machine made this century. A laptop is
 * exactly where the drivers behave in ways QEMU never will, and losing the
 * explanation on precisely those machines is the wrong way round - so it is
 * captured here too, and `log` prints it or writes it to a file.
 *
 * The buffer fills and then stops rather than wrapping: the interesting part
 * of a boot log is the beginning, and a ring would eat it first. */
#define BOOT_LOG_SIZE 65536

const char* boot_log(void);
boot_uint32_t boot_log_length(void);
int boot_log_truncated(void);

#endif
