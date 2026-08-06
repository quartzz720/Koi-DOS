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

    default:
        return SYSCALL_ERROR;
    }
}
