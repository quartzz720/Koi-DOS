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
   printed by `ver`.
 *
 * 0.51 rather than 0.6: the jump from 0.5 is not a jump in what the system is,
 * it is the point at which it stopped needing a USB stick to be changed. A
 * machine that updates itself over the network is a different thing to live
 * with than one that does not, and that is what beta means here. */
#define KOI_DOS_VERSION 0x0033

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
/* 9 adds the pointer (SYS_MOUSE, SYS_MOUSE_PLACE) and takes nothing away.
 *
 * The minimum stays at 8 for the first time, which is a decision rather than an
 * oversight. The rule above exists because a REUSED function number does the
 * wrong thing silently; 9 reuses none - it only fills two of the holes. And the
 * cost of moving the minimum is now real: a system update ships the kernel and
 * nothing else, so raising it would refuse every program already installed on a
 * machine until each was reinstalled. A rule worth keeping is worth keeping for
 * its reason, and the reason does not apply here. */
#define KOI_ABI_VERSION 14
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

/* The exit code of a program that was stopped with Ctrl+C rather than one that
 * finished. 130 is what the unix world uses for it - 128 plus the signal
 * number - and borrowing a number somebody else has already agreed on beats
 * inventing one that only means something here.
 *
 * A program never returns this itself; the kernel supplies it. A batch file
 * can therefore tell "the command failed" from "somebody stopped it", which
 * are different things to carry on from. */
#define KOI_EXIT_INTERRUPTED 130

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
/* Put the cursor somewhere, and show or hide it.
 *
 * What separates a program that prints from a program that has a screen. An
 * editor without this has to clear and reprint everything to move one
 * character - thousands of calls per keystroke, and a visible flicker on every
 * one of them. DOS programs reached for the BIOS or ANSI codes; this is the
 * same thing without the detour. Columns and rows, not pixels: the sizes are
 * KOI_INFO_TEXT_COLUMNS and KOI_INFO_TEXT_ROWS. */
#define SYS_GOTOXY 0x0B      /* (point: column, row) -> 0 */
#define SYS_CURSOR 0x0C      /* (visible) -> 0 */

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
/* Change which drive this program's paths are resolved against.
 *
 * A program starts on the drive the shell was standing on and, before this,
 * could never leave it. Which is fine for a program given a filename and fatal
 * for a file manager: the only way to reach another drive was to ask the shell
 * to change drive and restart - and a program that does that is asking to be
 * restarted from a drive it is no longer on. Mizu did exactly that, changed to
 * the USB stick, and could not find itself.
 *
 * Affects this program only. The shell stays where it was, and the next program
 * to run starts where the user is standing, so a program cannot move the user's
 * feet by exiting. The working directory returns to the root, because the one
 * it was in belonged to a different drive. */
#define SYS_SETDRIVE 0x1D    /* (drive letter) -> 1, or -1 when there is none */

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
/* Which volume the program's own paths are resolved against - the drive the
   shell was standing on. A program can enumerate the drives and could not,
   before this, tell which one it was on. */
#define KOI_INFO_VOLUME_IS_CURRENT 24
/* Whether there is anything to play sound through. A program that makes noise
   has to be able to ask, because every sound call succeeding into silence and
   every one failing look identical from the inside. */
#define KOI_INFO_AUDIO 22
#define KOI_INFO_AUDIO_RATE 23
/* The wall clock, packed so that one call is one moment.
 *
 * Not an hour call and a minute call. The clock moves between them, and a
 * program that reads 10:59 and then 00 has produced a time that never
 * happened - which is a bug that appears once an hour and is never
 * reproducible. The same reason SYS_MOUSE fills in one structure. */
#define KOI_INFO_TIME 25             /* hour << 16 | minute << 8 | second */
#define KOI_INFO_DATE 26             /* year << 16 | month << 8 | day */
#define KOI_INFO_VOLUME_TOTAL_BYTES 27 /* index selects the volume, KiB */
#define KOI_INFO_VOLUME_FREE_BYTES 28  /* index selects the volume, KiB */

#define KOI_TIME_HOUR(packed) ((int)(((packed) >> 16) & 0xFF))
#define KOI_TIME_MINUTE(packed) ((int)(((packed) >> 8) & 0xFF))
#define KOI_TIME_SECOND(packed) ((int)((packed) & 0xFF))
#define KOI_DATE_YEAR(packed) ((int)(((packed) >> 16) & 0xFFFF))
#define KOI_DATE_MONTH(packed) ((int)(((packed) >> 8) & 0xFF))
#define KOI_DATE_DAY(packed) ((int)((packed) & 0xFF))

