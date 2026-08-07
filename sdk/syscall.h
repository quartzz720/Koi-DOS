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
 *
 * Every other register is preserved - which the stub now actually does. It did
 * not always: the dispatcher is a C function and may clobber RCX, RDX, RSI,
 * RDI and R8-R11, and none of them were being saved. Nothing noticed until a
 * program compiled at -O2 kept a loop pointer in R8 across a call.
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
#define KOI_ABI_VERSION 8
#define KOI_ABI_MINIMUM 8
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
/* Is a key waiting, without taking it? The counterpart to SYS_GETCHAR, and
   the only way to write anything that has to keep moving while nobody is
   pressing anything - a game, an animation, an interruptible loop. */
#define SYS_KEYPRESSED 0x08  /* () -> 1 when a keystroke is ready, else 0 */
/* Wait, without spinning. A loop around SYS_SYSINFO's uptime would give the
   same delay while keeping a core busy for the whole of it; this parks the
   processor between ticks. Any keystroke arriving meanwhile is still buffered
   and still there afterwards. */
#define SYS_SLEEP 0x09       /* (milliseconds) */
/* One key going down or coming up, rather than a character.
 *
 * SYS_GETCHAR answers "what did they type", which is what a prompt wants and
 * is the wrong question for anything where a key is *held*: walking forward,
 * steering, holding a button. A character stream cannot express that at all -
 * it has no idea a key is still down, and no idea when it stopped being.
 *
 * The identity reported is the unshifted one, so a key gives the same value
 * going down as coming up. `w` is 'w' whether or not shift was held; the
 * arrows and modifiers are the KOI_KEY_* codes below. Returns 0 when nothing
 * has happened. Reading these does not consume characters, so a program may
 * use both. */
#define SYS_KEYEVENT 0x0A    /* () -> key, or key | KOI_KEY_RELEASED, or 0 */

/* Keys with no ASCII value, returned by SYS_GETCHAR above 0xFF so a caller can
   switch on them alongside ordinary characters.
 *
 * These live here rather than in the kernel's own header because a program
 * that reads the arrow keys needs to name them, and two copies of a number is
 * how two copies of a number drift apart. The kernel takes its names from
 * these. */
#define KOI_KEY_UP 0x100
#define KOI_KEY_DOWN 0x101
#define KOI_KEY_LEFT 0x102
#define KOI_KEY_RIGHT 0x103
#define KOI_KEY_HOME 0x104
#define KOI_KEY_END 0x105
#define KOI_KEY_PAGE_UP 0x106
#define KOI_KEY_PAGE_DOWN 0x107
#define KOI_KEY_DELETE 0x108
#define KOI_KEY_INSERT 0x109
#define KOI_KEY_SHIFT 0x10A
#define KOI_KEY_CONTROL 0x10B
#define KOI_KEY_ALT 0x10C
#define KOI_KEY_F1 0x110     /* F1..F12 are consecutive from here */

/* Set in a SYS_KEYEVENT result when the key came up rather than went down. */
#define KOI_KEY_RELEASED 0x8000
#define KOI_KEY_CODE(event) ((int)((event) & 0x7FFF))
#define KOI_KEY_IS_RELEASE(event) (((event) & KOI_KEY_RELEASED) != 0)

#define KOI_KEY_ESCAPE 27
#define KOI_KEY_ENTER '\n'
#define KOI_KEY_BACKSPACE '\b'
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
/* Move the read/write position.
 *
 * Reading a file front to back is the easy case and was the only one for a
 * while. Anything with an index in it needs the other: a WAD is a directory of
 * offsets and every lump read starts by jumping to one, so without this the
 * file can be read but not used. */
#define SYS_SEEK 0x15        /* (handle, offset, whence) -> position, or -1 */

#define KOI_SEEK_SET 0       /* from the beginning */
#define KOI_SEEK_CURRENT 1   /* from where it is now */
#define KOI_SEEK_END 2       /* from the end, offset usually negative */

/* Deleting and renaming. The filesystem has always been able to do both; the
   shell's `del` and `ren` are these calls' older siblings. A program that can
   create files and never remove them fills the disk and cannot tidy up after
   itself - a saved game it replaces, a temporary file it made. */
#define SYS_REMOVE 0x16      /* (path) -> 0, or -1 */
#define SYS_RENAME 0x17      /* (from, to) -> 0, or -1 */
/* Does this path exist? Cheaper to ask than to open, and the answer to "may I
   overwrite this" without the side effect of creating it. */
