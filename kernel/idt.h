#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include "../include/bootinfo.h"

#define IDT_ENTRIES 256
#define IRQ_BASE 32
#define IRQ_COUNT 16

/* What the stubs in isr.S leave on the stack. The register order matches their
   push order; changing one without the other corrupts every dump. */
typedef struct {
    boot_uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    boot_uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    boot_uint64_t vector;
    boot_uint64_t error_code;
    /* Pushed by the CPU itself. */
    boot_uint64_t rip, cs, rflags, rsp, ss;
} INTERRUPT_FRAME;

typedef void (*IRQ_HANDLER)(INTERRUPT_FRAME* frame);

void idt_init(void);

/* Register a handler for one of the 16 hardware IRQs. The dispatcher sends the
   end-of-interrupt to the PIC afterwards, so handlers do not do it. */
void irq_register(boot_uint8_t irq, IRQ_HANDLER handler);

/* Stop everything and print the machine state. Never returns. */
__attribute__((noreturn)) void panic(const char* message);

#endif
