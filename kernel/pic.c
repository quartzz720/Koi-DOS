#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20U
#define PIC1_DATA 0x21U
#define PIC2_COMMAND 0xA0U
#define PIC2_DATA 0xA1U

#define ICW1_INIT 0x11U   /* start init sequence, expect ICW4 */
#define ICW4_8086 0x01U   /* 8086/88 mode */
#define PIC_EOI 0x20U

#define PIC1_VECTOR_BASE 0x20U
#define PIC2_VECTOR_BASE 0x28U

void pic_init(void) {
    /* Start the initialisation sequence on both chips. io_wait() between
       writes because the 8259 is old enough to need the settling time. */
    outb(PIC1_COMMAND, ICW1_INIT); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT); io_wait();
    outb(PIC1_DATA, PIC1_VECTOR_BASE); io_wait();
    outb(PIC2_DATA, PIC2_VECTOR_BASE); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();  /* slave is wired to IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* slave identity: cascade line 2 */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Everything masked. Drivers unmask their own line as they come up, so a
       device the kernel cannot yet service never interrupts it. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask_irq(boot_uint8_t irq) {
    boot_uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    boot_uint8_t bit = (boot_uint8_t)(irq < 8 ? irq : irq - 8);
    outb(port, (boot_uint8_t)(inb(port) | (1U << bit)));
}

void pic_unmask_irq(boot_uint8_t irq) {
    boot_uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    boot_uint8_t bit = (boot_uint8_t)(irq < 8 ? irq : irq - 8);
    outb(port, (boot_uint8_t)(inb(port) & ~(1U << bit)));
    /* A line on the slave only reaches the CPU if the cascade line is open. */
    if (irq >= 8) pic_unmask_irq(2);
}

void pic_send_eoi(boot_uint8_t irq) {
    if (irq >= 8) outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}
