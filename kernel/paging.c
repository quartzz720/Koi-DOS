#include "paging.h"
#include "serial.h"
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

/* The memory type of a page is an index into the PAT, assembled from three
 * bits: PAT<<2 | PCD<<1 | PWT. On a 2 MiB page the PAT bit is bit 12 rather
 * than bit 7 - the address field starts at bit 21, so bit 12 is free.
 *
 * The processor powers up with no write-combining type in the table at all,
 * which is the one a framebuffer wants: it lets the processor gather stores
 * into whole cache-line bursts instead of sending each one across the bus on
 * its own. Entry 4 is reprogrammed to provide it. Entries 0-3 are left exactly
 * as they were found, so nothing that does not ask for the new type can
 * notice this happened. */
#define PAGE_LARGE_PAT 0x1000ULL
#define PAT_MSR 0x277

/* PA7..PA0, with PA4 changed from write-back to write-combining. The rest is
   the architectural default: WB, WT, UC-, UC, then those four again. */
#define PAT_VALUE 0x0007040100070406ULL
#define PAT_WRITE_COMBINING PAGE_LARGE_PAT   /* index 4: PAT=1, PCD=0, PWT=0 */

static int write_combining_available;

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

#define PAGE_USER 0x004ULL

/* Hand a range of memory to ring 3, and nothing else with it.
 *
 * The identity map is built out of 2 MiB pages, which is right for a kernel
 * that maps everything and wrong for this: a program's stack does not begin
 * on a 2 MiB boundary, and marking the leaf it sits in would hand the
 * program two megabytes of whatever else is in there. So the leaf is split
 * into four-kilobyte pages first - 512 entries describing exactly what the
 * large one described - and only the pages asked for get the user bit.
 *
 * Every level has to carry the bit as well: the processor takes the strictest
 * of them, so a page marked user under a table that is not is not reachable
 * from ring 3. Marking the tables costs nothing, because a table that is
 * user-accessible still only leads to leaves that are not.
 *
 * There is one address space here, shared by the kernel and whatever runs at
 * ring 3. That is not the finished arrangement - the plan is a space per
 * application - but it already buys the thing that matters: ring 3 can reach
 * what it was given and takes a fault on everything else, including all of
 * the kernel. */
static int split_large_page(boot_uint64_t* pd_entry) {
    boot_uint64_t large = *pd_entry;
    boot_uint64_t base = large & ~(LARGE_PAGE_SIZE - 1) & 0x000FFFFFFFFFF000ULL;
    boot_uint64_t flags = large & (PAGE_WRITABLE | PAGE_WRITE_THROUGH |
                                   PAGE_CACHE_DISABLE);
    boot_uint64_t* table;

    if (!(large & PAGE_LARGE)) return 1;          /* already four-kilobyte */
    table = alloc_table();
    if (!table) return 0;
    for (boot_uint64_t index = 0; index < ENTRIES; index++)
        table[index] = (base + index * PAGE_SIZE) | PAGE_PRESENT | flags;
    *pd_entry = (boot_uint64_t)(unsigned long long)table |
                PAGE_PRESENT | PAGE_WRITABLE;
    return 1;
}

int paging_allow_user(boot_uint64_t base, boot_uint64_t size) {
    boot_uint64_t address = base & ~(PAGE_SIZE - 1);
    boot_uint64_t end = base + size;

    if (!pml4 || end < base) return 0;
    end = (end + PAGE_SIZE - 1) & ~((boot_uint64_t)PAGE_SIZE - 1);

    for (; address < end; address += PAGE_SIZE) {
        boot_uint64_t pml4_index = (address >> 39) & (ENTRIES - 1);
        boot_uint64_t pdpt_index = (address >> 30) & (ENTRIES - 1);
        boot_uint64_t pd_index = (address >> 21) & (ENTRIES - 1);
        boot_uint64_t pt_index = (address >> 12) & (ENTRIES - 1);
        boot_uint64_t* pdpt;
        boot_uint64_t* pd;
        boot_uint64_t* pt;

        if (!(pml4[pml4_index] & PAGE_PRESENT)) return 0;
        pml4[pml4_index] |= PAGE_USER;
        pdpt = (boot_uint64_t*)(unsigned long long)
               (pml4[pml4_index] & 0x000FFFFFFFFFF000ULL);

        if (!(pdpt[pdpt_index] & PAGE_PRESENT)) return 0;
        pdpt[pdpt_index] |= PAGE_USER;
        pd = (boot_uint64_t*)(unsigned long long)
             (pdpt[pdpt_index] & 0x000FFFFFFFFFF000ULL);

        if (!(pd[pd_index] & PAGE_PRESENT)) return 0;
        if (!split_large_page(&pd[pd_index])) return 0;
        pd[pd_index] |= PAGE_USER;
        pt = (boot_uint64_t*)(unsigned long long)
             (pd[pd_index] & 0x000FFFFFFFFFF000ULL);

        pt[pt_index] |= PAGE_USER;
        __asm__ volatile ("invlpg (%0)" : : "r"((void*)(unsigned long long)address)
                          : "memory");
    }

    /* And the whole cache as well, for the tables above the leaf: those are
       shared, and a stale translation through one of them is not addressed by
       invalidating the page it leads to. */
    __asm__ volatile ("mov %%cr3, %%rax\n mov %%rax, %%cr3\n"
                      : : : "rax", "memory");
    return 1;
}