/* Text items, written into the caller's buffer and always terminated. */
#define KOI_TEXT_BUILD_DATE 0
#define KOI_TEXT_BUILD_COMMIT 1
#define KOI_TEXT_DISK_NAME 2         /* index selects the disk */
#define KOI_TEXT_VOLUME_LABEL 3      /* index selects the volume */
#define KOI_TEXT_AUDIO_DEVICE 4      /* the codec, or "none" */
#define KOI_TEXT_CPU_NAME 5          /* the processor brand string */
/* The path this program was loaded from, from the root of its drive.
 *
 * A program cannot work this out for itself: SYS_ARGS gives it the tail of the
 * command line and not the name it was invoked by. Which is fine until a
 * program has to ask for itself to be run again - a shell that chains a program
 * and wants to come back afterwards - and then guessing its own location is the
 * difference between working and working only when installed where the guess
 * happened to be right. */
#define KOI_TEXT_PROGRAM_PATH 6

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
/* (point, text, colour, background | style << 32). Bold, italic and underline
   are made from the glyph the font already has - see graphics.c - so they cost
   no extra font data and work with whatever font is fitted later. */
#define SYS_GFX_TEXT_STYLED 0x3B   /* (point, text, colour, packed) */
#define SYS_GFX_SCISSOR 0x3C       /* (point, size) - intersect clip rect */
#define SYS_GFX_SCISSOR_RESET 0x3D /* () - restore full-screen clipping */

#define KOI_TEXT_BOLD 1
#define KOI_TEXT_ITALIC 2
#define KOI_TEXT_UNDERLINE 4

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
#define SYS_SOUND_WHERE 0x46  /* (voice) -> frames played */
#define SYS_SOUND_LENGTH 0x47 /* (voice) -> frames in the sound */
#define SYS_SOUND_SEEK 0x48   /* (voice, frame) -> 0, or -1 */

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

/* ---- The pointer ---------------------------------------------------------
 *
 * One call, filling in a snapshot. Not four calls returning a coordinate each:
 * the pointer moves between them, and a program that reads x and then y can get
 * a position the pointer was never at. One structure, filled in one go, is a
 * place the pointer actually was.
 *
 * `scroll` is a running total rather than what has arrived since the last ask.
 * Taking would mean the first caller to look gets the notches and everyone else
 * sees a still wheel; a total can be read by any number of callers, each
 * remembering what it last saw and subtracting.
 */
#define SYS_MOUSE 0x50       /* (KOI_POINTER*) -> 1, 0 when there is none, -1 */
/* Put the pointer somewhere. What a program does on entering graphics mode, so
   that the pointer starts in the middle of its window rather than wherever it
   was left by whatever ran last. */
#define SYS_MOUSE_PLACE 0x51 /* (point) -> 0, or -1 */

/* ---- Running something else ----------------------------------------------
 *
 * One program runs at a time, at a fixed address, in one address space - the
 * whole of the design, and not going to change. So a program cannot call
 * another program; it asks for one to be run AFTER it has exited, when its own
 * memory is free and the new program can load into it.
 *
 * Requests run most-recent-first, which turns "run this and then bring me back"
 * into two ordinary calls:
 *
 *     koi_chain("MIZU Z:\\GAMES");   asked for first, runs second
 *     koi_chain("DOOM");             asked for last, runs first
 *     koi_exit(0);
 *
 * The argument is a command line, not a path: it goes back through the shell,
 * so the search path, drive letters, arguments and batch files behave exactly
 * as if it had been typed.
 *
 * Coming back is a fresh start and not a resume. Nothing of the program
 * survives, so anything it wants to remember it hands to itself as arguments or
 * writes to a file first. Small DOS shells did precisely this, for precisely
 * this reason. */
#define SYS_CHAIN 0x52       /* (command) -> 1, or 0 when too many are waiting */

/* ---- The clipboard -------------------------------------------------------
 *
 * One buffer, in the kernel, outliving the program that filled it. That is the
 * whole feature and it is the reason it belongs to the kernel rather than to a
 * shell: a clipboard that dies with the program cannot carry anything between
 * two programs, which is the only thing anyone wants a clipboard for.
 *
 * Windows 1.0 shipped one in 1985 and it was one of the three things the box
 * talked about. It is text only here - there is no second kind of thing this
 * system can put on it yet, and a type tag with one value is a lie about how
 * general something is.
 *
 * SYS_CLIP_GET into a null buffer returns the length without copying, which is
 * how a caller finds out how much room to make. */
