#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include "../include/bootinfo.h"

/* Just enough ACPI to answer hardware-presence questions. The bootloader
   already found the RSDP and passed it in BOOT_INFO; nothing had used it until
   now. This will grow when HPET and the APIC need the MADT. */

void acpi_init(boot_uint64_t rsdp_address);

/* Find a table by its four-character signature, e.g. "FACP" or "APIC".
   Returns NULL when absent or when ACPI could not be parsed at all. */
const void* acpi_find_table(const char signature[4]);

/* True when the FADT says the machine has an 8042 keyboard controller.
   On a UEFI machine with no legacy hardware, probing port 0x60 blindly reads
   garbage from a device that is not there. When ACPI is missing or too old to
   carry the flag, this answers "yes" - that is the safe assumption for the
   older machines that lack it. */
int acpi_has_8042(void);

#endif
