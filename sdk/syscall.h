#ifndef SYSCALL_H
#define SYSCALL_H

/* The Koi-DOS system call interface.
 *
 * Shared verbatim between the kernel and the programs it runs, so that the two
 * cannot drift apart.
 *
 * Calls are made with a software interrupt, the way INT 21h worked in DOS. The
 * vector is 0x40 rather than 0x21 because in protected mode 0x21 is taken: the
 * 8259s are remapped to vectors 32-47, which puts the keyboard IRQ exactly
 * there. DOS never had that collision - it lived in real mode.
 *
 * A software interrupt rather than SYSCALL/SYSRET is deliberate. Koi-DOS is a
 * ring-0 monolith, and the whole value of SYSRET is a fast ring 3 to ring 0
 * transition that does not happen here. INT costs three MSRs less setup and
 * gives an ABI that does not depend on where the kernel is linked.
 *
 * Convention:
 *   RAX  function number
 *   RDI, RSI, RDX, RCX  arguments, in that order
 *   RAX  return value
 * Every other register is preserved.
 */
#define SYSCALL_VECTOR 0x40

/* Major in the high byte, minor in the low one. Reported by SYS_VERSION and
   printed by `ver`. */
#define KOI_DOS_VERSION 0x0005

/* The interface's own version, which moves independently of the system's.
 *
 * A program records the version it was built against, and the kernel refuses
 * to start one it cannot honour. That check runs in both directions, and the
 * second one is the surprising half:
 *
 *   - A program built for a NEWER interface would call functions this kernel
 *     does not have. Obvious, and the usual worry.
 *   - A program built for an OLDER interface is refused too, while the
 *     interface is still ALPHA - because function numbers may have been
 *     reused since, and a program calling a number that has changed meaning
 *     does not fail. It does the wrong thing, silently, which is far worse
 *     than not starting.
 *
 * KOI_ABI_MINIMUM is what makes that a decision rather than a rule. It equals
 * KOI_ABI_VERSION for now; the day the numbering is frozen it stops moving,
 * and every program built from that day on keeps working forever.
 *
 * ONCE THE INTERFACE IS FROZEN, FUNCTION NUMBERS ARE NEVER REUSED. A removed
 * call leaves a hole. This is the promise that makes old programs safe, and it
 * costs nothing to keep - there are 256 numbers and twenty are in use. */
#define KOI_ABI_VERSION 2
#define KOI_ABI_MINIMUM 2
#define KOI_ABI_IS_ALPHA 1

/* Every program begins with this, placed at its load address by the linker
   script, so the kernel can read it before deciding to run anything. */
#define KOI_PROGRAM_MAGIC 0x21494F4BU     /* "KOI!" in memory order */

typedef struct {
    unsigned int magic;
    unsigned int abi_version;
    unsigned int reserved[2];
} KOI_PROGRAM_HEADER;

/* Console and process. */
#define SYS_EXIT 0x00        /* (code) - does not return */
#define SYS_PUTCHAR 0x01     /* (character) */
#define SYS_PUTS 0x02        /* (text) */
#define SYS_GETCHAR 0x03     /* () -> key, blocking */
#define SYS_READLINE 0x04    /* (buffer, size) -> length */
#define SYS_CLS 0x05         /* () */
#define SYS_SETCOLOR 0x06    /* (foreground, background) */
/* Replace the shell's own colours. Any argument outside 0-15 leaves that one
   as it was, so a program can change one colour without knowing the others.
   Returns the resulting theme packed as
   foreground | background << 8 | prompt << 16 | error << 24 - which is what
   lets a caller write the whole theme to a file after changing part of it.
   Persisting is the caller's job; the kernel only reads the file at boot. */
#define SYS_SETTHEME 0x07    /* (foreground, background, prompt, error) */

#define KOI_THEME_FOREGROUND(packed) ((int)((packed) & 0xFF))
#define KOI_THEME_BACKGROUND(packed) ((int)(((packed) >> 8) & 0xFF))
#define KOI_THEME_PROMPT(packed) ((int)(((packed) >> 16) & 0xFF))
#define KOI_THEME_ERROR(packed) ((int)(((packed) >> 24) & 0xFF))

/* Files. Handles are small non-negative integers; -1 means failure. */
#define SYS_OPEN 0x10        /* (path, mode) -> handle */
#define SYS_CLOSE 0x11       /* (handle) */
#define SYS_READ 0x12        /* (handle, buffer, length) -> bytes read */
#define SYS_WRITE 0x13       /* (handle, buffer, length) -> bytes written */
#define SYS_SIZE 0x14        /* (handle) -> bytes */

/* Directory enumeration. Without these a program cannot write its own `dir`,
   which makes the shell's built-in the only way to see a directory. */
#define SYS_FINDFIRST 0x18   /* (pattern, KOI_FIND_DATA*) -> search, or -1 */
#define SYS_FINDNEXT 0x19    /* (search, KOI_FIND_DATA*) -> 0, or -1 at the end */
#define SYS_FINDCLOSE 0x1A   /* (search) */

/* File attributes, as they sit in a FAT directory entry. */
#define KOI_ATTRIBUTE_READ_ONLY 0x01
#define KOI_ATTRIBUTE_HIDDEN 0x02
#define KOI_ATTRIBUTE_SYSTEM 0x04
#define KOI_ATTRIBUTE_DIRECTORY 0x10
#define KOI_ATTRIBUTE_ARCHIVE 0x20

#define KOI_NAME_MAX 256

/* What a search returns. Laid out identically for the kernel and for programs,
   because both sides include this file. */
