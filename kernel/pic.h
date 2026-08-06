#ifndef KERNEL_PIC_H
#define KERNEL_PIC_H

#include "../include/bootinfo.h"

/* The two cascaded 8259 controllers. They power up delivering IRQ0-7 on
   vectors 8-15, which in long mode collide with the CPU's own exceptions -
   a timer tick would arrive looking like a double fault. pic_init() moves
   them to 32-47. */

void pic_init(void);
void pic_mask_irq(boot_uint8_t irq);
void pic_unmask_irq(boot_uint8_t irq);
void pic_send_eoi(boot_uint8_t irq);

#endif
