#ifndef KOI_H
#define KOI_H

/* The Koi-DOS program interface.
 *
 * Everything here is a thin wrapper over the software interrupt described in
 * include/syscall.h. There is no C library: a program gets these calls, and
 * whatever it writes itself. */

#include "syscall.h"

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

/* The same, with the fourth argument the ABI defines. Separate because most
   calls need three and the extra register constraint costs nothing to omit. */
static inline long koi_call4(long function, long a, long b, long c, long d) {
    long result;
    __asm__ volatile ("int %1"
                      : "=a"(result)
                      : "i"(SYSCALL_VECTOR), "a"(function),
                        "D"(a), "S"(b), "d"(c), "c"(d)
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

/* Is a key waiting? Does not take it - koi_getchar still does that.
 *
 * This is what makes a game possible: koi_getchar stops everything until
 * somebody presses something, which is right for a prompt and wrong for
 * anything that has to keep moving. */
static inline int koi_keypressed(void) {
    return (int)koi_call(SYS_KEYPRESSED, 0, 0, 0);
}

/* One key going down or coming up, rather than a character.
 *
 * A character stream cannot say a key is being *held* - it has no idea a key
 * is still down and no idea when it stopped being. Anything where that matters
 * needs this: walking forward, steering, holding a button.
 *
 *     int event = koi_keyevent();
 *     if (event) {
 *         int key = KOI_KEY_CODE(event);
 *         if (KOI_KEY_IS_RELEASE(event)) ... else ...
 *     }
 *
 * The identity is the unshifted one, so a key reads the same both ways.
 * Returns 0 when nothing has happened. Draining this consumes no characters,
 * so a program may use koi_getchar as well. */
static inline int koi_keyevent(void) {
    return (int)koi_call(SYS_KEYEVENT, 0, 0, 0);
}

/* Memory beyond what the program image holds, in whole pages.
 *
 * Not a malloc and not meant to be one. A program that wants small objects
 * takes one large block and divides it itself - which is what every program
 * large enough to care already does, and what DOOM's zone allocator is.
 * Everything is released when the program exits, remembered or not. */
static inline void* koi_alloc(long bytes) {
    return (void*)koi_call(SYS_ALLOC, bytes, 0, 0);
}

static inline void koi_free(void* address) {
    (void)koi_call(SYS_FREE, (long)address, 0, 0);
}

/* Wait, without spinning. Keystrokes arriving meanwhile are buffered and are
   still there when this returns. */
static inline void koi_sleep(long milliseconds) {
    (void)koi_call(SYS_SLEEP, milliseconds, 0, 0);
}

/* Milliseconds since the system started. The clock a game measures itself
   against; it never goes backwards and never stops. */
static inline koi_uint64 koi_uptime(void) {
    return (koi_uint64)koi_call(SYS_SYSINFO, KOI_INFO_UPTIME_MS, 0, 0);
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

/* Move the read/write position. Returns where it ended up, or -1.
   Anything with an index in it needs this - a WAD is a directory of offsets. */
static inline long koi_seek(long handle, long offset, long whence) {
    return koi_call(SYS_SEEK, handle, whence, offset);
}

/* Delete, rename, and ask whether a path is there. A program that can create
   files and never remove them fills the disk and cannot tidy up after itself. */
static inline long koi_remove(const char* path) {
    return koi_call(SYS_REMOVE, (long)path, 0, 0);
}

static inline long koi_rename(const char* from, const char* to) {
    return koi_call(SYS_RENAME, (long)from, (long)to, 0);
}

/* Make a directory. An installed package keeps its own rather than emptying
   itself into \BIN, and a relative path resolves from where the shell is
   standing - so a program's data sits next to it. */
static inline long koi_mkdir(const char* path) {
    return koi_call(SYS_MKDIR, (long)path, 0, 0);
}

static inline int koi_exists(const char* path) {
    return (int)koi_call(SYS_EXISTS, (long)path, 0, 0);
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


/* What the system knows about itself. Two calls rather than a dozen: every new
   thing worth reporting would otherwise be another function number. An unknown
   item returns -1, so a program built against a newer header can tell "this
   kernel does not know" from "the answer is none". */
static inline long koi_sysinfo(long item, long index) {
    return koi_call(SYS_SYSINFO, item, index, 0);
}

static inline long koi_systext(long item, long index, char* buffer, long size) {
    return koi_call4(SYS_SYSTEXT, item, index, (long)buffer, size);
}

/* ---- Graphics ------------------------------------------------------------
 *
 * The shape of a graphics program:
 *
 *     KOI_SCREEN screen;
 *     if (koi_gfx_enter(&screen) != 0) return 1;
 *     koi_gfx_clear(koi_gfx_color(0, 0, 40));
 *     koi_gfx_fill(10, 10, 100, 60, koi_gfx_color(255, 200, 0));
 *     koi_gfx_present();
 *     koi_getchar();
 *     koi_gfx_leave();
 *
 * Nothing is on screen until koi_gfx_present. Always leave before returning -
 * the shell is not visible until you do.
 *
 * screen.pixels is the buffer itself. Writing to it directly is allowed and is
 * the fast path; the calls below exist so that a program does not have to care
 * how a pixel is laid out. */
static inline int koi_gfx_enter(KOI_SCREEN* screen) {
    return (int)koi_call(SYS_GFX_ENTER, (long)screen, 0, 0);
}

static inline void koi_gfx_leave(void) {
    (void)koi_call(SYS_GFX_LEAVE, 0, 0, 0);
}

static inline void koi_gfx_present(void) {
    (void)koi_call(SYS_GFX_PRESENT, 0, 0, 0);
}

/* Show only the part that changed. Anything that redraws continuously wants
   this: the screen is usually much larger than the area a program uses, and
   sending all of it every frame costs more than drawing does. */
static inline void koi_gfx_present_rect(int x, int y, int width, int height) {
    (void)koi_call(SYS_GFX_PRESENT_RECT, KOI_POINT(x, y),
                   KOI_POINT(width, height), 0);
}

/* Build a pixel for whatever channel order this machine's framebuffer uses.
   Never assemble one by hand: the order differs between machines, and code
   that guesses draws in the wrong colours on half of them. */
static inline koi_uint32 koi_gfx_color(int red, int green, int blue) {
    return (koi_uint32)koi_call(SYS_GFX_COLOR, red, green, blue);
}

static inline void koi_gfx_clear(koi_uint32 color) {
    (void)koi_call(SYS_GFX_CLEAR, (long)color, 0, 0);
}

static inline void koi_gfx_pixel(int x, int y, koi_uint32 color) {
    (void)koi_call(SYS_GFX_PIXEL, KOI_POINT(x, y), (long)color, 0);
}

static inline void koi_gfx_line(int x0, int y0, int x1, int y1,
                                koi_uint32 color) {
    (void)koi_call(SYS_GFX_LINE, KOI_POINT(x0, y0), KOI_POINT(x1, y1),
                   (long)color);
}

static inline void koi_gfx_rect(int x, int y, int width, int height,
                                koi_uint32 color) {
    (void)koi_call(SYS_GFX_RECT, KOI_POINT(x, y), KOI_POINT(width, height),
                   (long)color);
}

static inline void koi_gfx_fill(int x, int y, int width, int height,
                                koi_uint32 color) {
    (void)koi_call(SYS_GFX_FILL, KOI_POINT(x, y), KOI_POINT(width, height),
                   (long)color);
}

/* Text in the system font at pixel coordinates. Pass KOI_TEXT_TRANSPARENT as
   the background to draw only the lit pixels over whatever is already there. */
static inline void koi_gfx_text(int x, int y, const char* text,
                                koi_uint32 color, long background) {
    (void)koi_call4(SYS_GFX_TEXT, KOI_POINT(x, y), (long)text, (long)color,
                    background);
}

/* ---- The parts of a C library a program cannot do without ---------------
 *
 * Defined in koilib.c, which koicc compiles in alongside start.c. Not a C
 * library and not trying to be one: this is the subset that either the
 * compiler requires or that any program larger than a page rewrites badly on
 * its own.
 *
 * The memory four are not optional. GCC emits calls to them of its own accord
 * even with -ffreestanding - a struct assignment is enough - so a program
 * without them fails to link for reasons unrelated to anything it wrote. */
void* memset(void* destination, int value, koi_uint64 count);
void* memcpy(void* destination, const void* source, koi_uint64 count);
void* memmove(void* destination, const void* source, koi_uint64 count);
int memcmp(const void* left, const void* right, koi_uint64 count);

koi_uint64 strlen(const char* text);
char* strcpy(char* destination, const char* source);
char* strncpy(char* destination, const char* source, koi_uint64 count);
char* strcat(char* destination, const char* source);
int strcmp(const char* left, const char* right);
int strncmp(const char* left, const char* right, koi_uint64 count);
char* strchr(const char* text, int character);
char* strrchr(const char* text, int character);
char* strstr(const char* haystack, const char* needle);

int isdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

int abs(int value);
long strtol(const char* text, char** end, int base);
int atoi(const char* text);

/* One formatter, which the rest call. The conversions are d, i, u, x, X, c, s,
   p and %%, with width, left alignment and zero padding. There is no %f: this
   system has no floating point, so a program cannot have produced a value to
   print. */
int koi_vformat(char* out, koi_uint64 size, const char* format,
                __builtin_va_list arguments);
int koi_snprintf(char* out, koi_uint64 size, const char* format, ...);
int koi_sprintf(char* out, const char* format, ...);
int koi_printf(const char* format, ...);

/* A heap, dividing up the whole pages the system hands out. First fit with
   adjacent free blocks merged, which is the simplest arrangement that does not
   fragment itself to death - and is what DOS's own allocator did. */
void* malloc(koi_uint64 size);
void* calloc(koi_uint64 count, koi_uint64 size);
void* realloc(void* address, koi_uint64 size);
void free(void* address);

/* The processor's own name for itself.
 *
 * No system call for this on purpose: programs run in ring 0, so a program can
 * simply ask the processor. `buffer` needs 49 bytes. Returns 0 when the
 * processor does not carry a brand string, which no x86-64 part omits. */
static inline int koi_cpu_name(char* buffer) {
    unsigned int registers[4];
    unsigned int highest;
    int position = 0;

    __asm__ volatile ("cpuid"
                      : "=a"(highest), "=b"(registers[1]),
                        "=c"(registers[2]), "=d"(registers[3])
                      : "a"(0x80000000U));
    if (highest < 0x80000004U) { buffer[0] = 0; return 0; }

    for (unsigned int leaf = 0x80000002U; leaf <= 0x80000004U; leaf++) {
        __asm__ volatile ("cpuid"
                          : "=a"(registers[0]), "=b"(registers[1]),
                            "=c"(registers[2]), "=d"(registers[3])
                          : "a"(leaf));
        for (int word = 0; word < 4; word++)
            for (int byte = 0; byte < 4; byte++)
                buffer[position++] =
                    (char)((registers[word] >> (byte * 8)) & 0xFF);
    }
    buffer[position] = 0;

    /* The brand string is padded with leading spaces on many parts. */
    {
        int start = 0;
        while (buffer[start] == ' ') start++;
        if (start) {
            int index = 0;
            while (buffer[start + index]) {
                buffer[index] = buffer[start + index];
                index++;
            }
            buffer[index] = 0;
        }
    }
    return 1;
}

#endif