typedef struct {
    char name[KOI_NAME_MAX];
    unsigned int attributes;
    unsigned int size;
    unsigned short date;   /* year-1980 << 9 | month << 5 | day */
    unsigned short time;   /* hour << 11 | minute << 5 | second/2 */
} KOI_FIND_DATA;

/* Environment. */
#define SYS_ARGS 0x20        /* () -> pointer to the command line tail */
#define SYS_VERSION 0x21     /* () -> version, major in the high byte */

/* What the system knows about itself.
 *
 * Two calls rather than a dozen, on purpose. Every new thing worth reporting -
 * a temperature, a battery, a second screen - would otherwise be another
 * function number, and an ABI that grows a hole every time the kernel learns
 * something is an ABI nobody can rely on. One numeric call and one text call,
 * both selected by an item and an index, cover all of it.
 *
 * An unknown item returns -1 rather than zero, so a program built against a
 * newer header can tell "this kernel does not know" from "the answer is none". */
#define SYS_SYSINFO 0x22     /* (item, index) -> value, or -1 */
#define SYS_SYSTEXT 0x23     /* (item, index, buffer, size) -> length, or -1 */

/* Numeric items. Sizes are in kibibytes unless said otherwise. */
#define KOI_INFO_MEMORY_TOTAL 0
#define KOI_INFO_MEMORY_FREE 1
#define KOI_INFO_KERNEL_SIZE 2
#define KOI_INFO_HEAP_TOTAL 3
#define KOI_INFO_HEAP_FREE 4
#define KOI_INFO_UPTIME_MS 5
#define KOI_INFO_BUILD_NUMBER 6
#define KOI_INFO_SCREEN_WIDTH 7      /* pixels */
#define KOI_INFO_SCREEN_HEIGHT 8
#define KOI_INFO_TEXT_COLUMNS 9      /* characters */
#define KOI_INFO_TEXT_ROWS 10
#define KOI_INFO_PCI_DEVICES 11
#define KOI_INFO_DISK_COUNT 12
#define KOI_INFO_VOLUME_COUNT 13
#define KOI_INFO_USB_PORTS 14
#define KOI_INFO_USB_PORTS_USED 15
#define KOI_INFO_TIMER_HZ 16
#define KOI_INFO_TIMER_IS_INTERRUPT 17
/* These take an index: which disk, which volume. */
#define KOI_INFO_DISK_SECTORS 18
#define KOI_INFO_DISK_SECTOR_SIZE 19
#define KOI_INFO_VOLUME_LETTER 20    /* the drive letter, as a character */
#define KOI_INFO_VOLUME_IS_BOOT 21

/* Text items, written into the caller's buffer and always terminated. */
#define KOI_TEXT_BUILD_DATE 0
#define KOI_TEXT_BUILD_COMMIT 1
#define KOI_TEXT_DISK_NAME 2         /* index selects the disk */
#define KOI_TEXT_VOLUME_LABEL 3      /* index selects the volume */

/* Graphics.
 *
 * A program takes the screen, draws, shows the result, and gives the screen
 * back. Between the taking and the giving back the console is not on display -
 * but it has not lost anything either, and leaving restores it exactly.
 *
 * Nothing appears until SYS_GFX_PRESENT. That is not an optimisation, it is
 * the difference between an image and a program being watched as it draws one.
 *
 * SYS_GFX_ENTER fills in a KOI_SCREEN, which carries a pointer to the buffer
 * being drawn into. A program may write to it directly - this is a ring-0
 * system with no memory protection and pretending otherwise would only make
 * drawing slow. The primitives are here so that a program does not have to. */
#define SYS_GFX_ENTER 0x30   /* (KOI_SCREEN*) -> 0, or -1 */
#define SYS_GFX_LEAVE 0x31   /* () */
#define SYS_GFX_PRESENT 0x32 /* () */
#define SYS_GFX_COLOR 0x33   /* (red, green, blue) -> packed pixel */
#define SYS_GFX_CLEAR 0x34   /* (colour) */
#define SYS_GFX_PIXEL 0x35   /* (point, colour) */
#define SYS_GFX_LINE 0x36    /* (point, point, colour) */
#define SYS_GFX_RECT 0x37    /* (point, size, colour) - outline */
#define SYS_GFX_FILL 0x38    /* (point, size, colour) - solid */
#define SYS_GFX_TEXT 0x39    /* (point, text, colour, background) */

/* Two coordinates in one argument.
 *
 * The call convention carries four arguments, and a line needs five numbers.
 * Rather than widen the convention for one call - which every other call would
 * then have to keep working around - a point travels as a pair packed into one
 * 64-bit word. The wrappers in the SDK hide it; a program written against them
 * never sees this. */
#define KOI_POINT(x, y) \
    ((long)((((unsigned long)(unsigned int)(x)) << 32) | \
            ((unsigned long)(unsigned int)(y))))
#define KOI_POINT_X(packed) ((int)((unsigned long)(packed) >> 32))
#define KOI_POINT_Y(packed) ((int)((unsigned long)(packed) & 0xFFFFFFFFUL))

/* Passing the background colour has no meaning when the text is drawn over
   whatever is already there; this says so. */
#define KOI_TEXT_TRANSPARENT (-1)

typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;            /* bytes between the starts of two rows */
    unsigned int bytes_per_pixel;
    void* pixels;
} KOI_SCREEN;

#define OPEN_READ 0
#define OPEN_WRITE 1         /* creates, or truncates an existing file */

#define SYSCALL_ERROR ((long)-1)

#endif
