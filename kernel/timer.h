#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include "../include/bootinfo.h"

#define TIMER_HZ 1000U

/* PIT channel 0 polling timer. No interrupts are enabled or required. */
void timer_init(void);
void timer_poll(void);
boot_uint64_t timer_ticks(void);

#endif
