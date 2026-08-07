# <samp>Koi-DOS 🥂</samp>

A DOS-like operating system for UEFI machines, written in freestanding C with no standard
library.

> **`[NEW]`  The first programs written for Koi-DOS with the official SDK:**
>
> **[DOSFETCH](https://github.com/quartzz720/DOSFETCH)** — a system summary in the spirit of
> neofetch. No ASCII logo, on purpose.
>
> **[Games](https://github.com/quartzz720/Games)** — Snake, Tetris, Pong, Breakout, Reversi and
> Minesweeper, in one program with a menu.
>
> **[K-DOOM](https://github.com/quartzz720/K-DOOM)** — id Software's DOOM, on Koi-DOS. Four files
> replace the Unix layer and the game code is id's, unchanged. Bring your own WAD: the source is
> GPL, the game data is not.
>
> All three are built with nothing but [sdk/](sdk/), in their own repositories. That is the point
> of them: if one stops building, the SDK is broken for everybody rather than only for programs
> that happen to live in this tree.

"DOS-like" means the look and the character, not binary compatibility: drive letters, FAT32, a
familiar command set, a monolith in ring 0 with no memory protection. Programs are native
64-bit ones with their own system-call interface — real MS-DOS binaries are not a goal.

Two deliberate departures from DOS:

**The system volume is `Z:`**, not `C:`. In MS-DOS, `A:` and `B:` were reserved for floppy drives
and the system landed on `C:` only because that was the first letter left over. Floppies are
gone, and handing the system `A:` is not appealing either. Further volumes go `Y:`, `X:` downward.

**Long file names work in full.** The 8.3 limit was a consequence of 1980 hardware, not a design
goal, and this is what DOS would look like if it turned up on a UEFI machine today. Short names
still exist underneath every entry, but they are not what you see.

---

## What it does

The kernel boots on bare metal, leaves UEFI Boot Services behind, installs its own GDT, IDT and
page tables, brings up a keyboard over PS/2 or USB, finds disks over AHCI, NVMe and USB storage,
parses the partition table, mounts FAT32, and runs a shell that loads programs off that disk. It
can install itself onto a disk, and it can hand the screen to a program that wants to draw.

```
KOI DOS KERNEL
MEMORY ALLOC FREE: OK
MEMORY FREE: 1970 MB
CPU: GDT IDT PIC READY
PAGING: IDENTITY MAP 16384 MB, FRAMEBUFFER WRITE COMBINING
HEAP: 1023 KB, SELF TEST OK
KEYBOARD: PS/2 READY
TIMER: PIT POLLING 1000 HZ
APIC: TIMER 62652 KHZ, CALIBRATED
TIMER: LOCAL APIC, INTERRUPT DRIVEN
TIMER: 100 HPET MS COUNTED AS 100 MS WITHOUT POLLING
KEYBOARD: MOVED TO THE IO APIC
PCI: 9 DEVICES
AHCI: DISK 192 MB
NVME: DISK 128 MB
AUDIO: QEMU CODEC, 48 KHZ STEREO
XHCI: 2 OF 2 CONTROLLERS
XHCI: 2 OF 16 PORTS IN USE, KEYBOARD, STORAGE
VOLUMES: 4 FAT32 OF 4
SYSTEM VOLUME: KOI-DOS - the loader's partition has no drive letter

Z:\> dir \bin
 Volume in drive Z is KOI-DOS
 Directory of Z:\BIN

2026-08-07  04:17        13576  CAT.EXE
2026-08-07  04:17        18232  COLOR.EXE
2026-08-07  04:17        14352  DEMO.EXE
2026-08-07  04:17        13440  HELLO.EXE
2026-08-07  04:17        13496  LS.EXE
2026-08-07  04:17        13576  SAVE.EXE
2026-08-07  04:17        47032  SHOW.EXE

       7 file(s)          133704 bytes
       0 dir(s)        65536000 bytes free
```

Most of those boot lines are a claim being checked rather than a step being announced. The memory
figure comes from an allocate-and-free round trip, the heap from a self test, and the two timer
lines are one clock measuring another — the second is the measurement the polled PIT could not
make at all, because it used to read 100 ms as 0.

### Commands

| | |
|---|---|
| `dir [path] [/w]` | list a directory; `/w` gives names only |
| `cd`, `chdir` | change directory, `cd ..` to go up |
| `type` | print a file |
| `more` | print a file a screen at a time |
| `tree [path]` | the directory tree, drawn in CP437 |
| `attrib [+-RHSA] file` | show or change attributes |
| `copy`, `del`/`erase`, `ren`/`rename` | file management |
| `md`/`mkdir`, `rd`/`rmdir` | directories |
| `vol`, `date`, `time` | volume label and the clock |
| `mem` | memory, and what the drivers actually found |
| `pci` | every function on the PCI bus, with its class |
| `disk` | disks and partitions, whether or not they have a drive letter |
| `format <part>` | make a new filesystem — destroys everything on it |
| `part <disk>` | replace the partition table — destroys the whole disk |
| `setup` | install Koi-DOS onto a disk |
| `shutdown`, `reboot` | through ACPI, which is the only way there is |
| `beep [hz] [ms]` | a tone, if the machine has a sound device |
| `sound` | the sound device, and which output it chose |
| `log [file]` | the kernel log: on screen, or written to a file |
| `echo`, `ver`, `cls`, `help` | the usual |
| `Z:` | change drive |

The three destructive ones ask before they act, and none of them will touch the device the system
is running from. `format` and `part` require the partition or disk to be typed back by name —
`y` is too easy to press by reflex — and `setup` shows the licence and the layout it is about to
write before it writes anything.

`log` is the same idea taken to its end. Every driver says what it found and why it gave up over
COM1 — and no machine made this century has a COM1. The text is kept in memory as well, so `log`
prints it and `log KOI.LOG` writes it to a file, which is the only way to get a boot log off a
laptop and onto something that can read it. It was written the first time a sound card failed on
real hardware and the explanation went to a port that was not there.

`mem` doubles as the system's self-description, because the alternative to a line reading
`USB : keyboard, storage` is rebooting with a serial cable attached to find out whether a driver
came up:

```
Z:\> mem
Physical memory : 2047 MB
Available       : 1973 MB

Kernel image    : 2200 KB
Page tables     : 80 KB
Identity map    : 16386 MB (RAM and device windows)
Heap            : 1023 KB
Heap free       : 1023 KB

PCI devices     : 9
Disks           : 3  ahci0, nvme0, usb0
Volumes         : 3  Z:, Y:, X:
USB             : keyboard, storage (2 of 16 ports in use)
Audio           : QEMU codec, 48 kHz stereo
```

`ver` carries a build number, which during development is worth more than the version — two
kernels both claiming 0.5 differ by whatever happened in between. The number is the commit count,
so it only moves when history does, and a trailing `+` means the tree had uncommitted changes
when that kernel was built:

```
Z:\> ver
Koi-DOS 0.5 Alpha
Kernel 0.5.23, built 2026-08-07 (18400fe)
A DOS-like operating system for UEFI machines.
```

Patterns work where they should — `dir *.exe`, `copy *.txt backup`, `del *.bak`. `*` matches any
run of characters including dots, `?` exactly one. `del` with a pattern asks first.

Paths use backslashes. Forward slash introduces switches, as in DOS. All three path forms are
accepted and mean different things: `EFI\BOOT` from the current directory, `\EFI\BOOT` from the
root of the current drive, `Z:\EFI\BOOT` from the root of a named one.

Batch files run a line at a time; `@` suppresses the echo, `rem` and `:` are comments and labels,
and `AUTOEXEC.BAT` at the root of the boot drive runs at startup.

### Programs

Anything that is not a built-in command is looked up on disk as `NAME` or `NAME.EXE` — first in
the current directory, then at the root of the drive, then in `\BIN` — and given the rest of the
line as its arguments. `\BIN` is where an installation puts them, because the root of the system
volume is for the user's files and a pile of `.EXE` in it turns `dir` into a search. Seven ship
in [programs/](programs/):

| | |
|---|---|
| `hello` | prints its arguments and the version — the smallest complete example |
| `cat <file>` | prints a file, reading it through the system calls rather than asking the shell |
| `save <file> <text>` | writes its arguments to a file |
| `ls [pattern]` | a directory listing written as an ordinary program |
| `color` | changes the console colours and remembers them |
| `demo` | every drawing primitive on one screen, as a test that happens to look like something |
| `show <file.bmp>` | displays an uncompressed 24- or 32-bit BMP, centred |

`ls` and `color` exist to prove two specific things: that the directory-enumeration calls are
enough to write a listing with no privilege the shell does not also lack, and that a program can
change how the system looks and make it stick without reaching into kernel state.

Three more proofs live outside this repository on purpose, each built with nothing but the SDK:
**[DOSFETCH](https://github.com/quartzz720/DOSFETCH)**, a system summary in the spirit of
neofetch; **[Games](https://github.com/quartzz720/Games)**, six of them in one program with a
menu; and **[K-DOOM](https://github.com/quartzz720/K-DOOM)**, id Software's DOOM with its Unix
layer replaced. If one stops building, the SDK is broken for everybody rather than only for
programs that happen to sit in this tree.

Games proves the graphics calls, because it is the only thing here that draws sixty times a
second. DOOM proved something less comfortable: it is the first program that kept a live value in
a register across a system call, and it found that the stub had never preserved the registers
this document promised it would.

```
Z:\> color                 show the presets and the palette
Z:\> color amber           a preset: dos, mono, amber, green, paper, night
Z:\> color /b black        just the background
Z:\> color yellow blue     text and background
```

### Sound

**HD Audio**, because AC'97 is emulated everywhere and fitted to nothing: every machine this
could run on has an HDA controller, and QEMU emulating the easy one as well is a trap rather than
a shortcut. Works on real hardware, where it found the one thing QEMU could never have shown —
see below. **The PC speaker is not used at all** — no laptop has had one for years, and the
thing that makes the noise is the same chip the headphone socket comes out of.

The driver does one thing — it finds a codec, traces a path from a physical output back through
whatever mixers and selectors are in the way to a converter, unmutes every step of it, and leaves
a single stream of 48 kHz stereo running over a ring of memory forever. It never starts or stops:
a stream started per sound clicks, and one that always runs does not.

**Which output** is decided by what is plugged in, not by what the socket is called. Ranking by
name — line out, then headphones, then the built-in speaker — is right on a desk and wrong on a
laptop, where it picks the headphone socket and leaves the machine silent with nothing in it. So
a jack that reports something plugged into it wins; the built-in speaker comes next, because it
is always there; a jack that cannot report comes after that, since guessing "empty" would silence
a machine that works; and a jack that reports nothing plugged in is not used. External amplifiers
are switched on where the codec has one — a laptop whose speakers are wired through one is silent
without that while every register reads correctly.

`sound` prints all of it. "There is no sound" has several completely different causes that are
identical from a chair — no controller, a controller with no codec, a codec whose only outputs
are digital, an empty headphone socket, or the right socket chosen and muted further along — and
the codec's own description of its outputs is the difference between guessing and knowing:

```
Z:\> sound
Codec           : Realtek (10EC0295)
Output          : speaker, converter at node 2
Rate            : 48000 Hz, stereo, 16-bit
Volume          : 200 of 255
Voices          : 0 of 16 playing

Analogue outputs the codec describes:
  node 20 speaker    built in                 <- in use
  node 33 headphones nothing plugged in
```

Everything above that is [kernel/audio.c](kernel/audio.c), which has no hardware in it. Sixteen
voices, each with its own rate, volume and pan, resampled into the ring in 32.32 fixed point —
there is no floating point anywhere, because nothing configures SSE state after
`ExitBootServices`.

**The cache is flushed on the way out.** On x86 a device's DMA snoops the processor's caches, so
this should be unnecessary — and on the first real machine it was not. That controller marks its
stream traffic with a priority bit it will not let anyone clear, and traffic marked that way does
not snoop: the descriptor list had never left the cache, so the controller read a list of
zero-length buffers, transferred nothing and reported no error, because a buffer of zero bytes is
not an error. Meanwhile the command rings worked perfectly, because they are not a stream. Two or
three cache lines per millisecond, and it is the difference between silence and sound.

**The mixer runs from the timer interrupt.** Not for precision: forty-eight frames a millisecond
is nothing. It is because the alternative is mixing wherever the system happens to be looping,
and the moment a program stops making system calls — a game rendering a frame, a copy grinding
through a large file — the sound would stop with it. Measured: a tone held through a graphics
program starting, drawing and exiting, with not one silent 20 ms block in it.

**[K-DOOM](https://github.com/quartzz720/K-DOOM) is the first thing to use it**, and it needed
exactly one addition to do so: `SYS_SOUND_PARAMS`, which changes a sound that is already playing.
DOOM calls it every tic for every sound whose source has moved relative to the player, and
without it a rocket stays where it was fired.

The whole path was checked from outside the guest rather than by ear. QEMU records what the
machine played into a file, and the file is measurable where a noise in the room is not: a tone
asked for as 440 Hz for one second measures as 440 Hz for 1.000 seconds; six pistol shots in DOOM
land at the six moments the keys were sent, each the length `DSPISTOL` is at 11025 Hz, because a
wrong sample rate shows up as a wrong duration; and a sound panned from one side to the other
comes out silent in the far channel at each end and full in both at the centre.

### Writing programs without the kernel source

[sdk/](sdk/) holds the four files a program needs — the header, the entry stub,
the linker script and the system call definitions — plus a one-line build
script, so a program can be written and built by someone who does not have the
kernel checked out:

```bash
cd sdk && ./koicc mytool.c              # produces MYTOOL.EXE
./koicc main.c board.c -o mytool        # several sources, one program
```

No special compiler: a Koi-DOS program is a freestanding ELF64 binary, and an
ordinary x86-64 GCC produces one. `sdk/README.md` documents the ABI.

`koilib.c` comes with it: the memory and string primitives, character
classification, number conversion, a `printf`-style formatter and a heap. Not
a C library and not trying to be one — this is the subset that either the
compiler requires or that any program larger than a page rewrites badly on its
own. The compiler emits calls to `memcpy` and friends whatever you do, so
those four are not optional. Unused parts are collected at link time, so a
program that wants `strlen` does not also carry a formatter.

Several sources are one program, not several — there is no linker step to run
afterwards and no object files to keep. Anything larger than a single file
needs this, and the first program that did was a games collection.

**Programs you write are yours.** Including these headers does not place your
program under this project's licence; see the LICENSE.

### Configuration

`\BOOT\userspace.cfg` is read once at boot and applied before the shell paints anything. One
`key = value` per line, `#` or `;` starts a comment, unknown keys are ignored and a missing file
is not an error.

```ini
# Written by color.exe; read by the kernel at boot.
foreground = yellow
background = black
prompt = brown
```

Programs write that file; the kernel reads it. Neither reaches into the other.

### System calls

Programs call the kernel with `int 0x40`, in the spirit of DOS's INT 21h. `RAX` holds the
function number, `RDI`/`RSI`/`RDX`/`RCX` the arguments, `RAX` the result. Forty-two calls cover
console I/O, files, directory enumeration, the command line, exit codes, what the system knows
about itself, taking the screen and making a noise; they are listed in
[include/syscall.h](include/syscall.h), which the kernel and every program include from the same
copy so the two cannot drift apart. That file also names the keys that have no ASCII value — the
arrows and the function keys — because a program that reads them has to name them, and two copies
of a number is how two copies of a number drift apart.

`SYS_GFX_PRESENT_RECT` sends one rectangle instead of the whole screen, and it is the difference
between a game that runs and one that crawls: the screen is whatever size the firmware chose,
often far larger than the area a program uses, and sending all of it sixty times a second costs
more than everything else put together.

Three of those exist for programs that cannot afford to stop. `SYS_GETCHAR` waits, which is right
for a prompt and wrong for anything that has to keep moving; `SYS_KEYPRESSED` asks whether a key
is waiting without taking it, and `SYS_SLEEP` waits without spinning.

`SYS_KEYEVENT` answers a different question again: **which key went down, and which came up**. A
character stream cannot say that a key is being *held* — it has no idea a key is still down and no
idea when it stopped being — so anything where that matters, walking forward or steering or
holding a button, needs events rather than characters. Both drivers had the information all along
and were discarding it one line before it could be delivered. The identity reported is the
unshifted one, so a key reads the same going down as coming up.

`SYS_SOUND_PLAY` and `SYS_SOUND_TONE` put something into the one stream that is always running.
There is nothing to open and nothing to wait for: a call hands back a voice, or -1 when every
voice is busy or the machine has no sound hardware. **The samples are not copied**, which is what
makes firing the same effect twenty times a second free — and it is why every voice is stopped
when a program exits. A voice holds a pointer into the program's memory and the mixer reads it
from the timer interrupt; left running past its program, it would not make a wrong noise once, it
would make one a thousand times a second forever.

`SYS_ALLOC` and `SYS_FREE` give a program whole pages beyond its own image. Not a malloc and not
meant to be one: a program that wants small objects takes one large block and divides it itself.
Everything is released when the program exits, whether or not it remembered to — nothing here
reclaims memory later, so a leak would otherwise be permanent.

The vector is `0x40` rather than `0x21` because in protected mode `0x21` is the keyboard IRQ.
`INT` rather than `SYSCALL`/`SYSRET` because the whole value of `SYSRET` is a fast ring 3 to
ring 0 transition, which does not happen in a ring-0 monolith.

**A program carries the interface version it was built against**, and the kernel checks it before
running anything. Both directions are refused while the interface is ALPHA. Newer is obvious — it
would call functions this kernel does not have. Older is the surprising half: function numbers
may have been reused since, and a program calling a number that has changed meaning does not
fail, it quietly does the wrong thing. Once the numbering is frozen that stops, and numbers are
never reused again — a removed call leaves a hole.

### Writing a program

Drop a `.c` file in [programs/](programs/) and rebuild — the Makefile picks it up and produces a
`.EXE` on the disk image.

```c
#include "koi.h"

int main(const char* arguments) {
    koi_color(KOI_YELLOW, KOI_BLUE);
    koi_print("Hello.\n");
    if (arguments[0]) {
        koi_print("Arguments: ");
        koi_print(arguments);
        koi_print("\n");
    }
    return 0;
}
```

[programs/koi.h](programs/koi.h) wraps every system call. There is no C library: a program gets
those calls and whatever it writes itself.

A program that draws has the same shape, with the screen taken and given back around it:

```c
#include "koi.h"

int main(const char* arguments) {
    KOI_SCREEN screen;
    (void)arguments;

    if (koi_gfx_enter(&screen) != 0) return 1;
    koi_gfx_clear(koi_gfx_color(0, 0, 40));
    koi_gfx_fill(10, 10, 100, 60, koi_gfx_color(255, 200, 0));
    koi_gfx_text(10, 80, "hello", koi_gfx_color(255, 255, 255),
                 KOI_TEXT_TRANSPARENT);
    koi_gfx_present();
    koi_getchar();
    koi_gfx_leave();
    return 0;
}
```

Nothing appears until `koi_gfx_present` — that is not an optimisation, it is the difference
between an image and a program being watched as it draws one. `screen.pixels` is the buffer
itself and may be written directly, which is the fast path; the calls exist so that a program
does not have to know how a pixel is laid out. Never assemble a colour by hand: the framebuffer's
channel order differs between machines, and code that guesses draws in the wrong colours on half
of them.

---

## Status

Working: the boot chain, framebuffer console with an 8×16 CP437 font, exception handling with a
register dump instead of a silent reboot, physical page allocator, `kmalloc` heap, own page
tables, AHCI, NVMe, USB, GPT/MBR/whole-device partitioning, FAT32 read **and** write with long
names, the command interpreter, programs and system calls, graphics, and an installer.

**Keyboards work over both PS/2 and USB.** The xHCI driver takes the controller from the
firmware, resets the port, addresses the device, reads its descriptors and drives it in HID boot
protocol; keys land in the same buffer as PS/2 ones, so the shell cannot tell which kind produced
them. Verified with PS/2 switched off, leaving USB as the only way in.

**USB sticks work too.** SCSI over bulk-only transport, registered through the same block
interface AHCI uses, so the filesystem never learns which controller it is reading. A keyboard
and a stick share the controller — every event carries the slot and endpoint it came from, so a
keystroke that arrives during a disk read is delivered rather than swallowed. Booting from a
stick with an internal disk also present puts `Z:` on the stick and the internal EFI System
Partition on `Y:`.

**NVMe works**, registered through the same block interface as AHCI and USB storage, so the
filesystem never learns which of the three it is reading.

**Time is interrupt-driven.** The Local APIC timer keeps the millisecond tick — on-die and
per-CPU, so it stays cheap and needs no locking if this ever grows a second processor — and the
HPET is the monotonic clock it was calibrated against. IRQs come through the I/O APIC, honouring
whatever the firmware's MADT says about where a legacy IRQ actually arrives, and the 8259 is
masked once that is standing.

**A program can take the screen.** `koi_gfx_enter` hands it a buffer the size of the display and
stops the console from drawing; `koi_gfx_present` puts a finished frame up; `koi_gfx_leave` gives
the screen back and the console repaints exactly what was there before. Between those the program
may use the drawing calls or write to the buffer directly — this is ring 0 and pretending
otherwise would only make drawing slow. There is no mode switching: UEFI chose the resolution
before `ExitBootServices` and it cannot be changed afterwards, so "graphics mode" here means the
console stops drawing and something else starts. The shell takes the screen back whether or not a
program remembered to, which is the failure DOS programs were famous for.

**It can install itself.** `setup` writes a GPT with an EFI System Partition for the loader and a
system partition for everything else, makes both filesystems, copies the system across and marks
the system volume. The loader's partition gets no drive letter at all — not security, since any
other operating system sees an ordinary partition, but it does mean a stray `del` cannot reach
the files the machine needs to start. Verified the only way that means anything: installing to a
blank disk, removing the media, and booting the machine from what was written.

**The filesystem is fast enough to install with**, which it was not at first, and the reasons are
worth naming because none of them could show on a 64 MB test image. The cluster size was being
chosen backwards — the smallest that was still legal FAT32, which gave a 223 GB partition 234
million clusters and a 936 MiB allocation table. Free space was counted by walking that whole
table, and the count was thrown away on every cluster allocation, which is also why `dir` used to
freeze for a minute: it asks for the figure after printing the listing. FAT entries went to the
disk one at a time although 128 of them share a sector. And data moved one sector per command
although every driver underneath accepts a run. A 16 MB file from a USB stick to NVMe did not
finish in three minutes before; it takes five seconds now.

Missing: **audio and networking**. There is no `edit` — the line editor is
the one command from the old shell not carried over, and it belongs as a program now that
programs exist. `chkdsk` does not exist, so an interrupted write has to be repaired from another
system. The xHCI interrupt is not routed anywhere, so waiting for a USB key spins rather than
sleeps; USB devices are enumerated once at boot, so plugging a stick in afterwards does nothing
until a reboot; and anything behind a USB hub is invisible. `ahci_init()` stops at the first ATA
disk it finds, so a machine with two SATA drives shows one. NVMe describes each transfer with a
single PRP entry, which cannot cross a page boundary, so it moves 4 KiB per command where AHCI
moves 128 KiB.

**It has now run on physical hardware** — two machines, once each. Confirmed there: the boot
chain, the framebuffer console, AHCI, USB mass storage, the USB keyboard, and drive letters
across two FAT volumes. See [Running on real hardware](#running-on-real-hardware).

Writes are verified from outside the guest, because a filesystem that satisfies only its own
reader proves nothing:

```bash
./qemu.sh                 # create files at the Koi-DOS prompt, then quit
fsck.fat -n -v esp.img    # must report no errors
mdir -i esp.img ::/       # must show what the guest created
KEEP_IMAGE=1 ./qemu.sh    # boot the same image again - the files are still there
```

The original UEFI-based shell is under [legacy/](legacy/); it ran inside Boot Services, is not
built, and no longer compiles. Its command semantics were the reference for the new one.

See [ARCHITECTURE.md](ARCHITECTURE.md) for how the thing actually works.

---

## Tested on

**Fedora Linux 44** (x86_64) is the reference environment — everything below is verified there.
Other systems should work but are not routinely tested.

| Component | Version used |
|---|---|
| GCC | 16.1.1 |
| MinGW-w64 GCC | 16.1.1 |
| QEMU | 10.2.2 |
| binutils | 2.46.1 |

## What the build needs

Two compilers, because two ABIs are involved:

- **`gcc`** targeting ELF — builds the kernel and the programs. It must produce ELF64, so either
  a Linux-hosted GCC or an `x86_64-elf-*` cross-compiler.
- **`x86_64-w64-mingw32-gcc`** — builds `BOOTX64.EFI`. UEFI uses PE/COFF and the Microsoft x64
  ABI, which is exactly what MinGW targets.

Plus `binutils` (`nm`, `readelf`, `objcopy`), `qemu-system-x86_64`, OVMF firmware, `dosfstools`
(`mkfs.vfat`) and `mtools` for building the test image.

### Fedora (reference)

```bash
sudo dnf install gcc binutils mingw64-gcc qemu-system-x86-core edk2-ovmf dosfstools mtools
```

### Debian / Ubuntu

```bash
sudo apt install gcc binutils gcc-mingw-w64-x86-64 qemu-system-x86 ovmf dosfstools mtools
```

### Arch

```bash
sudo pacman -S gcc binutils mingw-w64-gcc qemu-system-x86 edk2-ovmf dosfstools mtools
```

### macOS

Untested. It needs an `x86_64-elf-gcc` cross-compiler — the system toolchain produces Mach-O,
not ELF — plus `mingw-w64`, `qemu` and `mtools` from Homebrew, and GNU `binutils` for `readelf`:

```bash
make KERNEL_CC=x86_64-elf-gcc
```

### Windows

The old MSYS2 instructions no longer apply. MSYS2's native GCC emits PE, and the kernel has to be
ELF, so MinGW alone can no longer build both halves of this project. Use **WSL2** with any of the
Linux setups above.

## Build and run

```bash
make          # BOOTX64.EFI + KERNEL.ELF + build/*.EXE
make check    # undefined symbols, relocations, program headers
./qemu.sh     # build a FAT32 image and boot it
```

`qemu.sh` does the whole cycle: runs `make`, creates the disk images with `mkfs.vfat`, populates
them with `mtools`, copies a fresh OVMF variable store to `/tmp/koi-vars.fd`, and launches QEMU.
Extra arguments pass straight through:

```bash
./qemu.sh -display none     # no window, serial log in the terminal only
./qemu.sh -s -S             # wait for gdb on :1234
KEEP_IMAGE=1 ./qemu.sh      # reuse the existing images instead of rebuilding them
KOI_SPLIT=1 ./qemu.sh       # boot the two-partition layout the installer writes
```

Three images, on three different kinds of controller, because one device of a kind never
exercises the code that handles two. `esp.img` is the system volume on an emulated SATA disk;
`stick.img` is presented as a USB stick, so the mass storage driver has something to enumerate;
and `nvme.img` is an NVMe drive carrying a real GPT with two partitions — one FAT, one
deliberately left as raw bytes, so that "a partition we cannot read" is a case being tested
rather than dead code that looks like it works.

`KOI_SPLIT=1` builds the boot disk the way `setup` lays it out — the loader on its own EFI System
Partition and the system on a second one, with the loader's getting no drive letter. Off by
default, because a quick-formatted USB stick is a single volume and that is how this has actually
been booted on real hardware. Both need testing; neither is "the" configuration.

For the same reason there are **two xHCI controllers**, with the keyboard on one and the stick on
the other. That is what the first real machine turned out to look like — a chipset controller and
a processor one — and a single controller made every event unambiguous by accident, hiding the
bug completely.

The boot log appears **in your terminal** as well as in the QEMU window, because `-serial stdio`
is always on. `Ctrl+C` in the terminal quits.

The script probes the usual OVMF locations for each distribution. If yours is somewhere else:

```bash
OVMF_CODE=/path/OVMF_CODE.fd OVMF_VARS=/path/OVMF_VARS.fd ./qemu.sh
```

`make check` reports undefined symbols (must be empty — a missing `memset` shows up here),
relocations (must be none, or the kernel is not actually position-fixed), and the program headers.

## Running on real hardware

A USB stick is now the sensible way to try it.

```bash
./deploy.sh          # find the stick, ask, copy the current build onto it
./deploy.sh -l       # list removable disks and stop
```

`deploy.sh` finds the stick rather than being told which it is, because the letter moves between
`sdc` and `sdf` depending on what else is plugged in and typing the wrong one once is how a disk
gets ruined. It refuses anything that is not removable, refuses the disks the running system is
on — asked of the kernel, not written down here, since a list of names would be wrong the first
time the machine changed — and shows what it found before it writes a byte.

**It only copies files.** It does not format, partition or touch a boot sector; preparing a stick
is a one-off done by hand. Existing files of the same name are replaced and everything else is
left alone. If the desktop has already mounted the stick it uses that mount rather than fighting
it, which is where `Device or resource busy` came from.

It also picks up `GAMES.EXE` and `DOSFETCH.EXE` from the sibling projects when they are checked
out next to this one, so one command refreshes everything.

DOOM is carried the same way but not into `\BIN`: a program with data files gets a directory of
its own, so `DOOM.EXE` and its WAD land together in `\DOOM`. The WAD is looked for beside the
port and then in a copy of the game next to it, is skipped when the one on the stick is already
that size — eleven megabytes over USB is worth not doing twice — and is in no repository, here
or there. If none is found, DOOM goes across anyway and the script says where to put yours.

The bootloader records the FAT volume serial of the device it was loaded from, and the kernel
matches on it rather than assuming the first FAT volume it finds is the right one. That
assumption would have been dangerous: booted off a stick, the first FAT volume the AHCI driver
finds is the **internal drive's EFI System Partition**, and `Z:` would have pointed at the real
system's boot files — with `del` and `copy` aimed there.

With mass storage working, the stick itself is enumerated, the serial matches, and `Z:` is the
stick. This is not theory: on a laptop with Windows on a SATA SSD, `Z:` came up as the stick and
Windows' EFI System Partition as `Y:` — reachable if you go looking, but not where anything lands
by default. If no volume matches, none is assigned: the prompt reads `?:\>` and the log says
`BOOT VOLUME: NOT FOUND`, which is the only safe answer.

Booting from a SATA disk the AHCI driver can see works too. Either way, putting Koi-DOS on the
EFI partition of a system you care about is not recommended, since it can write.

To install it, boot from the stick and run `setup`. It asks which disk, shows the licence and the
layout it is about to write, and requires the disk to be typed back by name before it touches
anything. **It replaces the whole partition table of the disk it is given** — there is no
dual-boot install yet, so it takes the disk or nothing. It refuses outright to touch the device
it is running from, which is the one mistake nothing could recover from.

The media needs `EFI\BOOT\BOOTX64.EFI`, `BOOT\KERNEL.ELF`, the programs in `BIN\`, and `LICENSE`
at the root — setup will not proceed without the last one, because agreeing to a licence it
cannot show you would be theatre.

Two commands earn their keep here, because a machine that misbehaves in the field has no serial
cable attached. `mem` says what each driver actually found; `pci` lists every function on the
bus with its class, which is how you tell "no driver for this" apart from "the device was never
enumerated". The second real machine was diagnosed from those two screens alone.

A note on keyboards, since the obvious guess is backwards — though with USB working, either kind
now does. A **laptop's internal keyboard is usually PS/2** — it hangs off the embedded controller, which presents itself as an 8042 — so it
stands a good chance of working already. A **desktop with a USB keyboard** is the harder case,
because the firmware's legacy emulation generally stops once boot services are gone. And an
8042 in the chipset does not mean a socket on the board: the driver resets the device and waits
for a reply rather than trusting the ACPI flag, and reports `NO KEYBOARD ATTACHED` when nothing
answers.

## Two testing details that matter

**Why a real FAT32 image and not `-drive fat:rw:`.** QEMU can synthesise a filesystem from a host
directory, which is convenient — but it synthesises FAT16, and this project grows its own FAT32
driver. Testing that driver against FAT16 would prove nothing.

**Why `-m 2G`.** With 256 MiB of guest RAM no allocation ever lands above 4 GiB, so a DMA address
silently truncated to 32 bits behaves perfectly. That class of bug only shows up on a machine
with real memory, so the test machine gets real memory.

## Layout

```
boot/bootloader.c      UEFI entry point, ELF64 kernel loader
include/
  bootinfo.h           the bootloader -> kernel ABI (no EFI types)
  efi.h                EFI types, protocols, GUIDs
  elf.h                minimal ELF64 definitions
  syscall.h            the system call ABI, shared with programs
kernel/
  kernel.c             entry point and init order
  serial.c             COM1 debug output, comes up first
  cpu.c                our own GDT, and the TSS the double-fault stack hangs off
  isr.S                interrupt stubs, and the system call gate
  idt.c                IDT, dispatch, panic screen
  pic.c                8259 remap and masking
  paging.c             our own page tables, 2 MiB identity map
  memory.c             physical page allocator
  heap.c               kmalloc/kfree
  console.c            framebuffer text console, and the theme
  font.c               8x16 CP437 font
  graphics.c           drawing primitives, and the screen a program can take
  keyboard.c           PS/2 keyboard on IRQ1
  acpi.c               RSDP/XSDT walk, hardware-presence questions
  rtc.c                CMOS clock, and FAT timestamp packing
  string.c             mem*/str*, and the glob matcher
  io.h                 port I/O
  timer.c              the millisecond tick: PIT while polling, Local APIC once it is up
  pci.c                PCI enumeration
  ahci.c               SATA/AHCI disk I/O
  nvme.c               NVMe queues, Identify, read/write
  hpet.c               the HPET, as a monotonic clock
  apic.c               Local APIC timer and I/O APIC interrupt routing
  xhci.c               USB 3 controller, HID boot keyboard and mass storage
  block.c              sector-device abstraction over any controller
  partition.c          GPT / MBR / whole device, and drive letters
  fat32.c              FAT32 with long names, read and write
  command.c            the command interpreter
  config.c             reads \BOOT\userspace.cfg at boot
  program.c            loads and runs programs
  syscall.c            system call dispatch
programs/
  koi.h                the interface programs are written against
  start.c              _start, calls main, turns its return into an exit
  program.ld           linker script, fixed at 16 MiB
  hello.c cat.c save.c ls.c color.c demo.c show.c
sdk/                   the four files a program needs, plus koicc
legacy/                the old UEFI Boot Services shell, kept for reference
linker.ld              kernel layout, fixed at 1 MiB
qemu.sh                image build + QEMU launch
deploy.sh              find the USB stick and copy the build onto it
```

## License

See [LICENSE](LICENSE).

---

**Author**: quartzz720
**Target**: UEFI x86_64
