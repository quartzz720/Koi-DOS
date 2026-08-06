#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include "../include/bootinfo.h"

/* Segment selectors into our own GDT. The IDT stores one of these in every
   gate, so the GDT has to be installed before the IDT. */
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
#define TSS_SELECTOR 0x18

/* Interrupt stack table slot used for the double-fault handler. */
#define IST_DOUBLE_FAULT 1

/* Replace the firmware GDT with our own. Until this runs the kernel is using
   descriptors that belonged to UEFI and that nothing obliges it to preserve
   after ExitBootServices. */
void gdt_init(void);

/* Install the task state segment and give the double-fault vector a stack of
   its own. Without it, a fault caused by a bad kernel stack pushes its own
   frame onto that same bad stack and the machine triple-faults - losing
   exactly the diagnostic the panic screen exists to print. */
void tss_init(void);

static inline void cpu_halt(void) {
    __asm__ volatile ("hlt");
}

static inline void cpu_disable_interrupts(void) {
    __asm__ volatile ("cli" : : : "memory");
}

static inline void cpu_enable_interrupts(void) {
    __asm__ volatile ("sti" : : : "memory");
}

/* Stop for good: mask interrupts and park the core. Used by the panic screen. */
__attribute__((noreturn)) static inline void cpu_hang(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

#endif
