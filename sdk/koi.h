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

/* The environment, as the shell has it. Returns the length, or 0 when there is
 * no such variable - so `koi_getenv("PATH", ...)` both fetches it and answers
 * whether it is set.
 *
 * Read-only, deliberately: there is one environment because there is one
 * program running at a time, and a program that wrote it would be changing the
 * shell's for good. */
static inline long koi_getenv(const char* name, char* buffer, long size) {
    return koi_call(SYS_GETENV, (long)name, (long)buffer, size);
}

/* The name of the index-th variable, for listing them. Returns 0 when there
   are no more. */
static inline long koi_env_at(long index, char* name, long size) {
    return koi_call(SYS_ENVAT, index, (long)name, size);
}

static inline void koi_cls(void) {
    koi_call(SYS_CLS, 0, 0, 0);
}

/* Put the cursor somewhere, in columns and rows. The screen's size is
   KOI_INFO_TEXT_COLUMNS and KOI_INFO_TEXT_ROWS. What separates a program that
   prints from a program that has a screen. */
static inline void koi_gotoxy(int column, int row) {
    (void)koi_call(SYS_GOTOXY, KOI_POINT(column, row), 0, 0);
}

/* Hide the cursor while redrawing, so it does not race across the screen. */
static inline void koi_cursor(int visible) {
    (void)koi_call(SYS_CURSOR, visible, 0, 0);
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

/* Change which drive this program's paths mean.
 *
 * Affects this program only - the shell stays where it was, so exiting cannot
 * move the user's feet. The working directory returns to the root, because the
 * one it was in belonged to a different drive. Returns 1, or -1 when there is
 * no such drive. */
static inline int koi_setdrive(int letter) {
    return (int)koi_call(SYS_SETDRIVE, (long)letter, 0, 0);
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

/* Where this program's own files are.
 *
 * A package installs into a directory of its own and keeps its data there:
 * DOOM and its WAD, Mizu and its wallpaper. The current directory is the
 * user's - it is wherever they were standing when they typed the name - and
 * once a package is on the search path, that is somewhere else entirely. A
 * program that opens "DOOM.WAD" and expects to find its own copy is asking the
 * wrong directory, and one that opens "\\MIZU\\WALLPAPER.BMP" has guessed
 * where it was installed, which is the same mistake spelled confidently.
 *
 * So: ask. The kernel knows the path it loaded, and everything beside the
 * program is one join away from it.
 *
 * `koi_beside` writes the full path to a file that ships with the program.
 * Returns its length, or 0 when it will not fit. A program loaded from the
 * root gets the name back unchanged, which is correct there. */
static inline long koi_beside(const char* name, char* buffer, long size) {
    long cut = 0;
    long length = 0;

    if (!name || !buffer || size <= 0) return 0;
    if (koi_systext(KOI_TEXT_PROGRAM_PATH, 0, buffer, size) <= 0) buffer[0] = 0;

    /* Everything up to and including the last backslash is the directory. */
    for (long index = 0; buffer[index]; index++)
        if (buffer[index] == '\\') cut = index + 1;
    length = cut;

    for (long index = 0; name[index]; index++) {
        if (length + 1 >= size) { buffer[0] = 0; return 0; }
        buffer[length++] = name[index];
    }
    buffer[length] = 0;
    return length;
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

/* The same, with KOI_TEXT_BOLD, KOI_TEXT_ITALIC and KOI_TEXT_UNDERLINE - made
   from the glyphs the font already carries, so they cost no font data and go
   on working when a different font is fitted. */
static inline void koi_gfx_text_styled(int x, int y, const char* text,
                                       koi_uint32 color, long background,
                                       int style) {
    (void)koi_call4(SYS_GFX_TEXT_STYLED, KOI_POINT(x, y), (long)text,
                    (long)color,
                    (background & 0xFFFFFFFFL) |
                    ((long)(style & 0xFF) << 32));
}

static inline void koi_gfx_scissor(int x, int y, int width, int height) {
    (void)koi_call(SYS_GFX_SCISSOR, KOI_POINT(x, y), KOI_POINT(width, height), 0);
}

static inline void koi_gfx_reset_scissor(void) {
    (void)koi_call(SYS_GFX_SCISSOR_RESET, 0, 0, 0);
}

/* Run a command and come back when it has finished.
 *
 * This program stays in memory with everything it has; the one it starts gets
 * a slot of its own, and this returns when that program exits. `koi_chain` is
 * the other half of the pair and means the opposite: give up this program's
 * memory first. Use chain when the thing being started needs the room; use
 * this when you want to come back where you were.
 *
 * The screen belongs to whoever took it. A program drawing on the framebuffer
 * should koi_gfx_leave() before this and koi_gfx_enter() afterwards - nothing
 * here does it, because a program that only wants to run `dir` should not have
 * its window torn down to do it. */
static inline int koi_run(const char* command) {
    return (int)koi_call(SYS_RUN, (long)command, 0, 0);
}

/* ---- Running something else ----------------------------------------------
 *
 * A program cannot call another program: one runs at a time, at a fixed
 * address. It asks for one to be run after it has exited, when its own memory
 * is free. Requests run most-recent-first, so "run this and bring me back" is
 * two calls:
 *
 *     koi_chain("MIZU Z:\\GAMES");   asked for first, runs second
 *     koi_chain("DOOM");             asked for last, runs first
 *     koi_exit(0);
 *
 * The argument is a command line, so the search path, drive letters and
 * arguments behave as if typed. Coming back is a fresh start, not a resume:
 * save anything worth keeping before calling this. */
static inline int koi_chain(const char* command) {
    return (int)koi_call(SYS_CHAIN, (long)command, 0, 0);
}

/* ---- The log -------------------------------------------------------------
 *
 * The kernel's log, not a log of its own. Two logs of one run have to be
 * interleaved afterwards by somebody guessing at the order, and knowing the
 * order is the whole value of a log.
 *
 * koi_log_bytes is the important one: everything else a program can report is
 * what it believed, and this is what is actually there. Use it whenever a
 * check fails - the dump taken at the moment of failure is the one nobody can
 * take afterwards. */
static inline void koi_log(const char* text) {
    (void)koi_call(SYS_LOG, (long)text, 0, 0);
}

static inline void koi_log_bytes(const char* label, const void* data,
                                 long length) {
    (void)koi_call(SYS_LOG_BYTES, (long)label, (long)data, length);
}

/* One sector, straight off a disk. Read-only, and it grants nothing a program
   did not already have - programs run in ring 0 - it makes looking at the
   bytes a deliberate act rather than an accident. `buffer` must hold
   koi_sector_size(disk) bytes. Returns that size, or -1. */
static inline long koi_sector_read(long disk, koi_uint64 lba, void* buffer) {
    return koi_call(SYS_SECTOR_READ, disk, (long)lba, (long)buffer);
}

static inline long koi_sector_size(long disk) {
    return koi_call(SYS_SECTOR_SIZE, disk, 0, 0);
}

/* ---- The clipboard -------------------------------------------------------
 *
 * One buffer, in the kernel, outliving the program that filled it - which is
 * the only arrangement that can carry anything between two programs, and
 * therefore the only arrangement anybody wants. Text only.
 *
 * koi_clip_get with a null buffer returns the length without copying, so a
 * caller can find out how much room to make before making it. */
static inline long koi_clip_put(const char* text, long length) {
    return koi_call(SYS_CLIP_PUT, (long)text, length, 0);
}

static inline long koi_clip_get(char* buffer, long size) {
    return koi_call(SYS_CLIP_GET, (long)buffer, size, 0);
}

/* ---- The pointer ---------------------------------------------------------
 *
 * One snapshot, filled in one go. Returns 1 when there is a pointer, 0 when the
 * machine has none - and fills the structure in either case, so a program that
 * ignores the answer reads a still cursor rather than rubbish.
 *
 *     KOI_POINTER pointer;
 *     int last_scroll = 0;
 *     while (running) {
 *         koi_mouse(&pointer);
 *         int wheel = pointer.scroll - last_scroll;
 *         last_scroll = pointer.scroll;
 *         if (wheel) scroll_the_list_by(wheel);
 *     }
 *
 * `scroll` is a running total, positive upwards, so subtract what you last saw.
 * On a laptop it is two fingers on the touchpad: the pad turns the gesture into
 * wheel notches itself, and nothing here has to know it was fingers. */
static inline int koi_mouse(KOI_POINTER* pointer) {
    return (int)koi_call(SYS_MOUSE, (long)pointer, 0, 0);
}

/* Put the pointer somewhere - what to do on entering graphics mode, so that it
   starts in the middle rather than wherever the last program left it. */
static inline int koi_mouse_place(int x, int y) {
    return (int)koi_call(SYS_MOUSE_PLACE, KOI_POINT(x, y), 0, 0);
}

/* ---- Sound ---------------------------------------------------------------
 *
 * There is nothing to open. One stream of 48 kHz stereo is always running and
 * these put things into it; a call returns a voice, or -1 when every voice is
 * busy or the machine has no sound hardware. Nothing here blocks - a sound
 * plays while the program gets on with something else, and koi_sound_active
 * is how to find out whether it has finished.
 *
 * The samples are not copied. Keep the buffer where it is until the sound has
 * finished, which is what makes firing the same effect twenty times a second
 * cost nothing.
 */
static inline int koi_sound_play(const KOI_SOUND* sound) {
    return (int)koi_call(SYS_SOUND_PLAY, (long)sound, 0, 0);
}

/* The one-line version, for a sound with no panning and no looping. */
static inline int koi_sound_play_simple(const void* samples,
                                        unsigned int frames,
                                        unsigned int rate, int bits,
                                        int channels, int volume) {
    KOI_SOUND sound;
    sound.samples = samples;
    sound.frames = frames;
    sound.rate = rate;
    sound.bits = (unsigned short)bits;
    sound.channels = (unsigned short)channels;
    sound.volume = (unsigned short)volume;
    sound.pan = 128;
    sound.loop = 0;
    sound.reserved = 0;
    return koi_sound_play(&sound);
}

static inline int koi_sound_tone(unsigned int hertz,
                                 unsigned int milliseconds, int volume) {
    return (int)koi_call(SYS_SOUND_TONE, (long)hertz, (long)milliseconds,
                         (long)volume);
}

/* -1 stops everything, which is what to do before returning if the program
   did not keep a list of what it started. */
static inline void koi_sound_stop(int voice) {
    (void)koi_call(SYS_SOUND_STOP, (long)voice, 0, 0);
}

/* Change a sound that is already playing - what a moving source needs. Pass
   -1 for either to leave it alone. Returns -1 once the sound has finished,
   which is an answer rather than a failure. */
static inline int koi_sound_params(int voice, int volume, int pan) {
    return (int)koi_call(SYS_SOUND_PARAMS, (long)voice, (long)volume,
                         (long)pan);
}

static inline int koi_sound_active(int voice) {
    return (int)koi_call(SYS_SOUND_ACTIVE, (long)voice, 0, 0);
}

/* 0 to 255, applied to everything. Pass -1 to ask without changing it. */
/* Where a sound has got to, how long it is, and how to move the first - all in
   source frames, so frames / rate is seconds. A progress bar needs exactly
   these three. */
static inline unsigned int koi_sound_where(int voice) {
    return (unsigned int)koi_call(SYS_SOUND_WHERE, voice, 0, 0);
}

static inline unsigned int koi_sound_length(int voice) {
    return (unsigned int)koi_call(SYS_SOUND_LENGTH, voice, 0, 0);
}

static inline int koi_sound_seek(int voice, unsigned int frame) {
    return (int)koi_call(SYS_SOUND_SEEK, voice, (long)frame, 0);
}

static inline int koi_sound_volume(int volume) {
    return (int)koi_call(SYS_SOUND_VOLUME, (long)volume, 0, 0);
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
