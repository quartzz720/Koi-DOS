#include "cpu.h"
#include "memory.h"
#include "string.h"

/* A 64-bit GDT is almost ceremonial: in long mode the base and limit of code
   and data segments are ignored. What matters is that the descriptors exist,
   are ours, and that the code descriptor has the L bit set. */

/* null, kernel code, kernel data, user code, user data, and two slots for the
   16-byte TSS descriptor. */
#define GDT_ENTRIES 7

#define DOUBLE_FAULT_STACK_PAGES 4

typedef struct __attribute__((packed)) {
    boot_uint16_t limit_low;
    boot_uint16_t base_low;
    boot_uint8_t base_middle;
    boot_uint8_t access;
    boot_uint8_t limit_high_and_flags;
    boot_uint8_t base_high;
} GDT_ENTRY;

typedef struct __attribute__((packed)) {
    boot_uint16_t limit;
    boot_uint64_t base;
} GDT_POINTER;

/* A system descriptor is twice the width of a segment descriptor: long mode
   widened the base to 64 bits and it does not fit in eight bytes. */
typedef struct __attribute__((packed)) {
    boot_uint16_t limit_low;
    boot_uint16_t base_low;
    boot_uint8_t base_middle;
    boot_uint8_t access;
    boot_uint8_t limit_high_and_flags;
    boot_uint8_t base_high;
    boot_uint32_t base_upper;
    boot_uint32_t reserved;
} TSS_DESCRIPTOR;

typedef struct __attribute__((packed)) {
    boot_uint32_t reserved0;
    boot_uint64_t rsp[3];      /* stacks for rings 0-2; unused, we stay in 0 */
    boot_uint64_t reserved1;
    boot_uint64_t ist[7];      /* ist[0] is IST1, the numbering starts at one */
    boot_uint64_t reserved2;
    boot_uint16_t reserved3;
    boot_uint16_t io_map_base;
} TASK_STATE_SEGMENT;

static TASK_STATE_SEGMENT tss;

static GDT_ENTRY gdt[GDT_ENTRIES];

static void set_entry(int index, boot_uint8_t access, boot_uint8_t flags) {
    gdt[index].limit_low = 0xFFFF;
    gdt[index].base_low = 0;
    gdt[index].base_middle = 0;
    gdt[index].access = access;
    gdt[index].limit_high_and_flags = (boot_uint8_t)(0x0F | (flags << 4));
    gdt[index].base_high = 0;
}

void gdt_init(void) {
    GDT_POINTER pointer;

    /* 0x00 null, 0x08 ring-0 code (present, executable, readable, L=1),
       0x10 ring-0 data (present, writable). */
    gdt[0] = (GDT_ENTRY){ 0, 0, 0, 0, 0, 0 };
    set_entry(1, 0x9A, 0x0A); /* granularity + long mode */
    set_entry(2, 0x92, 0x0C); /* granularity + 32-bit default size */
    /* The same two again at DPL 3. In long mode a segment carries almost
       nothing but its privilege level, and that is exactly what these are
       for: the processor refuses to run code at ring 3 through a descriptor
       that does not say ring 3. */
    set_entry(3, 0xFA, 0x0A); /* user code: present, DPL 3, executable, L=1 */
    set_entry(4, 0xF2, 0x0C); /* user data: present, DPL 3, writable */

    pointer.limit = (boot_uint16_t)(sizeof(gdt) - 1);
    pointer.base = (boot_uint64_t)(unsigned long long)&gdt[0];

    /* CS cannot be loaded with a plain mov. The far return pops a new RIP and
       a new CS together, which is the supported way to switch it in long
       mode. */
    __asm__ volatile (
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov %w2, %%ds\n"
        "mov %w2, %%es\n"
        "mov %w2, %%fs\n"
        "mov %w2, %%gs\n"
        "mov %w2, %%ss\n"
        :
        : "m"(pointer), "i"((boot_uint64_t)KERNEL_CODE_SELECTOR),
          "r"((boot_uint16_t)KERNEL_DATA_SELECTOR)
        : "rax", "memory");
}

