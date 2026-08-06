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

/* The MADT, parsed once and answered from.
 *
 * It describes the interrupt hardware: where the Local APIC registers are,
 * where the I/O APICs are, and which legacy IRQs the firmware has rewired to
 * different global interrupt lines. That last part is not a formality - IRQ0
 * is remapped on most machines, and routing it as though it were not sends the
 * timer interrupt somewhere nothing is listening. */
int acpi_madt_present(void);

/* Physical address of the Local APIC registers, honouring the 64-bit override
   entry when the firmware supplies one. Zero when unknown. */
boot_uint64_t acpi_local_apic_address(void);

/* First I/O APIC, and the global interrupt number its first input carries. */
boot_uint64_t acpi_io_apic_address(void);
boot_uint32_t acpi_io_apic_base(void);

/* Which global interrupt a legacy ISA IRQ actually arrives on, and how it is
   triggered. Returns the IRQ itself when the firmware listed no override,
   which is what the standard identity mapping means. */
boot_uint32_t acpi_interrupt_override(boot_uint8_t irq, int* active_low,
                                      int* level_triggered);

/* Turn the machine off, through ACPI's soft-off state.
 *
 * There is no other way. The power button is a request to the firmware, not
 * something software can press, and the old APM calls are long gone. Returns 0
 * when the tables do not describe how - in which case nothing happened and the
 * caller has to say so. On success it does not return.
 *
 * Finding out how requires reading a few bytes of AML out of the DSDT, which
 * is a bytecode this system has no interpreter for. What it does instead is
 * what every small operating system does: look for the four bytes `_S5_` and
 * decode the short package that follows. Fragile in principle, universal in
 * practice, and the alternative is an AML interpreter. */
int acpi_power_off(void);

/* Restart through ACPI's reset register, when the tables describe one.
   Returns 0 when they do not; the caller then has a cruder way. */
int acpi_reset(void);

/* Physical address of the HPET registers, or zero when the machine has none -
   which happens more often than it should, because some firmware hides it. */
boot_uint64_t acpi_hpet_address(void);

#endif
