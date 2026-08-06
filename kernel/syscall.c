#include "../include/syscall.h"
#include "syscall.h"
#include "console.h"
#include "serial.h"
#include "keyboard.h"
#include "fat32.h"
#include "partition.h"
#include "program.h"
#include "string.h"
#include "heap.h"
#include "memory.h"
#include "block.h"
#include "timer.h"
#include "pci.h"
#include "xhci.h"
#include "build.h"
#include "string.h"

/* Open files belonging to the running program. Small and fixed: one program
   runs at a time and DOS itself shipped with FILES=8 in CONFIG.SYS. */
#define HANDLE_MAX 8

typedef struct {
    int used;
    int writable;
    VOLUME* volume;
    FAT_ENTRY entry;
    boot_uint32_t position;
} OPEN_FILE;

static OPEN_FILE handles[HANDLE_MAX];

/* Directory searches in flight. A program walking two directories at once is
   unusual but cheap to allow. */
#define SEARCH_MAX 4

typedef struct {
    int used;
    VOLUME* volume;
    FAT_DIRECTORY directory;
    char pattern[FAT_NAME_MAX];
} SEARCH;

static SEARCH searches[SEARCH_MAX];

/* Set by the shell before a program starts, so file paths a program passes are
   resolved against the same drive the user was on. */
static VOLUME* working_volume;

void syscall_set_volume(VOLUME* volume) {
    working_volume = volume;
}

void syscall_close_all(void) {
    memset(handles, 0, sizeof(handles));
    memset(searches, 0, sizeof(searches));
}

static long find_next(long search, KOI_FIND_DATA* data);

/* Split "\\DIR\\*.TXT" into the directory to walk and the pattern to match. */
static void split_search(const char* path, char* directory, char* pattern) {
    const char* last = path;
    boot_uint64_t length;

    for (const char* cursor = path; *cursor; cursor++)
        if (*cursor == '\\') last = cursor + 1;

    length = strlen(last);
    if (length >= FAT_NAME_MAX) length = FAT_NAME_MAX - 1;
    memcpy(pattern, last, length);
    pattern[length] = 0;

    length = (boot_uint64_t)(last - path);
    while (length > 1 && path[length - 1] == '\\') length--;
    if (!length) { directory[0] = '\\'; directory[1] = 0; return; }
    memcpy(directory, path, length);
    directory[length] = 0;
}

static void fill_find_data(const FAT_ENTRY* entry, KOI_FIND_DATA* data) {
    boot_uint64_t length = strlen(entry->name);
    if (length >= KOI_NAME_MAX) length = KOI_NAME_MAX - 1;
    memcpy(data->name, entry->name, length);
    data->name[length] = 0;
    data->attributes = entry->attributes;
    data->size = entry->size;
    data->date = entry->modified_date;
    data->time = entry->modified_time;
}

static long find_first(const char* pattern, KOI_FIND_DATA* data) {
    char directory[FAT_NAME_MAX];
    int slot;

    if (!pattern || !data || !working_volume) return SYSCALL_ERROR;
    for (slot = 0; slot < SEARCH_MAX; slot++) if (!searches[slot].used) break;
    if (slot == SEARCH_MAX) return SYSCALL_ERROR;

    memset(&searches[slot], 0, sizeof(searches[slot]));
    split_search(pattern, directory, searches[slot].pattern);
    if (!searches[slot].pattern[0]) {
        searches[slot].pattern[0] = '*';
        searches[slot].pattern[1] = 0;
    }
    if (!fat32_opendir(working_volume, directory, &searches[slot].directory))
        return SYSCALL_ERROR;
    searches[slot].volume = working_volume;
    searches[slot].used = 1;

    if (find_next(slot, data) != 0) {
        searches[slot].used = 0;
        return SYSCALL_ERROR;
    }
    return slot;
}

static long find_next(long search, KOI_FIND_DATA* data) {
    FAT_ENTRY entry;

    if (search < 0 || search >= SEARCH_MAX || !searches[search].used || !data)
        return SYSCALL_ERROR;
    while (fat32_readdir(&searches[search].directory, &entry)) {
        if (!glob_match(searches[search].pattern, entry.name)) continue;
        fill_find_data(&entry, data);
        return 0;
    }
    return SYSCALL_ERROR;
}

static void write_out(const char* text) {
    console_write(text);
    serial_write(text);
}

