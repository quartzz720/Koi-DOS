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
/* Restart the machine, by the crudest method that always works.
 *
 * An empty interrupt descriptor table means the next interrupt cannot be
 * delivered, nor can the fault about not delivering it, nor the fault about
 * that - and three deep the processor stops trying and resets. The polite
 * routes are ACPI's reset register and pulsing the 8042, and neither is
 * universal: the first is optional and the second needs a controller this
 * machine may not have. This one needs nothing. */
__attribute__((noreturn)) static inline void cpu_reset(void) {
    struct __attribute__((packed)) { boot_uint16_t limit; boot_uint64_t base; }
        nothing = { 0, 0 };

    __asm__ volatile ("cli");
    __asm__ volatile ("lidt %0" : : "m"(nothing));
    __asm__ volatile ("int $3");
    for (;;) __asm__ volatile ("hlt");
}

__attribute__((noreturn)) static inline void cpu_hang(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

#endif