/* ---- An address space of its own ------------------------------------------
 *
 * Ring 3 keeps a program out of the kernel. It does not keep it out of another
 * program, because until now there was one set of tables and everything in it
 * was reachable by anything with the user bit set nearby. A space per program
 * is what makes "its own memory" true rather than nearly true.
 *
 * Building one from nothing would mean mapping the machine again, so a space
 * starts as a copy of the kernel's top level: the same 512 entries, pointing
 * at the same tables underneath. Everything is therefore mapped exactly as the
 * kernel has it - which is what makes an interrupt arriving while a program
 * runs land somewhere sensible - and none of it is user-accessible.
 *
 * A table is copied only when this space needs to differ from the kernel's:
 * marking a page user-accessible clones every table on the way down to it
 * first, so the bit lands in tables nobody else is looking at. That is why one
 * program's slot cannot appear in another's space, and why the kernel's own
 * mappings cannot be changed by any of this.
 *
 * The cost is one page per cloned table - four or five pages for a program,
 * because everything above and beside its own memory stays shared. */
/* One page each, and they are cheap. Sixty-four was enough for a program and
   a screen on the bench, which is exactly the kind of number that is enough
   until somebody has a bigger screen: a 2560x1600 framebuffer alone is eight
   of them, and every megabyte of a program's own memory is another. */
#define SPACE_TABLES_MAX 128

struct PAGING_SPACE {
    boot_uint64_t* pml4;
    void* owned[SPACE_TABLES_MAX];      /* tables this space cloned */
    int owned_count;
};

static PAGING_SPACE spaces[PROGRAM_SPACES_MAX];

static int space_owns(const PAGING_SPACE* space, const void* table) {
    for (int index = 0; index < space->owned_count; index++)
        if (space->owned[index] == table) return 1;
    return 0;
}

static boot_uint64_t* space_take(PAGING_SPACE* space, boot_uint64_t* entry) {
    boot_uint64_t* table = (boot_uint64_t*)(unsigned long long)
                           (*entry & 0x000FFFFFFFFFF000ULL);
    boot_uint64_t* copy;

    if (space_owns(space, table)) return table;
    if (space->owned_count >= SPACE_TABLES_MAX) return (boot_uint64_t*)0;

    copy = alloc_table();
    if (!copy) return (boot_uint64_t*)0;
    for (boot_uint64_t index = 0; index < ENTRIES; index++)
        copy[index] = table[index];

    space->owned[space->owned_count++] = copy;
    *entry = (boot_uint64_t)(unsigned long long)copy |
             (*entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER));
    return copy;
}

PAGING_SPACE* paging_space_create(void) {
    PAGING_SPACE* space = (PAGING_SPACE*)0;

    if (!pml4) return (PAGING_SPACE*)0;
    for (int index = 0; index < PROGRAM_SPACES_MAX; index++)
        if (!spaces[index].pml4) { space = &spaces[index]; break; }
    if (!space) return (PAGING_SPACE*)0;

    space->owned_count = 0;
    space->pml4 = alloc_table();
    if (!space->pml4) return (PAGING_SPACE*)0;
    for (boot_uint64_t index = 0; index < ENTRIES; index++)
        space->pml4[index] = pml4[index];
    space->owned[space->owned_count++] = space->pml4;
    return space;
}

void paging_space_destroy(PAGING_SPACE* space) {
    if (!space || !space->pml4) return;
    /* The tables it cloned, and nothing else: everything shared with the
       kernel is still the kernel's and still in use. */
    for (int index = 0; index < space->owned_count; index++) {
        free_pages(space->owned[index], 1);
        table_bytes -= PAGE_SIZE;
    }
    space->owned_count = 0;
    space->pml4 = (boot_uint64_t*)0;
}

