#include "../include/syscall.h"
#include "syscall.h"
#include "command.h"
#include "console.h"
#include "serial.h"
#include "keyboard.h"
#include "layout.h"
#include "environment.h"
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
#include "graphics.h"
#include "mouse.h"
#include "rtc.h"
#include "audio.h"
#include "hda.h"
#include "build.h"
#include "string.h"

static const char* cpu_brand_name(void) {
    static char name[64];
    static int cached;
    boot_uint32_t a, b, c, d;

    if (cached) return name;

    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                              : "a"(0x80000000U), "c"(0));
    if (a < 0x80000004U) {
        memcpy(name, "unknown CPU", 12);
        cached = 1;
        return name;
    }

    for (int leaf = 0; leaf < 3; leaf++) {
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                  : "a"(0x80000002U + (boot_uint32_t)leaf),
                                    "c"(0));
        memcpy(name + leaf * 16 + 0, &a, 4);
        memcpy(name + leaf * 16 + 4, &b, 4);
        memcpy(name + leaf * 16 + 8, &c, 4);
        memcpy(name + leaf * 16 + 12, &d, 4);
    }
    name[48] = 0;

    {
        int start = 0;
        int end = 47;
        while (name[start] == ' ') start++;
        while (end >= start && name[end] == ' ') name[end--] = 0;
        if (start > 0) memmove(name, name + start, strlen(name + start) + 1);
    }

    cached = 1;
    return name;
}

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
   resolved from the same place the user was standing. */
#define WORKING_PATH_MAX 256
static VOLUME* working_volume;
static char working_path[WORKING_PATH_MAX] = "\\";

void syscall_set_location(VOLUME* volume, const char* directory) {
    boot_uint64_t length;

    working_volume = volume;
    if (!directory || directory[0] != '\\') {
        working_path[0] = '\\';
        working_path[1] = 0;
        return;
    }
    length = strlen(directory);
    if (length >= WORKING_PATH_MAX) length = WORKING_PATH_MAX - 1;
    memcpy(working_path, directory, length);
    working_path[length] = 0;
}

/* Turn whatever a program wrote into a path from the root of the drive.
 *
 * A leading backslash already means that. Anything else is relative to where
 * the shell was, which is the reading DOS has always had and the only one that
 * makes `ls` or `cat FILE.TXT` behave the same as the built-in commands. */
static void resolve_working(const char* name, char* result) {
    boot_uint64_t length = 0;

    if (name[0] == '\\') {
        while (name[length] && length + 1 < WORKING_PATH_MAX) {
            result[length] = name[length];
            length++;
        }
        result[length] = 0;
        return;
    }

    length = strlen(working_path);
    if (length >= WORKING_PATH_MAX) length = WORKING_PATH_MAX - 1;
    memcpy(result, working_path, length);
    if (length && result[length - 1] != '\\' && length + 1 < WORKING_PATH_MAX)
        result[length++] = '\\';
    for (boot_uint64_t index = 0;
         name[index] && length + 1 < WORKING_PATH_MAX; index++)
        result[length++] = name[index];
    result[length] = 0;
}

/* Memory a program asked for.
 *
 * Tracked rather than handed out and forgotten, because nothing here reclaims
 * memory later: a block a program leaks would be gone until the machine is
 * restarted. Eight blocks is generous - a program that needs more than a
 * handful of large allocations should be taking one and dividing it itself,
 * which is what a program large enough to care already does. */
#define BLOCK_MAX 8

typedef struct {
    void* address;
    boot_uint64_t pages;
} PROGRAM_BLOCK;

static PROGRAM_BLOCK blocks[BLOCK_MAX];

/* The clipboard. Kernel memory, and deliberately not freed when a program
   exits: the whole point is that it survives the program that filled it. */
static char* clipboard;
static boot_uint32_t clipboard_length;

