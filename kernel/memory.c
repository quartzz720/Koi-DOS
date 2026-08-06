#include "memory.h"
#include "program.h"

#define PAGE_BITMAP_SIZE (2ULL * 1024ULL * 1024ULL)
#define PAGE_BITMAP_PAGES (PAGE_BITMAP_SIZE * 8ULL)
#define PAGE_BITMAP_LIMIT (PAGE_BITMAP_PAGES * PAGE_SIZE)

/* Highest page a 32-bit device address can reach. */
#define LOW_MEMORY_PAGES (4ULL * 1024ULL * 1024ULL * 1024ULL / PAGE_SIZE)

static boot_uint8_t page_bitmap[PAGE_BITMAP_SIZE];
static boot_uint64_t page_search_hint;
static boot_uint64_t physical_pages;
static boot_uint64_t kernel_bytes;

/* Does this descriptor describe RAM the machine has? See bootinfo.h for why
   this is an allowlist rather than "everything that is not a device window". */
static int describes_memory(boot_uint32_t type) {
    switch (type) {
    case BOOT_MEMORY_LOADER_CODE:
    case BOOT_MEMORY_LOADER_DATA:
    case BOOT_MEMORY_BOOT_SERVICES_CODE:
    case BOOT_MEMORY_BOOT_SERVICES_DATA:
    case BOOT_MEMORY_RUNTIME_CODE:
    case BOOT_MEMORY_RUNTIME_DATA:
    case BOOT_MEMORY_USABLE:
    case BOOT_MEMORY_ACPI_RECLAIM:
    case BOOT_MEMORY_ACPI_NVS:
    case BOOT_MEMORY_PERSISTENT:
        return 1;
    default:
        return 0;
    }
}

static void bitmap_set(boot_uint64_t page) {
    page_bitmap[page >> 3] |= (boot_uint8_t)(1U << (page & 7));
}

static void bitmap_clear(boot_uint64_t page) {
    page_bitmap[page >> 3] &= (boot_uint8_t)~(1U << (page & 7));
}

static int bitmap_test(boot_uint64_t page) {
    return (page_bitmap[page >> 3] & (boot_uint8_t)(1U << (page & 7))) != 0;
}

static void reserve_range(boot_uint64_t start, boot_uint64_t size) {
    boot_uint64_t end;
    boot_uint64_t first;
    boot_uint64_t last;

    if (!size || start >= PAGE_BITMAP_LIMIT) return;
    end = start + size;
    if (end < start || end > PAGE_BITMAP_LIMIT) end = PAGE_BITMAP_LIMIT;
    first = start / PAGE_SIZE;
    last = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (boot_uint64_t page = first; page < last; page++) bitmap_set(page);
}

static void release_conventional_range(boot_uint64_t start, boot_uint64_t count) {
    boot_uint64_t end;
    boot_uint64_t first;
    boot_uint64_t last;

    if (!count || start >= PAGE_BITMAP_LIMIT) return;
    if (count > (boot_uint64_t)-1 / PAGE_SIZE) end = PAGE_BITMAP_LIMIT;
    else end = start + count * PAGE_SIZE;
    if (end < start || end > PAGE_BITMAP_LIMIT) end = PAGE_BITMAP_LIMIT;
    first = start / PAGE_SIZE;
    last = end / PAGE_SIZE;
    for (boot_uint64_t page = first; page < last; page++) bitmap_clear(page);
}

void memory_init(BOOT_INFO* info) {
    for (boot_uint64_t i = 0; i < PAGE_BITMAP_SIZE; i++) page_bitmap[i] = 0xFF;
    physical_pages = 0;

    for (boot_uint64_t offset = 0;
         offset + info->memory_map_descriptor_size <= info->memory_map_size;
         offset += info->memory_map_descriptor_size) {
        BOOT_MEMORY_DESCRIPTOR* descriptor =
            (BOOT_MEMORY_DESCRIPTOR*)(info->memory_map + offset);
        /* Runtime-services and ACPI regions count towards the machine's
           memory: they are RAM the board has, simply RAM we are not allowed
           to hand out. */
        if (describes_memory(descriptor->type))
            physical_pages += descriptor->page_count;
        if (descriptor->type == BOOT_MEMORY_USABLE)
            release_conventional_range(descriptor->physical_start,
                                       descriptor->page_count);
    }
    kernel_bytes = info->kernel_image_size;

    reserve_range(0, PAGE_SIZE);
    /* The image range covers .bss, so the bitmap below - which lives in .bss -
       is reserved as part of it. No separate reserve for it is needed. */
    reserve_range(info->kernel_image_base, info->kernel_image_size);
    reserve_range(info->kernel_stack_base, info->kernel_stack_size);
    reserve_range(info->framebuffer_base, info->framebuffer_size);
    reserve_range(info->memory_map, info->memory_map_size);
    /* Programs are linked -no-pie at a fixed address, so the allocator must
       never hand out the window they load into. */
    reserve_range(PROGRAM_BASE, PROGRAM_LIMIT - PROGRAM_BASE);
    page_search_hint = 1;
}