#define SYS_CLIP_PUT 0x53    /* (text, length) -> length kept, or -1 */
#define SYS_CLIP_GET 0x54    /* (buffer, size) -> length, 0 when empty */

#define KOI_CLIP_MAX 65536

/* ---- The log, and the bytes underneath -----------------------------------
 *
 * A program writes into the same log the kernel writes into - the one that
 * goes to COM1, is kept in memory for `log`, and is written to a file at boot.
 * Not a log of its own: two logs of the same run have to be interleaved by
 * hand afterwards, by somebody guessing at the order, and the whole value of a
 * log is that it already knows the order.
 *
 * SYS_LOG_BYTES is the one that matters. Everything a program can say about
 * itself is what it believed; this is what is actually there. Diagnosing the
 * lost-files bug meant carrying the disk to another machine and taking it
 * apart there, because nothing on the machine that failed could show a sector.
 *
 * SYS_SECTOR_READ is read-only and cannot damage anything. It grants no power
 * a program did not have - programs run in ring 0 and can already touch any
 * memory in the machine - it makes an existing capability usable on purpose
 * rather than by accident. There is deliberately no write counterpart. */
#define SYS_LOG 0x55         /* (text) -> 0 */
#define SYS_LOG_BYTES 0x56   /* (label, data, length) -> 0 */
#define SYS_SECTOR_READ 0x57 /* (disk, lba, buffer) -> bytes read, or -1 */
/* How many disks, and how large a sector is on one of them - which a caller
   needs before it can offer a buffer. */
#define SYS_SECTOR_SIZE 0x58 /* (disk) -> bytes per sector, or -1 */

/* Run a command and come back when it has finished.
 *
 * The caller stays in memory with everything it had; the program it starts is
 * loaded into a slot of its own and this returns when that program exits. It
 * is what DOS's EXEC did and what SYS_CHAIN was standing in for while the
 * machine could hold one image at a time.
 *
 * SYS_CHAIN is still here and still means something different: chaining gives
 * up this program's memory before the next one starts, which is the only way
 * to run something that needs more of the window than is left. Running keeps
 * it. A shell that wants to come back where it was wants this one.
 *
 * The screen belongs to whoever took it. A program holding the framebuffer
 * should give it back before calling this and take it again afterwards -
 * nothing here does that for it, because a program that only wants to run
 * `dir` should not have its window torn down. */
#define SYS_RUN 0x59         /* (command) -> the program's exit code, or -1 */

/* The environment, as the shell has it.
 *
 * Read-only from a program, and that is a decision rather than an omission.
 * There is one environment because there is one program running at a time; a
 * program that could write it would be changing the shell's, and the change
 * would outlive the program that made it. DOS gave each program a copy for
 * exactly that reason, and copies are what this will get when programs are
 * isolated - at which point a setter can be added and will mean something.
 *
 * SYS_GETENV fills `buffer` with the value and returns its length, or 0 when
 * there is no such variable. SYS_ENVAT is how a program lists them: it takes
 * an index from zero and fills `name`, returning 0 when the index is past the
 * end. */
#define SYS_GETENV 0x5A      /* (name, buffer, size) -> length, or 0 */
#define SYS_ENVAT 0x5B       /* (index, name, size) -> length, or 0 */

#define KOI_BUTTON_LEFT 0x01
#define KOI_BUTTON_RIGHT 0x02
#define KOI_BUTTON_MIDDLE 0x04
#define KOI_BUTTON_SIDE_4 0x10
#define KOI_BUTTON_SIDE_5 0x20

typedef struct {
    int x;
    int y;
    unsigned int buttons;      /* KOI_BUTTON_*, as they are right now */
    int scroll;                /* notches since boot, positive upwards */
    unsigned int movements;    /* packets that have moved it, ever */
    unsigned int has_wheel;    /* also: whether two-finger scrolling works */
    /* How many times each has gone down, ever: left, right, middle.
     *
     * Use these for clicks, not `buttons`. A click lasts a tenth of a second
     * at most, and a program that looks thirty times a second will sooner or
     * later look between the press and the release and see nothing - which
     * presents as a button that sometimes does not work rather than as a
     * missed sample. A count cannot be missed. */
    unsigned int presses[3];
} KOI_POINTER;

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
