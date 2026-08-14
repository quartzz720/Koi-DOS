#include "idt.h"
#include "cpu.h"
#include "pic.h"
#include "apic.h"
#include "console.h"
#include "program.h"
#include "serial.h"
#include "string.h"
#include "../include/syscall.h"

#define GATE_INTERRUPT 0x8E /* present, DPL 0, 64-bit interrupt gate */
/* A trap gate leaves IF alone. System calls run with interrupts enabled, which
   matters because SYS_EXIT never returns through iretq - it unwinds straight
   back into the shell, and an interrupt gate would have left interrupts off
   with nothing to turn them back on. */
#define GATE_TRAP 0x8F
#define GATE_TRAP_USER 0xEF   /* the same, callable from ring 3 */

typedef struct __attribute__((packed)) {
    boot_uint16_t offset_low;
    boot_uint16_t selector;
    boot_uint8_t ist;
    boot_uint8_t type_attributes;
    boot_uint16_t offset_middle;
    boot_uint32_t offset_high;
    boot_uint32_t reserved;
} IDT_ENTRY;

typedef struct __attribute__((packed)) {
    boot_uint16_t limit;
    boot_uint64_t base;
} IDT_POINTER;

/* Filled in by isr.S. */
extern void* isr_stub_table[IRQ_BASE + IRQ_COUNT];
extern void syscall_stub(void);

static IDT_ENTRY idt[IDT_ENTRIES];
static IRQ_HANDLER irq_handlers[IRQ_COUNT];

static const char* exception_name(boot_uint64_t vector) {
    static const char* names[32] = {
        "DIVIDE BY ZERO", "DEBUG", "NON MASKABLE INTERRUPT", "BREAKPOINT",
        "OVERFLOW", "BOUND RANGE EXCEEDED", "INVALID OPCODE",
        "DEVICE NOT AVAILABLE", "DOUBLE FAULT", "COPROCESSOR SEGMENT OVERRUN",
        "INVALID TSS", "SEGMENT NOT PRESENT", "STACK SEGMENT FAULT",
        "GENERAL PROTECTION FAULT", "PAGE FAULT", "RESERVED",
        "X87 FLOATING POINT", "ALIGNMENT CHECK", "MACHINE CHECK",
        "SIMD FLOATING POINT", "VIRTUALISATION", "CONTROL PROTECTION",
        "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED",
        "HYPERVISOR INJECTION", "VMM COMMUNICATION", "SECURITY EXCEPTION",
        "RESERVED", "RESERVED"
    };
    return vector < 32 ? names[vector] : "UNKNOWN";
}