/* Claim the first free page in [first, last). */
static void* claim_page_in_range(boot_uint64_t first, boot_uint64_t last) {
    for (boot_uint64_t page = first; page < last; page++) {
        if (!bitmap_test(page)) {
            bitmap_set(page);
            return (void*)(unsigned long long)(page * PAGE_SIZE);
        }
    }
    return (void*)0;
}

void* alloc_page(void) {
    void* page = claim_page_in_range(page_search_hint, PAGE_BITMAP_PAGES);
    if (!page) page = claim_page_in_range(1, page_search_hint);
    if (page) page_search_hint =
        (boot_uint64_t)(unsigned long long)page / PAGE_SIZE + 1;
    return page;
}

void* alloc_page_low(void) {
    /* Search upward from page 1 rather than from the hint: low memory is a
       scarce resource that only devices need, so it should stay packed at the
       bottom and not be fragmented by the general allocator's position. */
    return claim_page_in_range(1, LOW_MEMORY_PAGES);
}

void* alloc_pages(boot_uint64_t count) {
    boot_uint64_t page = 1;

    if (!count) return (void*)0;
    if (count == 1) return alloc_page();

    while (page + count <= PAGE_BITMAP_PAGES) {
        boot_uint64_t offset = 0;
        while (offset < count && !bitmap_test(page + offset)) offset++;
        if (offset == count) {
            for (boot_uint64_t i = 0; i < count; i++) bitmap_set(page + i);
            return (void*)(unsigned long long)(page * PAGE_SIZE);
        }
        /* Skip past the page that broke the run - every start position in
           between would fail on the same page. */
        page += offset + 1;
    }
    return (void*)0;
}

void free_pages(void* address, boot_uint64_t count) {
    boot_uint64_t physical = (boot_uint64_t)(unsigned long long)address;
    boot_uint64_t first;

    if (!address || (physical & (PAGE_SIZE - 1)) != 0) return;
    first = physical / PAGE_SIZE;
    if (!first || first + count > PAGE_BITMAP_PAGES) return;
    for (boot_uint64_t i = 0; i < count; i++) bitmap_clear(first + i);
    if (first < page_search_hint) page_search_hint = first;
}

boot_uint64_t memory_physical_pages(void) {
    return physical_pages;
}

boot_uint64_t memory_kernel_bytes(void) {
    return kernel_bytes;
}

boot_uint64_t memory_free_pages(void) {
    boot_uint64_t free = 0;
    /* Whole-byte scan: the bitmap covers 16 million pages and is almost
       entirely 0xFF on a machine with modest RAM, so skipping full bytes
       turns this from a visible pause into nothing. */
    for (boot_uint64_t index = 0; index < PAGE_BITMAP_SIZE; index++) {
        boot_uint8_t byte = page_bitmap[index];
        if (byte == 0xFF) continue;
        for (unsigned bit = 0; bit < 8; bit++)
            if (!(byte & (1U << bit))) free++;
    }
    return free;
}

void free_page(void* address) {
    boot_uint64_t physical = (boot_uint64_t)(unsigned long long)address;
    boot_uint64_t page;

    if (!address || physical >= PAGE_BITMAP_LIMIT ||
        (physical & (PAGE_SIZE - 1)) != 0) return;
    page = physical / PAGE_SIZE;
    if (page == 0) return;
    bitmap_clear(page);
    if (page < page_search_hint) page_search_hint = page;
}

int memory_self_test(void) {
    void* page = alloc_page();
    if (!page) return 0;
    free_page(page);
    return 1;
}