static long do_open(const char* path, long mode) {
    int slot;

    if (!path || !working_volume) return SYSCALL_ERROR;
    for (slot = 0; slot < HANDLE_MAX; slot++) if (!handles[slot].used) break;
    if (slot == HANDLE_MAX) return SYSCALL_ERROR;

    memset(&handles[slot], 0, sizeof(handles[slot]));
    handles[slot].volume = working_volume;

    if (mode == OPEN_WRITE) {
        /* Truncate by removing and recreating: the alternative is walking the
           cluster chain to release the tail, and this is one program-visible
           operation either way. */
        FAT_ENTRY existing;
        if (fat32_stat(working_volume, path, &existing))
            (void)fat32_remove(working_volume, path);
        if (!fat32_create(working_volume, path, 0, &handles[slot].entry))
            return SYSCALL_ERROR;
        handles[slot].writable = 1;
    } else {
        if (!fat32_stat(working_volume, path, &handles[slot].entry))
            return SYSCALL_ERROR;
        if (handles[slot].entry.attributes & FAT_ATTRIBUTE_DIRECTORY)
            return SYSCALL_ERROR;
    }
    handles[slot].used = 1;
    return slot;
}

static OPEN_FILE* handle_of(long handle) {
    if (handle < 0 || handle >= HANDLE_MAX) return (OPEN_FILE*)0;
    return handles[handle].used ? &handles[handle] : (OPEN_FILE*)0;
}

/* Called from the vector 0x40 stub. Four arguments, matching the ABI in
   include/syscall.h; `d` is unused so far but is part of the contract. */
/* What the system knows about itself, as numbers.
 *
 * Everything here is already printed by `mem` or the boot log; the point of
 * the call is that a program can ask instead of a person reading it off the
 * screen. An unknown item is an error rather than zero, so a program built
 * against a newer header can tell the two apart. */
static long system_info(long item, long index) {
    switch (item) {
    case KOI_INFO_MEMORY_TOTAL:
        return (long)(memory_physical_pages() * PAGE_SIZE / 1024U);
    case KOI_INFO_MEMORY_FREE:
        return (long)(memory_free_pages() * PAGE_SIZE / 1024U);
    case KOI_INFO_KERNEL_SIZE:
        return (long)(memory_kernel_bytes() / 1024U);
    case KOI_INFO_HEAP_TOTAL:
        return (long)(heap_total() / 1024U);
    case KOI_INFO_HEAP_FREE:
        return (long)((heap_total() - heap_used()) / 1024U);
    case KOI_INFO_UPTIME_MS:
        return (long)timer_ticks();
    case KOI_INFO_BUILD_NUMBER:
        return KOI_BUILD_NUMBER;
    case KOI_INFO_SCREEN_WIDTH:
        return (long)console_width();
    case KOI_INFO_SCREEN_HEIGHT:
        return (long)console_height();
    case KOI_INFO_TEXT_COLUMNS:
        return (long)console_columns();
    case KOI_INFO_TEXT_ROWS:
        return (long)console_rows();
    case KOI_INFO_PCI_DEVICES:
        return (long)pci_device_count();
    case KOI_INFO_DISK_COUNT:
        return (long)block_device_count();
    case KOI_INFO_VOLUME_COUNT:
        return (long)volume_count();
    case KOI_INFO_USB_PORTS:
        return (long)xhci_port_count();
    case KOI_INFO_USB_PORTS_USED:
        return (long)xhci_ports_connected();
    case KOI_INFO_TIMER_HZ:
        return TIMER_HZ;
    case KOI_INFO_TIMER_IS_INTERRUPT:
        return timer_is_interrupt_driven();

    case KOI_INFO_DISK_SECTORS: {
        BLOCK_DEVICE* device = block_device((boot_uint32_t)index);
        return device ? (long)device->sector_count : SYSCALL_ERROR;
    }
    case KOI_INFO_DISK_SECTOR_SIZE: {
        BLOCK_DEVICE* device = block_device((boot_uint32_t)index);
        return device ? (long)device->sector_size : SYSCALL_ERROR;
    }
    case KOI_INFO_VOLUME_LETTER: {
        VOLUME* volume = volume_at((boot_uint32_t)index);
        return volume ? (long)(unsigned char)volume->letter : SYSCALL_ERROR;
    }
    case KOI_INFO_VOLUME_IS_BOOT: {
        VOLUME* volume = volume_at((boot_uint32_t)index);
        return volume ? volume->is_boot_volume : SYSCALL_ERROR;
    }

    default:
        return SYSCALL_ERROR;
    }
}

