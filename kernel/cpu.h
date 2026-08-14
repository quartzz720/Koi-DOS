#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include "../include/bootinfo.h"

/* Segment selectors into our own GDT. The IDT stores one of these in every
   gate, so the GDT has to be installed before the IDT. */
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
/* Ring 3's own segments. The requested privilege level is part of the
   selector, which is why these end in 3 - a selector for ring 3 is not the
   same number as the entry it points at. */
#define USER_CODE_SELECTOR 0x1B      /* entry 3, RPL 3 */
#define USER_DATA_SELECTOR 0x23      /* entry 4, RPL 3 */
#define TSS_SELECTOR 0x28            /* entries 5 and 6 */

/* The stack interrupts arrive on when they arrive from ring 3. Nothing may be
   trusted about the stack an application was using, so the processor is given
   one of the kernel's own to switch to - that is what TSS.RSP0 is for, and it
   is the whole reason the TSS still exists in long mode. */
#define KERNEL_INTERRUPT_STACK_PAGES 4

/* Leave ring 0 and run `entry` at ring 3 with `stack` as its stack. Does not
   return: the only ways back are a system call, a fault, or the program
   exiting - all of which arrive as interrupts. */
__attribute__((noreturn)) void cpu_enter_user(boot_uint64_t entry,
                                              boot_uint64_t stack);

/* Where interrupts from ring 3 will find a kernel stack. Set before anything
   is entered at ring 3, and again whenever that stack changes. */
void cpu_set_kernel_stack(boot_uint64_t top);

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

/* Push a range of memory out of the cache, and order it against what follows.
 *
 * On x86 a device's DMA snoops the caches, so this is normally unnecessary -
 * and normally is not always. An HD Audio controller marks its stream traffic
 * with a priority bit that some chipsets tie to non-snooped transfers, and a
 * non-snooped read of a buffer the CPU has only written into its own cache
 * returns whatever was in memory before: usually zeros. Nothing fails, no
 * error bit is set, and the device simply transfers nothing.
 *
 * Costs three cache lines a millisecond where it is not needed, and is the
 * difference between sound and silence where it is. */
void cpu_flush_cache(const void* address, boot_uint64_t bytes);

static inline void cpu_halt(void) {
    __asm__ volatile ("hlt");
}

static inline void cpu_disable_interrupts(void) {
    __asm__ volatile ("cli" : : : "memory");
}

static inline void cpu_enable_interrupts(void) {
    __asm__ volatile ("sti" : : : "memory");
}

/* Shut interrupts out for a moment and put them back exactly as they were.
 *
 * A plain cli/sti pair around a short critical section is wrong in one case
 * that is easy to reach and hard to see: called with interrupts already off -
 * from inside another handler, or during start-up before they are first
 * enabled - the `sti` turns them on early rather than leaving them alone.
 * Saving the flag makes the pair say what it means. */
static inline boot_uint64_t cpu_hold_interrupts(void) {
    boot_uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void cpu_release_interrupts(boot_uint64_t flags) {
    if (flags & 0x200ULL) __asm__ volatile ("sti" : : : "memory");
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
