#ifndef KERNEL_APIC_H
#define KERNEL_APIC_H

#include "../include/bootinfo.h"

/* The Local APIC and the I/O APIC: the interrupt hardware every machine built
 * this century actually uses, with the 8259 kept around only for boot.
 *
 * The division of labour between the two timers is deliberate. The **Local
 * APIC timer is the source of events**: it sits on the processor die, so
 * reading and programming it costs nothing much, and it is per-CPU, which
 * means that if this system ever grows more than one processor its tick needs
 * no locking. The **HPET is the source of time** - out on the chipset, slow to
 * read, but monotonic and independent of the processor's clock, which makes it
 * exactly right for measuring how fast the APIC timer runs.
 *
 * That last part is not optional. The APIC timer's frequency is not written
 * down anywhere the OS can read on AMD hardware - the CPUID leaves that give
 * it are Intel's - so it has to be measured against a clock that is known. */

/* Enable the Local APIC and measure its timer. Returns 1 when it is usable. */
int apic_init(void);
int apic_available(void);

/* The measured timer frequency, in hertz. Zero before calibration. */
boot_uint32_t apic_timer_frequency(void);

/* Start the timer interrupting `hz` times a second on `vector`. */
int apic_start_timer(boot_uint32_t hz, boot_uint8_t vector);

/* Acknowledge the interrupt currently being serviced. Every interrupt
   delivered through the APIC needs this, and none delivered through the 8259
   does - which is why the dispatcher has to know which is in use. */
void apic_end_of_interrupt(void);

/* Route a legacy ISA IRQ to `vector` through the I/O APIC, honouring whatever
   the firmware said about where that IRQ really arrives and how it is
   triggered. Returns 1 when the route was programmed. */
int apic_route_irq(boot_uint8_t irq, boot_uint8_t vector);

#endif