void tss_init(void) {
    TSS_DESCRIPTOR* descriptor;
    boot_uint64_t base = (boot_uint64_t)(unsigned long long)&tss;
    void* stack = alloc_pages(DOUBLE_FAULT_STACK_PAGES);

    memset(&tss, 0, sizeof(tss));
    /* No I/O permission bitmap: a limit that stops at the end of the TSS is
       how the manual says to express "none". */
    tss.io_map_base = (boot_uint16_t)sizeof(tss);

    if (stack) {
        /* Stacks grow down, so IST holds the top. */
        tss.ist[IST_DOUBLE_FAULT - 1] =
            (boot_uint64_t)(unsigned long long)stack +
            DOUBLE_FAULT_STACK_PAGES * PAGE_SIZE;
    }

    descriptor = (TSS_DESCRIPTOR*)&gdt[5];
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->limit_low = (boot_uint16_t)(sizeof(tss) - 1);
    descriptor->base_low = (boot_uint16_t)base;
    descriptor->base_middle = (boot_uint8_t)(base >> 16);
    descriptor->access = 0x89;   /* present, 64-bit available TSS */
    descriptor->base_high = (boot_uint8_t)(base >> 24);
    descriptor->base_upper = (boot_uint32_t)(base >> 32);

    /* And a stack for interrupts that arrive from ring 3. Without it the
       processor would keep using whatever stack the application had, which is
       memory the application chose and may have made unusable - the fault
       handler would then fault, and that is a machine that reboots rather
       than a program that stops. */
    {
        void* interrupt_stack = alloc_pages(KERNEL_INTERRUPT_STACK_PAGES);

        if (interrupt_stack)
            tss.rsp[0] = (boot_uint64_t)(unsigned long long)interrupt_stack +
                         KERNEL_INTERRUPT_STACK_PAGES * PAGE_SIZE;
    }

    __asm__ volatile ("ltr %w0" : : "r"((boot_uint16_t)TSS_SELECTOR) : "memory");
}

void cpu_set_kernel_stack(boot_uint64_t top) {
    tss.rsp[0] = top;
}

/* Into ring 3, by pretending to return from an interrupt that never happened.
 *
 * iretq pops RIP, CS, RFLAGS, RSP and SS in one go, and it is the only
 * instruction that will load a code segment of a lower privilege level. So the
 * way down is to build the frame the processor would have pushed on the way up
 * and then return from it. Interrupts are enabled in the flags it pops rather
 * than beforehand, because between here and there the stack belongs to nobody
 * in particular. */
/* The same, with a value for the function's first argument.
 *
 * A thread is entered at a function the program chose, and that function takes
 * something - which window, which connection, what to fetch. The value goes in
 * the register the calling convention uses for a first parameter, so the
 * thread reads it as an ordinary argument and nothing in the program has to
 * know it arrived from the kernel. */
__attribute__((noreturn)) void cpu_enter_user_with(boot_uint64_t entry,
                                                   boot_uint64_t stack,
                                                   boot_uint64_t argument) {
    stack -= 8;
    __asm__ volatile (
        "mov %w2, %%ds\n"
        "mov %w2, %%es\n"
        "mov %w2, %%fs\n"
        "mov %w2, %%gs\n"
        "pushq %3\n"           /* SS  */
        "pushq %1\n"           /* RSP */
        "pushq $0x202\n"       /* RFLAGS: reserved bit, interrupts enabled */
        "pushq %4\n"           /* CS  */
        "pushq %0\n"           /* RIP */
        "iretq\n"
        :
        : "r"(entry), "r"(stack),
          "r"((boot_uint16_t)USER_DATA_SELECTOR),
          "i"((boot_uint64_t)USER_DATA_SELECTOR),
          "i"((boot_uint64_t)USER_CODE_SELECTOR),
          "D"(argument)
        : "memory");
    __builtin_unreachable();
}