static long do_alloc(long bytes) {
    boot_uint64_t pages;
    int slot;
    void* address;

    if (bytes <= 0) return 0;
    pages = ((boot_uint64_t)bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (slot = 0; slot < BLOCK_MAX; slot++) if (!blocks[slot].address) break;
    if (slot == BLOCK_MAX) return 0;

    address = alloc_pages(pages);
    if (!address) return 0;

    blocks[slot].address = address;
    blocks[slot].pages = pages;
    return (long)(unsigned long long)address;
}

static long do_free(long address) {
    if (!address) return 0;
    for (int slot = 0; slot < BLOCK_MAX; slot++) {
        if ((long)(unsigned long long)blocks[slot].address != address) continue;
        free_pages(blocks[slot].address, blocks[slot].pages);
        blocks[slot].address = (void*)0;
        blocks[slot].pages = 0;
        return 0;
    }
    /* An address this never handed out. Refusing beats freeing whatever
       happens to be there. */
    return SYSCALL_ERROR;
}

void syscall_close_all(void) {
    memset(handles, 0, sizeof(handles));
    memset(searches, 0, sizeof(searches));
    /* Sound first, and this one is not tidiness.
     *
     * A voice holds a pointer into the program's own memory and the mixer
     * reads it from the timer interrupt. Leave one playing past the program
     * that started it and the mixer keeps reading an address that now belongs
     * to something else - which is not a wrong noise, it is a wrong noise
     * arriving a thousand times a second forever. */
    audio_stop_all();
    /* Anything the program still held goes back, whether or not it asked. */
    for (int slot = 0; slot < BLOCK_MAX; slot++) {
        if (!blocks[slot].address) continue;
        free_pages(blocks[slot].address, blocks[slot].pages);
        blocks[slot].address = (void*)0;
        blocks[slot].pages = 0;
    }
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
    char directory[WORKING_PATH_MAX];
    char absolute[WORKING_PATH_MAX];
    int slot;

    if (!pattern || !data || !working_volume) return SYSCALL_ERROR;
    for (slot = 0; slot < SEARCH_MAX; slot++) if (!searches[slot].used) break;
    if (slot == SEARCH_MAX) return SYSCALL_ERROR;

    memset(&searches[slot], 0, sizeof(searches[slot]));
    resolve_working(pattern, absolute);
    split_search(absolute, directory, searches[slot].pattern);
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
    char absolute[WORKING_PATH_MAX];
    int slot;

    if (!path || !working_volume) return SYSCALL_ERROR;
    for (slot = 0; slot < HANDLE_MAX; slot++) if (!handles[slot].used) break;
    if (slot == HANDLE_MAX) return SYSCALL_ERROR;

    memset(&handles[slot], 0, sizeof(handles[slot]));
    handles[slot].volume = working_volume;
    resolve_working(path, absolute);

    if (mode == OPEN_WRITE) {
        /* Truncate by removing and recreating: the alternative is walking the
           cluster chain to release the tail, and this is one program-visible
           operation either way. */
        FAT_ENTRY existing;
        if (fat32_stat(working_volume, absolute, &existing))
            (void)fat32_remove(working_volume, absolute);
        if (!fat32_create(working_volume, absolute, 0, &handles[slot].entry))
            return SYSCALL_ERROR;
        handles[slot].writable = 1;
    } else {
        if (!fat32_stat(working_volume, absolute, &handles[slot].entry))
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
    case KOI_INFO_VOLUME_IS_CURRENT: {
        VOLUME* volume = volume_at((boot_uint32_t)index);
        if (!volume) return SYSCALL_ERROR;
        return volume == working_volume ? 1 : 0;
    }
    case KOI_INFO_VOLUME_TOTAL_BYTES: {
        VOLUME* volume = volume_at((boot_uint32_t)index);
        return volume ? (long)(fat32_total_bytes(volume) / 1024U)
                      : SYSCALL_ERROR;
    }
    case KOI_INFO_VOLUME_FREE_BYTES: {
        VOLUME* volume = volume_at((boot_uint32_t)index);
        return volume ? (long)(fat32_free_bytes(volume) / 1024U)
                      : SYSCALL_ERROR;
    }
    case KOI_INFO_TIME: {
        RTC_TIME now;
        rtc_read(&now);
        return ((long)now.hour << 16) | ((long)now.minute << 8) | now.second;
    }
    case KOI_INFO_DATE: {
        RTC_TIME now;
        rtc_read(&now);
        return ((long)now.year << 16) | ((long)now.month << 8) | now.day;
    }
    case KOI_INFO_AUDIO:
        return audio_ready() ? 1 : 0;
    case KOI_INFO_AUDIO_RATE:
        return audio_ready() ? (long)HDA_RATE : 0;

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
    case KOI_TEXT_AUDIO_DEVICE:
        source = audio_device_name();
        break;
    case KOI_TEXT_CPU_NAME:
        source = cpu_brand_name();
        break;
    case KOI_TEXT_PROGRAM_PATH:
        source = program_path();
        break;
    case KOI_TEXT_VERSION_NAME:
        source = KOI_VERSION_NAME;
        break;
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
    /* Ctrl+C, acted on here and nowhere else.
     *
     * A program is stopped at a system call because that is the one moment it
     * is known to be between two pieces of its own work and standing in the
     * kernel on purpose. Ending it from the keyboard interrupt instead would
     * mean abandoning a stack mid-instruction, with whatever the program was
     * holding still held.
     *
     * The unwind itself is not new: it is exactly what SYS_EXIT does below,
     * and has done since programs existed. What Ctrl+C adds is the decision,
     * not the mechanism.
     *
     * What this cannot catch, and it is worth being plain about: a program
     * that loops forever without calling anything. It never enters the kernel,
     * so the kernel never gets a turn. Everything that prints, reads a key,
     * sleeps, or touches a file is interruptible - which is nearly everything,
     * including the accidental loops people actually write. The rest waits for
     * a scheduler that can take the processor away, and that is a different
     * piece of work.
     *
     * SYS_EXIT is exempt: a program already leaving does not need stopping,
     * and its exit code is its own. */
    if (function != SYS_EXIT && program_depth() > 0) {
        /* Ask the USB controllers whether anything was typed.
         *
         * Their interrupt is not routed, so a USB keyboard is silent until
         * somebody drains its event ring - and the only place that happened
         * was the loop that waits for a keystroke. A program that is not
         * waiting for one therefore never heard anything, which is precisely
         * the program Ctrl+C is for. On a machine whose keyboard is USB, and
         * that is most machines, this key would not have worked at all.
         *
         * Not on every call: a program printing in a loop makes thousands a
         * second and draining a ring is not free. Twenty milliseconds is far
         * below what anybody notices between pressing a key and the program
         * stopping, and far above the cost. */
        static boot_uint64_t last_poll;
        boot_uint64_t now = timer_ticks();

        if (now - last_poll >= 20) {
            last_poll = now;
            if (xhci_controller_count()) xhci_poll();
        }

        if (keyboard_break_taken()) {
            write_out("^C\n");
            program_exit(KOI_EXIT_INTERRUPTED);
        }
    }

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

    case SYS_LAYOUT_GESTURE:
        layout_gesture_enable((int)a);
        return 0;

    case SYS_GETENV:
    case SYS_ENVAT: {
        const char* source = (const char*)0;
        char* into;
        long size;
        long length = 0;

        if (function == SYS_GETENV) {
            if (!a || !b || c <= 0) return 0;
            source = environment_get((const char*)a);
            into = (char*)b;
            size = c;
        } else {
            if (!b || c <= 0) return 0;
            if (!environment_at((int)a, &source, (const char**)0)) source = 0;
            into = (char*)b;
            size = c;
        }
        if (!source) { into[0] = 0; return 0; }
        while (source[length] && length + 1 < size) {
            into[length] = source[length];
            length++;
        }
        into[length] = 0;
        return length;
    }

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

    case SYS_GOTOXY:
        console_set_cursor((boot_uint32_t)KOI_POINT_X(a),
                           (boot_uint32_t)KOI_POINT_Y(a));
        return 0;

    case SYS_CURSOR:
        console_show_cursor((int)a);
        return 0;

    case SYS_KEYPRESSED:
        return keyboard_pending() ? 1 : 0;

    case SYS_KEYEVENT:
        return keyboard_next_event();

    case SYS_SLEEP:
        /* Clamped rather than trusted. A program that computes a delay wrongly
           should stutter, not hang the only thread the system has. */
        if (a > 0) timer_wait(a > 60000 ? 60000 : (boot_uint64_t)a);
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

    case SYS_SEEK: {
        OPEN_FILE* file = handle_of(a);
        long position;

        if (!file) return SYSCALL_ERROR;
        switch (b) {
        case KOI_SEEK_SET: position = (long)c; break;
        case KOI_SEEK_CURRENT: position = (long)file->position + (long)c; break;
        case KOI_SEEK_END: position = (long)file->entry.size + (long)c; break;
        default: return SYSCALL_ERROR;
        }
        /* Past the end is allowed to be asked for and clamped rather than
           refused: a caller seeking to the end to measure a file is doing
           something ordinary, and reading there simply returns nothing. */
        if (position < 0) return SYSCALL_ERROR;
        if ((boot_uint32_t)position > file->entry.size)
            position = (long)file->entry.size;
        file->position = (boot_uint32_t)position;
        return position;
    }

    case SYS_REMOVE: {
        char absolute[WORKING_PATH_MAX];
        if (!a || !working_volume) return SYSCALL_ERROR;
        resolve_working((const char*)a, absolute);
        return fat32_remove(working_volume, absolute) ? 0 : SYSCALL_ERROR;
    }

    case SYS_RENAME: {
        char from[WORKING_PATH_MAX];
        char to[WORKING_PATH_MAX];
        if (!a || !b || !working_volume) return SYSCALL_ERROR;
        resolve_working((const char*)a, from);
        resolve_working((const char*)b, to);
        return fat32_rename(working_volume, from, to) ? 0 : SYSCALL_ERROR;
    }

    case SYS_MKDIR: {
        char absolute[WORKING_PATH_MAX];
        FAT_ENTRY entry;
        if (!a || !working_volume) return SYSCALL_ERROR;
        resolve_working((const char*)a, absolute);
        return fat32_create(working_volume, absolute, 1, &entry)
               ? 0 : SYSCALL_ERROR;
    }

    case SYS_EXISTS: {
        char absolute[WORKING_PATH_MAX];
        FAT_ENTRY entry;
        if (!a || !working_volume) return SYSCALL_ERROR;
        resolve_working((const char*)a, absolute);
        return fat32_stat(working_volume, absolute, &entry) ? 1 : 0;
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

    case SYS_ALLOC:
        return do_alloc(a);

    case SYS_FREE:
        return do_free(a);

    /* Graphics. Every drawing call is silently ignored when the program has
       not entered the mode - the alternative is a program that forgot to enter
       getting a stream of errors it has no way to act on, when the honest
       answer is that nothing was drawn. */
    case SYS_GFX_ENTER: {
        GRAPHICS_SCREEN screen;
        KOI_SCREEN* out = (KOI_SCREEN*)a;

        if (!out || !graphics_enter(&screen)) return SYSCALL_ERROR;
        out->width = screen.width;
        out->height = screen.height;
        out->pitch = screen.pitch;
        out->bytes_per_pixel = screen.bytes_per_pixel;
        out->pixels = screen.pixels;
        return 0;
    }

    case SYS_GFX_LEAVE:
        graphics_leave();
        return 0;

    case SYS_SOUND_PLAY: {
        const KOI_SOUND* sound = (const KOI_SOUND*)(unsigned long long)a;
        if (!sound) return SYSCALL_ERROR;
        return audio_play(sound->samples, sound->frames, sound->rate,
                          sound->bits, sound->channels, sound->volume,
                          sound->pan, (int)sound->loop);
    }

    case SYS_SOUND_TONE:
        return audio_tone((boot_uint32_t)a, (boot_uint32_t)b, (int)c);

    case SYS_SOUND_STOP:
        /* -1 means all of them, which is what a program wants when it is
           shutting down and does not want to have kept a list. */
        if (a < 0) audio_stop_all();
        else audio_stop((int)a);
        return 0;

    case SYS_SOUND_PARAMS:
        return audio_set_params((int)a, (int)b, (int)c);

    case SYS_SOUND_WHERE:
        return (long)audio_position((int)a);
    case SYS_SOUND_LENGTH:
        return (long)audio_length((int)a);
    case SYS_SOUND_SEEK:
        return audio_seek((int)a, (boot_uint32_t)b);
    case SYS_SOUND_ACTIVE:
        return audio_active((int)a) ? 1 : 0;

    case SYS_SOUND_VOLUME:
        if (a >= 0) audio_set_volume((int)a);
        return audio_volume();

    case SYS_GFX_PRESENT:
        graphics_present();
        return 0;

    case SYS_GFX_PRESENT_RECT:
        graphics_present_rect(KOI_POINT_X(a), KOI_POINT_Y(a),
                              KOI_POINT_X(b), KOI_POINT_Y(b));
        return 0;

    case SYS_GFX_COLOR:
        return (long)graphics_color((boot_uint8_t)a, (boot_uint8_t)b,
                                    (boot_uint8_t)c);

    case SYS_GFX_CLEAR:
        graphics_clear((boot_uint32_t)a);
        return 0;

    case SYS_GFX_PIXEL:
        graphics_pixel(KOI_POINT_X(a), KOI_POINT_Y(a), (boot_uint32_t)b);
        return 0;

    case SYS_GFX_LINE:
        graphics_line(KOI_POINT_X(a), KOI_POINT_Y(a),
                      KOI_POINT_X(b), KOI_POINT_Y(b), (boot_uint32_t)c);
        return 0;

    case SYS_GFX_RECT:
        graphics_rect(KOI_POINT_X(a), KOI_POINT_Y(a),
                      KOI_POINT_X(b), KOI_POINT_Y(b), (boot_uint32_t)c);
        return 0;

    case SYS_GFX_FILL:
        graphics_fill(KOI_POINT_X(a), KOI_POINT_Y(a),
                      KOI_POINT_X(b), KOI_POINT_Y(b), (boot_uint32_t)c);
        return 0;

    case SYS_GFX_TEXT_STYLED:
        /* The style rides in the high half of the background word: the call
           already takes four arguments and a fifth would mean a different
           shape for one flag. */
        graphics_text_styled(KOI_POINT_X(a), KOI_POINT_Y(a), (const char*)b,
                             (boot_uint32_t)c, (boot_uint32_t)(d & 0xFFFFFFFF),
                             (int)(d & 0xFFFFFFFF) == KOI_TEXT_TRANSPARENT,
                             (int)((d >> 32) & 0xFF));
        return 0;
    case SYS_GFX_SCISSOR:
        graphics_scissor(KOI_POINT_X(a), KOI_POINT_Y(a),
                         KOI_POINT_X(b), KOI_POINT_Y(b));
        return 0;
    case SYS_GFX_SCISSOR_RESET:
        graphics_reset_scissor();
        return 0;
    case SYS_GFX_DIM:
        graphics_dim(KOI_POINT_X(a), KOI_POINT_Y(a),
                     KOI_POINT_X(b), KOI_POINT_Y(b), (int)c);
        return 0;
    case SYS_GFX_TEXT:
        graphics_text(KOI_POINT_X(a), KOI_POINT_Y(a), (const char*)b,
                      (boot_uint32_t)c, (boot_uint32_t)(d < 0 ? 0 : d),
                      d == KOI_TEXT_TRANSPARENT);
        return 0;

    case SYS_MOUSE: {
        KOI_POINTER* out = (KOI_POINTER*)a;

        if (!out) return SYSCALL_ERROR;
        /* Filled in even when there is no pointer, so that a program which
           ignores the return value reads a still cursor at the origin rather
           than whatever happened to be in its stack. */
        out->x = mouse_present() ? mouse_x() : 0;
        out->y = mouse_present() ? mouse_y() : 0;
        out->buttons = mouse_present() ? (unsigned int)mouse_buttons() : 0;
        out->scroll = mouse_present() ? mouse_scroll() : 0;
        out->movements = mouse_present() ? mouse_movements() : 0;
        out->has_wheel = (mouse_present() && mouse_has_wheel()) ? 1U : 0U;
        for (int button = 0; button < 3; button++)
            out->presses[button] = mouse_present()
                ? (unsigned int)mouse_presses(button) : 0U;
        return mouse_present() ? 1 : 0;
    }

    case SYS_MOUSE_PLACE:
        if (!mouse_present()) return SYSCALL_ERROR;
        mouse_place(KOI_POINT_X(a), KOI_POINT_Y(a));
        return 0;

    case SYS_CHAIN:
        return program_chain((const char*)a) ? 1 : 0;

    case SYS_LOG:
        if (!a) return SYSCALL_ERROR;
        serial_write((const char*)a);
        return 0;

    case SYS_LOG_BYTES:
        if (!b || c <= 0) return SYSCALL_ERROR;
        serial_write_bytes((const char*)a, (const void*)b, (boot_uint32_t)c);
        return 0;

    case SYS_SECTOR_READ: {
        BLOCK_DEVICE* device = block_device((boot_uint32_t)a);

        if (!device || !c) return SYSCALL_ERROR;
        if (!block_read(device, (boot_uint64_t)b, 1, (void*)c))
            return SYSCALL_ERROR;
        return (long)device->sector_size;
    }

    case SYS_RUN: {
        /* The exit code of what was run is not carried back yet: the shell
           prints it and does not return it. Zero means "it ran"; -1 means the
           command line was not usable. Saying so is better than inventing a
           number that looks like a result. */
        const char* line = (const char*)a;
        if (!line) return -1;
        command_execute_line(line);
        return 0;
    }
    case SYS_SECTOR_SIZE: {
        BLOCK_DEVICE* device = block_device((boot_uint32_t)a);
        return device ? (long)device->sector_size : SYSCALL_ERROR;
    }

    case SYS_CLIP_PUT: {
        const char* text = (const char*)a;
        long length = b;

        if (!text) return SYSCALL_ERROR;
        if (length < 0) length = (long)strlen(text);
        if (length > KOI_CLIP_MAX) length = KOI_CLIP_MAX;

        /* Taken on the first use and kept. Sixty-four kilobytes held for the
           life of the machine is cheaper than the alternative, which is a
           clipboard that fails on a machine that has been running a while. */
        if (!clipboard) {
            clipboard = (char*)kmalloc(KOI_CLIP_MAX + 1);
            if (!clipboard) return SYSCALL_ERROR;
        }
        memcpy(clipboard, text, (boot_uint64_t)length);
        clipboard[length] = 0;
        clipboard_length = (boot_uint32_t)length;
        return length;
    }

    case SYS_CLIP_GET: {
        char* out = (char*)a;
        long size = b;
        long length = (long)clipboard_length;

        if (!clipboard || !clipboard_length) return 0;
        /* Asking with no buffer asks how much room to make. */
        if (!out || size <= 0) return length;
        if (length > size - 1) length = size - 1;
        memcpy(out, clipboard, (boot_uint64_t)length);
        out[length] = 0;
        return length;
    }

    case SYS_SETDRIVE: {
        int letter = (int)a;

        if (letter >= 'a' && letter <= 'z') letter -= 'a' - 'A';
        for (boot_uint32_t index = 0; index < volume_count(); index++) {
            VOLUME* volume = volume_at(index);

            if (!volume || (int)volume->letter != letter) continue;
            working_volume = volume;
            /* Back to the root. The directory it was standing in belonged to
               the drive it has just left, and a path that happens to exist on
               both drives is worse than one that exists on neither. */
            working_path[0] = '\\';
            working_path[1] = 0;
            return 1;
        }
        return SYSCALL_ERROR;
    }

    default:
        return SYSCALL_ERROR;
    }
}
