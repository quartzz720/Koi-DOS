#include "paging.h"
#include "memory.h"
#include "string.h"

/* Identity map built from 2 MiB pages.
 *
 * 2 MiB leaves rather than 4 KiB ones keep the tables small - one PD entry per
 * 2 MiB means a 4 GiB map costs four page directories instead of two thousand
 * page tables - and they hit in the TLB far more often. Nothing in a DOS-like
 * system needs finer granularity than that. */

#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_LARGE 0x080ULL
#define PAGE_WRITE_THROUGH 0x008ULL
#define PAGE_CACHE_DISABLE 0x010ULL

#define ENTRIES 512ULL
#define LARGE_PAGE_SIZE (2ULL * 1024ULL * 1024ULL)
#define GIGABYTE (1024ULL * 1024ULL * 1024ULL)

static boot_uint64_t* pml4;
static boot_uint64_t mapped_bytes;
static boot_uint64_t table_bytes;

static boot_uint64_t* alloc_table(void) {
    boot_uint64_t* table = (boot_uint64_t*)alloc_page();
    if (table) {
        memset(table, 0, PAGE_SIZE);
        table_bytes += PAGE_SIZE;
    }
    return table;
}

/* Map [start, start + size) as 2 MiB pages with the given extra flags. */
static int map_range(boot_uint64_t start, boot_uint64_t size, boot_uint64_t flags) {
    boot_uint64_t address = start & ~(LARGE_PAGE_SIZE - 1);
    boot_uint64_t end = start + size;

    if (end < start) return 0;
    end = (end + LARGE_PAGE_SIZE - 1) & ~(LARGE_PAGE_SIZE - 1);

    for (; address < end; address += LARGE_PAGE_SIZE) {
        boot_uint64_t pml4_index = (address >> 39) & (ENTRIES - 1);
        boot_uint64_t pdpt_index = (address >> 30) & (ENTRIES - 1);
        boot_uint64_t pd_index = (address >> 21) & (ENTRIES - 1);
        boot_uint64_t* pdpt;
        boot_uint64_t* pd;

        if (!(pml4[pml4_index] & PAGE_PRESENT)) {
            boot_uint64_t* table = alloc_table();
            if (!table) return 0;
            pml4[pml4_index] = (boot_uint64_t)(unsigned long long)table |
                               PAGE_PRESENT | PAGE_WRITABLE;
        }
        pdpt = (boot_uint64_t*)(unsigned long long)(pml4[pml4_index] & ~0xFFFULL);

        if (!(pdpt[pdpt_index] & PAGE_PRESENT)) {
            boot_uint64_t* table = alloc_table();
            if (!table) return 0;
            pdpt[pdpt_index] = (boot_uint64_t)(unsigned long long)table |
                               PAGE_PRESENT | PAGE_WRITABLE;
        }
        pd = (boot_uint64_t*)(unsigned long long)(pdpt[pdpt_index] & ~0xFFFULL);

        /* Count only entries not already present, so overlapping ranges
           are not tallied twice. */
        if (!(pd[pd_index] & PAGE_PRESENT)) mapped_bytes += LARGE_PAGE_SIZE;
        pd[pd_index] = address | PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE | flags;
    }
    return 1;
}

int paging_init(const BOOT_INFO* info) {
    mapped_bytes = 0;
    table_bytes = 0;
    pml4 = alloc_table();
    if (!pml4) return 0;

    /* The whole first 4 GiB unconditionally: PCI device windows, the local
       APIC and the HPET all sit in holes below 4 GiB that the memory map does
       not describe at all. */
    if (!map_range(0, 4ULL * GIGABYTE, 0)) return 0;

    /* Then each descriptor on its own. Mapping from zero up to the highest
       descriptor instead would swallow the enormous gap between the top of RAM
       and the 64-bit PCI window - a terabyte of it on QEMU's q35 - and a
       pointer strayed into that gap would silently succeed rather than fault.
       Every type is mapped, not just the usable ones: ACPI tables, runtime
       services and reclaimable regions all still have to be readable. */
    for (boot_uint64_t offset = 0;
         offset + info->memory_map_descriptor_size <= info->memory_map_size;
         offset += info->memory_map_descriptor_size) {
        const BOOT_MEMORY_DESCRIPTOR* descriptor =
            (const BOOT_MEMORY_DESCRIPTOR*)(info->memory_map + offset);
        if (!descriptor->page_count) continue;
        if (!map_range(descriptor->physical_start,
                       descriptor->page_count * PAGE_SIZE, 0))
            return 0;
    }

    /* The framebuffer is device memory. Caching writes to it would leave the
       screen showing whatever was in the cache; mark it write-through so the
       pixels actually reach the adapter. */
    if (info->framebuffer_base && info->framebuffer_size) {
        if (!map_range(info->framebuffer_base, info->framebuffer_size,
                       PAGE_WRITE_THROUGH))
            return 0;
    }

    __asm__ volatile ("mov %0, %%cr3"
                      : : "r"((boot_uint64_t)(unsigned long long)pml4)
                      : "memory");
    return 1;
}

int paging_map_device(boot_uint64_t base, boot_uint64_t size) {
    if (!pml4) return 0;      /* still on the firmware's tables; nothing to do */
    if (!size) return 0;
    if (!map_range(base, size, PAGE_CACHE_DISABLE)) return 0;

    /* Reload CR3 to drop anything the TLB cached about these addresses. A
       not-present entry can be remembered as such on some processors, and an
       MMIO window that faults once would keep faulting. */
    {
        boot_uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
    return 1;
}

boot_uint64_t paging_mapped_bytes(void) {
    return mapped_bytes;
}

boot_uint64_t paging_table_bytes(void) {
    return table_bytes;
}