/* The same, as text. Always terminated; returns the length written. */
static long system_text(long item, long index, char* buffer, long size) {
    const char* source = 0;

    if (!buffer || size <= 0) return SYSCALL_ERROR;

    switch (item) {
    case KOI_TEXT_BUILD_DATE:
        source = KOI_BUILD_DATE;
        break;
    case KOI_TEXT_BUILD_COMMIT:
        source = KOI_BUILD_COMMIT;
        break;
    case KOI_TEXT_DISK_NAME: {
        BLOCK_DEVICE* device = block_device((boot_uint32_t)index);
        if (!device) return SYSCALL_ERROR;
        source = device->name;
        break;
    }
    case KOI_TEXT_VOLUME_LABEL: {
        VOLUME* volume = volume_at((boot_uint32_t)index);
        if (!volume) return SYSCALL_ERROR;
        source = volume->label;
        break;
    }
    default:
        return SYSCALL_ERROR;
    }

    {
        long length = 0;
        while (source[length] && length + 1 < size) {
            buffer[length] = source[length];
            length++;
        }
        buffer[length] = 0;
        return length;
    }
}

long syscall_dispatch(long function, long a, long b, long c, long d);

long syscall_dispatch(long function, long a, long b, long c, long d) {
    switch (function) {
    case SYS_EXIT:
        program_exit((int)a);

    case SYS_PUTCHAR:
        console_putchar((char)a);
        serial_putchar((char)a);
        return 0;

    case SYS_PUTS:
        if (a) write_out((const char*)a);
        return 0;

    case SYS_GETCHAR:
        return keyboard_getchar();

    case SYS_READLINE:
        if (!a || b <= 0) return 0;
        return (long)keyboard_read_line((char*)a, (boot_uint64_t)b);

    case SYS_CLS:
        console_clear();
        return 0;

    case SYS_SETCOLOR:
        console_set_color((boot_uint8_t)a, (boot_uint8_t)b);
        return 0;

    case SYS_SETTHEME: {
        CONSOLE_THEME theme = *console_theme();
        if (a >= 0 && a < 16) theme.foreground = (boot_uint8_t)a;
        if (b >= 0 && b < 16) theme.background = (boot_uint8_t)b;
        if (c >= 0 && c < 16) theme.prompt = (boot_uint8_t)c;
        if (d >= 0 && d < 16) theme.error = (boot_uint8_t)d;
        console_set_theme(&theme);
        console_use_theme();
        return (long)theme.foreground | ((long)theme.background << 8) |
               ((long)theme.prompt << 16) | ((long)theme.error << 24);
    }

    case SYS_OPEN:
        return do_open((const char*)a, b);

    case SYS_CLOSE: {
        OPEN_FILE* file = handle_of(a);
        if (!file) return SYSCALL_ERROR;
        file->used = 0;
        return 0;
    }

    case SYS_READ: {
        OPEN_FILE* file = handle_of(a);
        boot_uint32_t got;
        if (!file || !b || c <= 0) return SYSCALL_ERROR;
        got = fat32_read(file->volume, &file->entry, file->position,
                         (void*)b, (boot_uint32_t)c);
        file->position += got;
        return (long)got;
    }

    case SYS_WRITE: {
        OPEN_FILE* file = handle_of(a);
        boot_uint32_t put;
        if (!file || !file->writable || !b || c <= 0) return SYSCALL_ERROR;
        put = fat32_write(file->volume, &file->entry, file->position,
                          (const void*)b, (boot_uint32_t)c);
        file->position += put;
        return (long)put;
    }

    case SYS_SIZE: {
        OPEN_FILE* file = handle_of(a);
        return file ? (long)file->entry.size : SYSCALL_ERROR;
    }

    case SYS_FINDFIRST:
        return find_first((const char*)a, (KOI_FIND_DATA*)b);

    case SYS_FINDNEXT:
        return find_next(a, (KOI_FIND_DATA*)b);

    case SYS_FINDCLOSE:
        if (a >= 0 && a < SEARCH_MAX) searches[a].used = 0;
        return 0;

    case SYS_ARGS:
        return (long)program_arguments();

    case SYS_VERSION:
        /* Major in the high byte, minor in the low one. The same number the
           `ver` command prints - two versions for one system would be one
           too many. */
        return KOI_DOS_VERSION;

    case SYS_SYSINFO:
        return system_info(a, b);

    case SYS_SYSTEXT:
        return system_text(a, b, (char*)c, d);

    default:
        return SYSCALL_ERROR;
    }
}
