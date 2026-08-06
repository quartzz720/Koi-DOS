#ifndef KOI_H
#define KOI_H

/* The Koi-DOS program interface.
 *
 * Everything here is a thin wrapper over the software interrupt described in
 * include/syscall.h. There is no C library: a program gets these calls, and
 * whatever it writes itself. */

#include "../include/syscall.h"

typedef unsigned char koi_uint8;
typedef unsigned short koi_uint16;
typedef unsigned int koi_uint32;
typedef unsigned long long koi_uint64;

#define KOI_NULL ((void*)0)

/* Colours, matching the kernel console. */
#define KOI_BLACK 0
#define KOI_BLUE 1
#define KOI_GREEN 2
#define KOI_CYAN 3
#define KOI_RED 4
#define KOI_MAGENTA 5
#define KOI_BROWN 6
#define KOI_LIGHT_GRAY 7
#define KOI_DARK_GRAY 8
#define KOI_LIGHT_BLUE 9
#define KOI_LIGHT_GREEN 10
#define KOI_LIGHT_CYAN 11
#define KOI_LIGHT_RED 12
#define KOI_LIGHT_MAGENTA 13
#define KOI_YELLOW 14
#define KOI_WHITE 15

/* The interrupt itself. RAX carries the function number, RDI/RSI/RDX/RCX the
   arguments; the compiler is told RAX is both an input and the result. */
static inline long koi_call(long function, long a, long b, long c) {
    long result;
    __asm__ volatile ("int %1"
                      : "=a"(result)
                      : "i"(SYSCALL_VECTOR), "a"(function),
                        "D"(a), "S"(b), "d"(c)
                      : "memory", "cc", "r11");
    return result;
}

static inline void koi_exit(int code) {
    koi_call(SYS_EXIT, code, 0, 0);
    for (;;);   /* unreachable; keeps the compiler from falling through */
}

static inline void koi_putchar(char character) {
    koi_call(SYS_PUTCHAR, (long)(unsigned char)character, 0, 0);
}

static inline void koi_print(const char* text) {
    koi_call(SYS_PUTS, (long)text, 0, 0);
}

static inline int koi_getchar(void) {
    return (int)koi_call(SYS_GETCHAR, 0, 0, 0);
}

static inline long koi_readline(char* buffer, long size) {
    return koi_call(SYS_READLINE, (long)buffer, size, 0);
}

static inline void koi_cls(void) {
    koi_call(SYS_CLS, 0, 0, 0);
}

static inline void koi_color(int foreground, int background) {
    koi_call(SYS_SETCOLOR, foreground, background, 0);
}

/* Change the shell's colours for this session. Pass -1 to leave one alone.
   Returns the resulting theme packed; use the KOI_THEME_* macros on it.
   Making it stick is the caller's job: write the configuration file. */
static inline long koi_theme(int foreground, int background, int prompt,
                             int error) {
    long result;
    __asm__ volatile ("int %1"
                      : "=a"(result)
                      : "i"(SYSCALL_VECTOR), "a"((long)SYS_SETTHEME),
                        "D"((long)foreground), "S"((long)background),
                        "d"((long)prompt), "c"((long)error)
                      : "memory", "cc", "r11");
    return result;
}

static inline long koi_open(const char* path, long mode) {
    return koi_call(SYS_OPEN, (long)path, mode, 0);
}

static inline long koi_close(long handle) {
    return koi_call(SYS_CLOSE, handle, 0, 0);
}

static inline long koi_read(long handle, void* buffer, long length) {
    return koi_call(SYS_READ, handle, (long)buffer, length);
}

static inline long koi_write(long handle, const void* buffer, long length) {
    return koi_call(SYS_WRITE, handle, (long)buffer, length);
}

static inline long koi_filesize(long handle) {
    return koi_call(SYS_SIZE, handle, 0, 0);
}

static inline long koi_findfirst(const char* pattern, KOI_FIND_DATA* data) {
    return koi_call(SYS_FINDFIRST, (long)pattern, (long)data, 0);
}

static inline long koi_findnext(long search, KOI_FIND_DATA* data) {
    return koi_call(SYS_FINDNEXT, search, (long)data, 0);
}

static inline void koi_findclose(long search) {
    koi_call(SYS_FINDCLOSE, search, 0, 0);
}

static inline const char* koi_arguments(void) {
    return (const char*)koi_call(SYS_ARGS, 0, 0, 0);
}

static inline long koi_version(void) {
    return koi_call(SYS_VERSION, 0, 0, 0);
}

/* Print an unsigned value in decimal. Small enough to inline, and a program
   without it would have to reimplement it immediately. */
static inline void koi_print_dec(koi_uint64 value) {
    char buffer[21];
    int index = 20;

    buffer[index] = 0;
    do {
        buffer[--index] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value);
    koi_print(&buffer[index]);
}

#endif
