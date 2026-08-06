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

#define OPEN_READ 0
#define OPEN_WRITE 1         /* creates, or truncates an existing file */

#define SYSCALL_ERROR ((long)-1)

#endif