#define SYS_EXISTS 0x1B      /* (path) -> 1, 0, or -1 when there is no volume */
/* Make a directory. An installed package keeps its own, rather than emptying
   itself into \BIN alongside the system's own programs - which is also how a
   program finds its data, since a relative path resolves from where the shell
   is standing. A package manager that cannot create a directory cannot do
   that. */
#define SYS_MKDIR 0x1C       /* (path) -> 0, or -1 */

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

/* Memory, for programs that need more than their own image holds.
 *
 * Whole pages, because that is what the kernel has to give. This is not a
 * malloc and is not meant to be one: a program that wants small objects takes
 * one large block and manages it itself, which is what every program large
 * enough to care already does.
 *
 * Everything a program took is released when it exits, whether or not it
 * remembered to - a leak that outlives the program would be permanent, since
 * nothing here reclaims memory later. */
#define SYS_ALLOC 0x24       /* (bytes) -> address, or 0 */
#define SYS_FREE 0x25        /* (address) */

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
/* Whether there is anything to play sound through. A program that makes noise
   has to be able to ask, because every sound call succeeding into silence and
   every one failing look identical from the inside. */
#define KOI_INFO_AUDIO 22
#define KOI_INFO_AUDIO_RATE 23

/* Text items, written into the caller's buffer and always terminated. */
#define KOI_TEXT_BUILD_DATE 0
#define KOI_TEXT_BUILD_COMMIT 1
#define KOI_TEXT_DISK_NAME 2         /* index selects the disk */
#define KOI_TEXT_VOLUME_LABEL 3      /* index selects the volume */
#define KOI_TEXT_AUDIO_DEVICE 4      /* the codec, or "none" */

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
/* Show one rectangle rather than the whole screen.
 *
 * The screen is whatever size the firmware chose, often far larger than the
 * area a program uses, and sending all of it sixty times a second costs more
 * than everything else the program does put together. Coordinates are clipped,
 * so a caller that knows what it changed need not also know where the edges
 * are. */
#define SYS_GFX_PRESENT_RECT 0x3A  /* (point, size) */

/* ---- Sound ---------------------------------------------------------------
 *
 * One stream of 48 kHz stereo is always running; these put things into it.
 * There is no call to open or close a device, because there is nothing to
 * open: a program asks for a sound and gets a voice back, or -1 when every
 * voice is busy or the machine has no sound hardware.
 *
 * A sound's samples are NOT copied. The buffer must stay where it is until the
 * sound finishes or is stopped - which is what makes firing a sound effect
 * free rather than a copy of it per shot. Every voice is stopped when the
 * program exits, so a program that forgets cannot leave the mixer reading
 * memory that has been handed to something else.
 */
#define SYS_SOUND_PLAY 0x40    /* (KOI_SOUND*) -> voice, or -1 */
#define SYS_SOUND_TONE 0x41    /* (hertz, milliseconds, volume) -> voice, -1 */
#define SYS_SOUND_STOP 0x42    /* (voice), or -1 for all of them */
#define SYS_SOUND_ACTIVE 0x43  /* (voice) -> 1 while it is still playing */
#define SYS_SOUND_VOLUME 0x44  /* (volume 0-255, or -1 to ask) -> volume */

/* Change a sound that is already playing. What a moving source needs: DOOM
   calls this every tic for every sound whose direction or distance from the
   player has changed, and without it a rocket that flies past stays where it
   was fired. Pass -1 for either to leave it alone. */
#define SYS_SOUND_PARAMS 0x45  /* (voice, volume, pan) -> 0, or -1 */

#define KOI_SOUND_U8 8         /* unsigned bytes, 0x80 is silence */
#define KOI_SOUND_S16 16       /* signed 16-bit, little endian */

/* What a sound is. Eight things do not fit in four registers, and a
   descriptor is clearer than packing them two to a word. */
typedef struct {
    const void* samples;
    unsigned int frames;       /* not bytes: a stereo frame is two samples */
    unsigned int rate;         /* what it was recorded at; resampled for you */
    unsigned short bits;       /* KOI_SOUND_U8 or KOI_SOUND_S16 */
    unsigned short channels;   /* 1 or 2 */
    unsigned short volume;     /* 0-255 */
    unsigned short pan;        /* 0 left, 128 centre, 255 right */
    unsigned int loop;         /* non-zero to repeat until stopped */
    unsigned int reserved;
} KOI_SOUND;

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