void paging_space_enter(const PAGING_SPACE* space) {
    boot_uint64_t table = (boot_uint64_t)(unsigned long long)
                          (space ? space->pml4 : pml4);
    if (!table) return;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(table) : "memory");
}

int paging_space_allow_user(PAGING_SPACE* space, boot_uint64_t base,
                            boot_uint64_t size) {
    boot_uint64_t address = base & ~(PAGE_SIZE - 1);
    boot_uint64_t end = base + size;

    if (!space || !space->pml4 || end < base) return 0;
    end = (end + PAGE_SIZE - 1) & ~((boot_uint64_t)PAGE_SIZE - 1);

    for (; address < end; address += PAGE_SIZE) {
        boot_uint64_t pml4_index = (address >> 39) & (ENTRIES - 1);
        boot_uint64_t pdpt_index = (address >> 30) & (ENTRIES - 1);
        boot_uint64_t pd_index = (address >> 21) & (ENTRIES - 1);
        boot_uint64_t pt_index = (address >> 12) & (ENTRIES - 1);
        boot_uint64_t* pdpt;
        boot_uint64_t* pd;
        boot_uint64_t* pt;

        if (!(space->pml4[pml4_index] & PAGE_PRESENT)) return 0;
        pdpt = space_take(space, &space->pml4[pml4_index]);
        if (!pdpt) return 0;
        space->pml4[pml4_index] |= PAGE_USER;

        if (!(pdpt[pdpt_index] & PAGE_PRESENT)) return 0;
        pd = space_take(space, &pdpt[pdpt_index]);
        if (!pd) return 0;
        pdpt[pdpt_index] |= PAGE_USER;

        if (!(pd[pd_index] & PAGE_PRESENT)) return 0;
        if (pd[pd_index] & PAGE_LARGE) {
            /* A leaf, and splitting it makes a table that is this space's from
               the moment it exists: the entry pointing at it is in a table
               this space already owns.
             *
             * The claim has to be conditional on the split, and that is not a
             * detail. Claiming whatever the entry points at would claim a
             * table shared with the kernel whenever the leaf had already been
             * split by somebody else - and then this space would write user
             * bits into the kernel's own table and free it on the way out. */
            if (!split_large_page(&pd[pd_index])) return 0;
            if (space->owned_count >= SPACE_TABLES_MAX) return 0;
            space->owned[space->owned_count++] =
                (void*)(unsigned long long)(pd[pd_index] & 0x000FFFFFFFFFF000ULL);
        }
        pt = space_take(space, &pd[pd_index]);
        if (!pt) return 0;
        pd[pd_index] |= PAGE_USER;

        pt[pt_index] |= PAGE_USER;
        /* And tell the processor to look again.
         *
         * A translation it has already made is cached, and the cache holds
         * what the tables said at the time - which for these pages is "the
         * kernel only". The kernel touched them a moment ago on purpose:
         * graphics_enter clears the buffer before handing it over, and that
         * clearing is what puts the old answer in the cache. The tables then
         * say user-accessible and the processor goes on refusing, because it
         * never re-reads them.
         *
         * It cost a laptop and a photograph to find, and nothing at all in
         * QEMU, which throws its cached translations away far more eagerly
         * than silicon does. One instruction per page, at the moment the page
         * changes meaning. */
        __asm__ volatile ("invlpg (%0)" : : "r"((void*)(unsigned long long)address)
                          : "memory");
    }
    return 1;
}

/* Put a write-combining type into the PAT, if this processor has one to put it
 * in. Every x86-64 part does; the check is here because acting on an absent
 * feature would set an MSR that does not exist and take a #GP doing it.
 *
 * Returns the page flag that selects it, or 0 when it could not be arranged -
 * in which case the caller falls back to write-through, which is correct and
 * merely slow. */
static boot_uint64_t enable_write_combining(void) {
    boot_uint32_t a;
    boot_uint32_t b;
    boot_uint32_t c;
    boot_uint32_t d;

    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(d & (1U << 16))) return 0;     /* no PAT on this processor */

    __asm__ volatile ("wrmsr"
                      : : "c"((boot_uint32_t)PAT_MSR),
                          "a"((boot_uint32_t)(PAT_VALUE & 0xFFFFFFFFU)),
                          "d"((boot_uint32_t)(PAT_VALUE >> 32)));
    write_combining_available = 1;
    return PAT_WRITE_COMBINING;
}

