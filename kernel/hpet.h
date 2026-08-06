#ifndef KERNEL_HPET_H
#define KERNEL_HPET_H

#include "../include/bootinfo.h"

/* The High Precision Event Timer, used here as a clock rather than as a source
 * of events.
 *
 * The division of labour is deliberate. The HPET sits on the chipset, behind
 * the system bus, so every read of its counter is a slow MMIO round trip -
 * wrong for something ticking a thousand times a second. What it is good at is
 * being monotonic and independent of the processor's clock, which makes it the
 * right thing to calibrate the Local APIC timer against and the right thing to
 * measure a microsecond delay with.
 *
 * It may simply not be there. Some firmware hides or disables it, so every
 * caller has to cope with `hpet_available()` returning zero. */

/* Find it through ACPI, map it, and start its counter. Returns 1 when it is
   running. */
int hpet_init(void);
int hpet_available(void);

/* Ticks per second. Zero when there is no HPET. */
boot_uint32_t hpet_frequency(void);

/* The low 32 bits of the main counter.
 *
 * Deliberately not the full 64: an HPET is allowed to implement only a 32-bit
 * counter, and a 64-bit read of one that does not returns something that is
 * not a wider version of the same number. Thirty-two bits wrap every few
 * minutes, which is invisible to every user here because they all subtract two
 * readings - and unsigned subtraction is correct across a wrap. */
boot_uint32_t hpet_counter(void);

/* Busy-wait, accurately, for short intervals. This is what hardware bring-up
   needs and what the millisecond tick counter cannot express. */
void hpet_delay_us(boot_uint32_t microseconds);

#endif
