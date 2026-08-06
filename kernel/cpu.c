#include "cpu.h"
#include "memory.h"
#include "string.h"

/* A 64-bit GDT is almost ceremonial: in long mode the base and limit of code
   and data segments are ignored. What matters is that the descriptors exist,
   are ours, and that the code descriptor has the L bit set. */

/* null, code, data, and two slots for the 16-byte TSS descriptor. */
#define GDT_ENTRIES 5

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

    descriptor = (TSS_DESCRIPTOR*)&gdt[3];
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->limit_low = (boot_uint16_t)(sizeof(tss) - 1);
    descriptor->base_low = (boot_uint16_t)base;
    descriptor->base_middle = (boot_uint8_t)(base >> 16);
    descriptor->access = 0x89;   /* present, 64-bit available TSS */
    descriptor->base_high = (boot_uint8_t)(base >> 24);
    descriptor->base_upper = (boot_uint32_t)(base >> 32);

    __asm__ volatile ("ltr %w0" : : "r"((boot_uint16_t)TSS_SELECTOR) : "memory");
}