__attribute__((noreturn)) void cpu_enter_user(boot_uint64_t entry,
                                              boot_uint64_t stack) {
    /* Eight bytes down, so the stack looks the way a call would have left it.
     *
     * The x86-64 convention is that at the first instruction of a function
     * the stack pointer is eight less than a multiple of sixteen - because a
     * call has just pushed a return address onto an aligned stack. A program
     * entered at ring 0 arrives through `call` and gets that for free. This
     * one arrives through iretq, which pushes nothing, so it used to start
     * eight bytes out.
     *
     * Nothing noticed while programs had no floating point: the compiler only
     * assumes the alignment when it emits an instruction that requires it,
     * and every one of those is an SSE instruction. The first program built
     * with SSE enabled stopped with a general protection fault, which is what
     * `movaps` does to an address ending in 8. */
    stack -= 8;
    __asm__ volatile (
        "mov %w2, %%ds\n"
        "mov %w2, %%es\n"
        "mov %w2, %%fs\n"
        "mov %w2, %%gs\n"
        "pushq %3\n"           /* SS  */
        "pushq %1\n"           /* RSP */
        "pushq $0x202\n"       /* RFLAGS: reserved bit, interrupts enabled */
        "pushq %4\n"           /* CS  */
        "pushq %0\n"           /* RIP */
        "iretq\n"
        :
        : "r"(entry), "r"(stack),
          "r"((boot_uint16_t)USER_DATA_SELECTOR),
          "i"((boot_uint64_t)USER_DATA_SELECTOR),
          "i"((boot_uint64_t)USER_CODE_SELECTOR)
        : "memory");
    __builtin_unreachable();
}

/* Floating point, which this machine did not have.
 *
 * Not because the processor lacks it - every x86-64 has SSE2 and the compiler
 * would use it by default - but because nothing had set it up. After
 * ExitBootServices the FPU and SSE state is whatever the firmware left, and
 * an instruction touching it either faults or works on somebody else's
 * numbers. So the kernel and every program were built -mgeneral-regs-only,
 * and the machine could not add two fractions.
 *
 * That was the right trade while nothing needed them. It stops being right at
 * the first thing that does: an MP3 decoder, a JPEG, anything that scales a
 * picture properly. Four bits in two registers is the whole cost.
 *
 *   CR0.EM off   - do not trap SSE as though it were absent
 *   CR0.MP on    - the FPU is present and WAIT should behave
 *   CR4.OSFXSR   - the operating system uses FXSAVE/FXRSTOR, so SSE is legal
 *   CR4.OSXMMEXCPT - and it handles SIMD exceptions rather than #UD
 *
 * The kernel itself stays -mgeneral-regs-only. It has no arithmetic that
 * wants this, and a kernel that never touches SSE is a kernel that cannot
 * corrupt a program's - which makes a system call free of any save at all.
 * Only the scheduler has to care, and only between programs. */
void cpu_enable_sse(void) {
    boot_uint64_t cr0;
    boot_uint64_t cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);            /* EM */
    cr0 |= (1ULL << 1);             /* MP */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);   /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    /* And a known state to start from, rather than the firmware's. */
    __asm__ volatile ("fninit");
}

/* The cache line size, from CPUID leaf 1. Sixty-four everywhere that matters,
   but read rather than assumed: a stride larger than the real line would skip
   lines, and skipping one is the whole bug this exists to avoid. */
static boot_uint32_t cache_line_size(void) {
    static boot_uint32_t cached;
    boot_uint32_t a, b, c, d;

    if (cached) return cached;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                              : "a"(1), "c"(0));
    cached = ((b >> 8) & 0xFF) * 8;
    if (cached < 16 || cached > 256) cached = 64;
    return cached;
}

void cpu_flush_cache(const void* address, boot_uint64_t bytes) {
    boot_uint64_t line = cache_line_size();
    boot_uint64_t start = (boot_uint64_t)(unsigned long long)address;
    boot_uint64_t end = start + bytes;

    if (!bytes) return;
    start &= ~(line - 1);

    /* Fenced on both sides: the writes have to be in the cache before they can
       be flushed out of it, and the flushes have to have completed before
       whatever tells the device to go and look. */
    __asm__ volatile ("mfence" : : : "memory");
    for (; start < end; start += line)
        __asm__ volatile ("clflush (%0)" : : "r"((const void*)(unsigned long long)start) : "memory");
    __asm__ volatile ("mfence" : : : "memory");
}