int paging_write_combining(void) {
    return write_combining_available;
}

int paging_init(const BOOT_INFO* info) {
    boot_uint64_t framebuffer_flags;

    mapped_bytes = 0;
    table_bytes = 0;
    write_combining_available = 0;
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

    /* The framebuffer is device memory: writes have to reach the adapter
     * rather than sit in a cache. Write-through does that and was what this
     * used before, but it sends every store across the bus on its own, and a
     * full screen is a million of them - which measured at nearly eight
     * milliseconds a frame, half the budget of anything that animates.
     *
     * Write-combining reaches the adapter too, and lets the processor gather
     * stores into whole cache-line bursts first. Falling back to write-through
     * when the PAT cannot be arranged keeps the old behaviour rather than
     * risking a screen that shows stale pixels. */
    framebuffer_flags = enable_write_combining();
    if (!framebuffer_flags) framebuffer_flags = PAGE_WRITE_THROUGH;
    if (info->framebuffer_base && info->framebuffer_size) {
        if (!map_range(info->framebuffer_base, info->framebuffer_size,
                       framebuffer_flags))
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

/* Whether a range is already reachable from ring 3 in this space.
 *
 * The question a thread's stack raises: the program says "run on this
 * memory", and the kernel has to know whether the program could touch that
 * memory itself. If it could, running a thread on it grants nothing new; if
 * it could not, the kernel would be handing ring 3 a page it was never
 * given - which is the whole of what the check is for.
 *
 * Asked of the page tables rather than of an address range, because "inside
 * the program's slot" stopped being the same question the moment a program
 * could allocate memory: what koi_alloc hands back is a kernel page marked
 * reachable, and it lives nowhere near the slot. */
static int range_is_user(boot_uint64_t* tables, boot_uint64_t base,
                         boot_uint64_t size) {
    boot_uint64_t address = base & ~((boot_uint64_t)PAGE_SIZE - 1);
    boot_uint64_t end = base + size;

    if (!tables || end < base) return 0;
    end = (end + PAGE_SIZE - 1) & ~((boot_uint64_t)PAGE_SIZE - 1);

    for (; address < end; address += PAGE_SIZE) {
        boot_uint64_t pml4_index = (address >> 39) & (ENTRIES - 1);
        boot_uint64_t pdpt_index = (address >> 30) & (ENTRIES - 1);
        boot_uint64_t pd_index = (address >> 21) & (ENTRIES - 1);
        boot_uint64_t pt_index = (address >> 12) & (ENTRIES - 1);
        boot_uint64_t* pdpt;
        boot_uint64_t* pd;
        boot_uint64_t* pt;

        if (!(tables[pml4_index] & PAGE_PRESENT)) return 0;
        if (!(tables[pml4_index] & PAGE_USER)) return 0;
        /* The address out of an entry, and only the address: the high bits
           carry flags - the topmost says "never execute this" - and masking
           with ~0xFFF leaves them in place, which turns a table pointer into
           a number no memory has. The rest of this file uses this mask; the
           first version of this function did not, and every check failed. */
        pdpt = (boot_uint64_t*)(unsigned long long)
               (tables[pml4_index] & 0x000FFFFFFFFFF000ULL);
        if (!(pdpt[pdpt_index] & PAGE_PRESENT)) return 0;
        if (!(pdpt[pdpt_index] & PAGE_USER)) return 0;
        pd = (boot_uint64_t*)(unsigned long long)
             (pdpt[pdpt_index] & 0x000FFFFFFFFFF000ULL);
        if (!(pd[pd_index] & PAGE_PRESENT)) return 0;
        if (!(pd[pd_index] & PAGE_USER)) return 0;
        if (pd[pd_index] & PAGE_LARGE) continue;      /* a whole 2 MiB, allowed */
        pt = (boot_uint64_t*)(unsigned long long)
             (pd[pd_index] & 0x000FFFFFFFFFF000ULL);
        if (!(pt[pt_index] & PAGE_PRESENT)) return 0;
        if (!(pt[pt_index] & PAGE_USER)) return 0;
    }
    return 1;
}

int paging_is_user(boot_uint64_t base, boot_uint64_t size) {
    return range_is_user(pml4, base, size);
}

int paging_space_is_user(PAGING_SPACE* space, boot_uint64_t base,
                         boot_uint64_t size) {
    if (!space) return paging_is_user(base, size);
    return range_is_user(space->pml4, base, size);
}