static boot_uint64_t read_cr2(void) {
    boot_uint64_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void set_gate_typed(int vector, void* handler, boot_uint8_t ist,
                           boot_uint8_t type) {
    boot_uint64_t address = (boot_uint64_t)(unsigned long long)handler;
    idt[vector].offset_low = (boot_uint16_t)address;
    idt[vector].selector = KERNEL_CODE_SELECTOR;
    idt[vector].ist = ist;
    idt[vector].type_attributes = type;
    idt[vector].offset_middle = (boot_uint16_t)(address >> 16);
    idt[vector].offset_high = (boot_uint32_t)(address >> 32);
    idt[vector].reserved = 0;
}

static void set_gate(int vector, void* handler, boot_uint8_t ist) {
    set_gate_typed(vector, handler, ist, GATE_INTERRUPT);
}

/* Everything the panic screen prints goes to the framebuffer and to COM1.
   A machine that faults before the console is usable still leaves a log. */
static void report(const char* text) {
    console_write(text);
    serial_write(text);
}

static void report_hex(boot_uint64_t value) {
    console_write_hex(value);
    serial_write_hex(value);
}

static void report_register(const char* name, boot_uint64_t value) {
    report(name);
    report(" ");
    report_hex(value);
    report("\n");
}

__attribute__((noreturn)) static void panic_with_frame(const char* message,
                                                       const INTERRUPT_FRAME* frame) {
    console_set_color(COLOR_WHITE, COLOR_RED);
    console_clear();
    report("*** KOI DOS HALTED ***\n\n");
    report(message);
    report("\n\n");

    if (frame) {
        report("VECTOR ");
        report_hex(frame->vector);
        report("  ERROR ");
        report_hex(frame->error_code);
        report("\n\n");
        report_register("RIP", frame->rip);
        report_register("RSP", frame->rsp);
        report_register("RFLAGS", frame->rflags);
        report_register("CS ", frame->cs);
        /* CR2 holds the address that faulted; meaningless for anything but a
           page fault, but harmless to show. */
        report_register("CR2", read_cr2());
        report("\n");
        report_register("RAX", frame->rax);
        report_register("RBX", frame->rbx);
        report_register("RCX", frame->rcx);
        report_register("RDX", frame->rdx);
        report_register("RSI", frame->rsi);
        report_register("RDI", frame->rdi);
        report_register("RBP", frame->rbp);
        /* All sixteen, not the eight that happen to have names from 1978.
           A fault whose cause is in r15 is not diagnosable from a dump that
           stops at rbp - which is exactly how one afternoon went. */
        report_register("R8 ", frame->r8);
        report_register("R9 ", frame->r9);
        report_register("R10", frame->r10);
        report_register("R11", frame->r11);
        report_register("R12", frame->r12);
        report_register("R13", frame->r13);
        report_register("R14", frame->r14);
        report_register("R15", frame->r15);
    }
    cpu_hang();
}

__attribute__((noreturn)) void panic(const char* message) {
    panic_with_frame(message, (const INTERRUPT_FRAME*)0);
}

/* Called from isr_common in isr.S. */
void interrupt_dispatch(INTERRUPT_FRAME* frame);

void interrupt_dispatch(INTERRUPT_FRAME* frame) {
    if (frame->vector < IRQ_BASE) {
        /* A fault at ring 3 is a program's mistake, not the machine's.
         *
         * This is the whole prize of the ring, and the point where "a program
         * crashed" stops meaning "the machine crashed". The frame says where
         * it came from: the low two bits of the code segment are the
         * privilege level it was running at, and 3 means the program. It is
         * ended the way any program ends - the kernel is untouched, the shell
         * comes back, and nothing here has to be unwound, because the stack
         * that faulted was the program's own and is about to be forgotten. */
        if ((frame->cs & 3) == 3 && program_depth()) {
            console_set_color(console_theme()->error,
                              console_theme()->background);
            console_write("\n");
            console_write(exception_name(frame->vector));
            console_write(" in this program. It has been stopped.\n");
            console_use_theme();
            serial_write("PROGRAM: fault at ring 3 - ");
            serial_write(exception_name(frame->vector));
            serial_write(", RIP ");
            serial_write_hex(frame->rip);
            serial_write("\n");
            /* Interrupts back on before leaving.
             *
             * An exception arrives through an interrupt gate, and the
             * processor clears the interrupt flag on the way in - the handler
             * normally returns with iretq, which puts back the flags the
             * frame carries. This does not return: it unwinds to the shell
             * and abandons the frame, so the flag it would have restored is
             * abandoned with it. The machine then runs on with interrupts off
             * for good, which looks exactly like a keyboard that has died -
             * the prompt is drawn, the shell is waiting, and IRQ1 will never
             * arrive again. */
            __asm__ volatile ("sti");
            program_exit(KOI_EXIT_FAULT);
        }
        panic_with_frame(exception_name(frame->vector), frame);
    }
    if (frame->vector < IRQ_BASE + IRQ_COUNT) {
        boot_uint8_t irq = (boot_uint8_t)(frame->vector - IRQ_BASE);
        if (irq_handlers[irq]) irq_handlers[irq](frame);
        /* The acknowledgement goes to whichever controller delivered it.
           Sending it to the 8259 while the APIC is doing the delivering
           leaves the APIC believing the interrupt is still in service, and
           nothing of that priority is ever delivered again. */
        if (apic_available()) apic_end_of_interrupt();
        else pic_send_eoi(irq);
    }
}

void irq_register(boot_uint8_t irq, IRQ_HANDLER handler) {
    if (irq >= IRQ_COUNT) return;
    irq_handlers[irq] = handler;
}

void idt_init(void) {
    IDT_POINTER pointer;

    memset(idt, 0, sizeof(idt));
    for (int vector = 0; vector < IRQ_BASE + IRQ_COUNT; vector++)
        set_gate(vector, isr_stub_table[vector], 0);
    /* Vector 8 gets a stack of its own, so a double fault caused by a broken
       kernel stack can still be reported instead of triple-faulting. */
    set_gate(8, isr_stub_table[8], IST_DOUBLE_FAULT);
    /* The one gate ring 3 is allowed to walk through. DPL 3 on a gate is not
       a permission on the handler - it is who may call it - and a system call
       nobody outside the kernel can make is not a system call. Everything
       else stays at DPL 0, so an application cannot invoke a fault handler by
       hand and hand the kernel a frame it invented. */
    set_gate_typed(SYSCALL_VECTOR, (void*)syscall_stub, 0, GATE_TRAP_USER);

    pointer.limit = (boot_uint16_t)(sizeof(idt) - 1);
    pointer.base = (boot_uint64_t)(unsigned long long)&idt[0];
    __asm__ volatile ("lidt %0" : : "m"(pointer));
}
