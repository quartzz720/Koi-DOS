#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include "../include/bootinfo.h"

#define TIMER_HZ 1000U

/* PIT channel 0 polling timer. No interrupts are enabled or required. */
void timer_init(void);
void timer_poll(void);
boot_uint64_t timer_ticks(void);

/* Deadlines, written this way on purpose.
 *
 * Every driver that waits for hardware needs the same two things: "has long
 * enough passed" and "hold still for a moment". Spelling those out as explicit
 * `timer_ticks()` arithmetic plus a `timer_poll()` loop puts the choice of
 * time source into every driver, which is exactly what makes it expensive to
 * change. Behind these two calls it is one place.
 *
 * Both keep the time source advancing while they wait, so they are safe to
 * call whether or not the source is interrupt-driven. */
int timer_expired(boot_uint64_t start, boot_uint64_t milliseconds);
void timer_wait(boot_uint64_t milliseconds);

/* Hand the count over to an interrupt. After this `timer_poll()` does nothing
   and the tick arrives on its own, which is the only way the count stays
   honest across work that does not stop to poll. */
void timer_use_interrupt(void);
int timer_is_interrupt_driven(void);

/* One millisecond has passed. Called from the timer interrupt handler. */
void timer_tick(void);

#endif
