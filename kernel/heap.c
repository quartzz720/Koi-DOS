#include "heap.h"
#include "memory.h"
#include "string.h"
#include "idt.h"

/* First-fit free list with splitting and forward coalescing.
 *
 * Memory comes from the page allocator in arenas. Blocks within one arena form
 * an address-ordered list that covers it completely, so two adjacent free
 * blocks in the same arena are always physically adjacent and can be merged.
 * Blocks are never merged across arenas - separate alloc_pages() calls carry
 * no promise of adjacency. */

#define ALIGNMENT 16ULL
#define ARENA_MIN_PAGES 256ULL     /* 1 MiB */
#define BLOCK_MAGIC 0x4B4F49484541ULL /* "KOIHEA" */

typedef struct BLOCK {
    boot_uint64_t magic;
    boot_uint64_t size;        /* payload bytes, not counting this header */
    struct BLOCK* next;        /* next block in the same arena, ascending */
    boot_uint64_t free;
} BLOCK;

typedef struct ARENA {
    struct ARENA* next;
    boot_uint64_t pages;
    BLOCK* first;
} ARENA;

static ARENA* arenas;
static boot_uint64_t total_bytes;
static boot_uint64_t used_bytes;

static boot_uint64_t align_up(boot_uint64_t value) {
    return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

/* Claim `pages` from the page allocator and lay an arena over it. */
static ARENA* grow(boot_uint64_t payload_needed) {
    boot_uint64_t bytes = sizeof(ARENA) + sizeof(BLOCK) + payload_needed;
    boot_uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    ARENA* arena;
    BLOCK* block;

    if (pages < ARENA_MIN_PAGES) pages = ARENA_MIN_PAGES;
    arena = (ARENA*)alloc_pages(pages);
    if (!arena) return (ARENA*)0;

    arena->pages = pages;
    block = (BLOCK*)((boot_uint8_t*)arena + align_up(sizeof(ARENA)));
    block->magic = BLOCK_MAGIC;
    block->size = pages * PAGE_SIZE - align_up(sizeof(ARENA)) - sizeof(BLOCK);
    block->next = (BLOCK*)0;
    block->free = 1;

    arena->first = block;
    arena->next = arenas;
    arenas = arena;
    total_bytes += block->size;
    return arena;
}

void heap_init(void) {
    arenas = (ARENA*)0;
    total_bytes = 0;
    used_bytes = 0;
    (void)grow(0);
}

/* Cut `size` off the front of a free block when the remainder can still hold a
   header plus something useful. */
static void split(BLOCK* block, boot_uint64_t size) {
    BLOCK* remainder;

    if (block->size < size + sizeof(BLOCK) + ALIGNMENT) return;
    remainder = (BLOCK*)((boot_uint8_t*)block + sizeof(BLOCK) + size);
    remainder->magic = BLOCK_MAGIC;
    remainder->size = block->size - size - sizeof(BLOCK);
    remainder->next = block->next;
    remainder->free = 1;
    block->size = size;
    block->next = remainder;
    total_bytes -= sizeof(BLOCK);
}

static void* take(BLOCK* block, boot_uint64_t size) {
    split(block, size);
    block->free = 0;
    used_bytes += block->size;
    return (boot_uint8_t*)block + sizeof(BLOCK);
}

void* kmalloc(boot_uint64_t size) {
    if (!size) return (void*)0;
    size = align_up(size);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (ARENA* arena = arenas; arena; arena = arena->next)
            for (BLOCK* block = arena->first; block; block = block->next)
                if (block->free && block->size >= size) return take(block, size);
        /* Nothing fits: add an arena and try once more. */
        if (attempt == 0 && !grow(size)) break;
    }
    return (void*)0;
}

void* kcalloc(boot_uint64_t size) {
    void* pointer = kmalloc(size);
    if (pointer) memset(pointer, 0, align_up(size));
    return pointer;
}

void kfree(void* pointer) {
    BLOCK* block;

    if (!pointer) return;
    block = (BLOCK*)((boot_uint8_t*)pointer - sizeof(BLOCK));
    if (block->magic != BLOCK_MAGIC) panic("HEAP CORRUPTION IN KFREE");
    if (block->free) panic("DOUBLE FREE");

    block->free = 1;
    used_bytes -= block->size;

    /* Merge with the following block when it is also free. Only forward:
       backward merging would need a previous pointer in every header, and the
       free list is walked from the arena head often enough that fragmentation
       stays bounded without it. */
    while (block->next && block->next->free) {
        BLOCK* next = block->next;
        block->size += sizeof(BLOCK) + next->size;
        block->next = next->next;
        total_bytes += sizeof(BLOCK);
    }
}

boot_uint64_t heap_used(void) { return used_bytes; }
boot_uint64_t heap_total(void) { return total_bytes; }

int heap_self_test(void) {
    boot_uint64_t used_before = used_bytes;
    boot_uint8_t* blocks[8];
    void* large;

    /* Several small blocks, each written full, to catch a split that hands
       out overlapping payloads. */
    for (int i = 0; i < 8; i++) {
        blocks[i] = (boot_uint8_t*)kmalloc(100);
        if (!blocks[i]) return 0;
        memset(blocks[i], 0xA0 + i, 100);
    }
    for (int i = 0; i < 8; i++)
        for (int byte = 0; byte < 100; byte++)
            if (blocks[i][byte] != (boot_uint8_t)(0xA0 + i)) return 0;

    /* kcalloc must zero. */
    large = kcalloc(4096);
    if (!large) return 0;
    for (int byte = 0; byte < 4096; byte++)
        if (((boot_uint8_t*)large)[byte]) return 0;

    for (int i = 0; i < 8; i++) kfree(blocks[i]);
    kfree(large);

    /* Everything handed out came back. If coalescing were broken this would
       still pass, but a leak would not. */
    return used_bytes == used_before;
}
