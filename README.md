# <samp>Koi-DOS 🥂</samp>

A DOS-like operating system for UEFI machines, written in freestanding C with no standard
library.

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
page tables, brings up a keyboard, finds the disk over AHCI, parses the partition table, mounts
FAT32, and runs a shell that loads programs off that disk.

```
KOI DOS KERNEL
MEMORY ALLOC FREE: OK
MEMORY FREE: 1984 MB
CPU: GDT IDT PIC READY
PAGING: IDENTITY MAP 16384 MB
HEAP: 1023 KB, SELF TEST OK
KEYBOARD: PS/2 READY
TIMER: PIT POLLING 1000 HZ
PCI: 6 DEVICES
AHCI: DISK 64 MB
VOLUMES: 1 FAT32 OF 1

Z:\> dir *.exe
 Volume in drive Z is KOI-DOS
 Directory of Z:\

2026-08-06  03:24         9360  CAT.EXE
2026-08-06  03:24        14024  COLOR.EXE
2026-08-06  03:24         9224  HELLO.EXE
2026-08-06  03:24         9280  LS.EXE
2026-08-06  03:24         9360  SAVE.EXE

       5 file(s)           51248 bytes
       0 dir(s)        65864192 bytes free
```

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
| `vol`, `mem`, `date`, `time` | volume label, memory use, the clock |
| `echo`, `ver`, `cls`, `help` | the usual |
| `Z:` | change drive |

Patterns work where they should — `dir *.exe`, `copy *.txt backup`, `del *.bak`. `*` matches any
run of characters including dots, `?` exactly one. `del` with a pattern asks first.

Paths use backslashes. Forward slash introduces switches, as in DOS. All three path forms are
accepted and mean different things: `EFI\BOOT` from the current directory, `\EFI\BOOT` from the
root of the current drive, `Z:\EFI\BOOT` from the root of a named one.

Batch files run a line at a time; `@` suppresses the echo, `rem` and `:` are comments and labels,
and `AUTOEXEC.BAT` at the root of the boot drive runs at startup.

### Programs

Anything that is not a built-in command is looked up on disk as `NAME` or `NAME.EXE`, first in
the current directory and then at the root of the drive, and given the rest of the line as its
arguments. Five ship in [programs/](programs/):

| | |
|---|---|
| `hello` | prints its arguments and the version — the smallest complete example |
| `cat <file>` | prints a file, reading it through the system calls rather than asking the shell |
| `save <file> <text>` | writes its arguments to a file |
| `ls [pattern]` | a directory listing written as an ordinary program |
| `color` | changes the console colours and remembers them |

`ls` and `color` exist to prove two specific things: that the directory-enumeration calls are
enough to write a listing with no privilege the shell does not also lack, and that a program can
change how the system looks and make it stick without reaching into kernel state.

```
Z:\> color                 show the presets and the palette
Z:\> color amber           a preset: dos, mono, amber, green, paper, night
Z:\> color /b black        just the background
Z:\> color yellow blue     text and background
```

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
function number, `RDI`/`RSI`/`RDX`/`RCX` the arguments, `RAX` the result. Eighteen calls cover
console I/O, files, directory enumeration, the command line and exit codes; they are listed in
[include/syscall.h](include/syscall.h), which the kernel and every program include from the same
copy so the two cannot drift apart.

The vector is `0x40` rather than `0x21` because in protected mode `0x21` is the keyboard IRQ.
`INT` rather than `SYSCALL`/`SYSRET` because the whole value of `SYSRET` is a fast ring 3 to
ring 0 transition, which does not happen in a ring-0 monolith.

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

---

## Status

Working: the boot chain, framebuffer console with an 8×16 CP437 font, PS/2 keyboard, exception
handling with a register dump instead of a silent reboot, physical page allocator, `kmalloc`
heap, own page tables, AHCI, GPT/MBR/whole-device partitioning, FAT32 read **and** write with
long names, the command interpreter, programs and system calls.

Missing: USB and NVMe. There is no `edit` — the line editor is the one command from the old shell
not carried over, and it belongs as a program now that programs exist. `chkdsk` does not exist,
so an interrupted write has to be repaired from another system.

**Nothing has been run on physical hardware yet — QEMU only.** See
[Running on real hardware](#running-on-real-hardware).

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

`qemu.sh` does the whole cycle: runs `make`, creates a 64 MiB FAT32 image with `mkfs.vfat`,
populates it with `mtools`, copies a fresh OVMF variable store to `/tmp/koi-vars.fd`, and
launches QEMU. Extra arguments pass straight through:

```bash
./qemu.sh -display none     # no window, serial log in the terminal only
./qemu.sh -s -S             # wait for gdb on :1234
KEEP_IMAGE=1 ./qemu.sh      # reuse the existing image instead of rebuilding it
```

The boot log appears **in your terminal** as well as in the QEMU window, because `-serial stdio`
is always on. `Ctrl+C` in the terminal quits.

The script probes the usual OVMF locations for each distribution. If yours is somewhere else:

```bash
OVMF_CODE=/path/OVMF_CODE.fd OVMF_VARS=/path/OVMF_VARS.fd ./qemu.sh
```

`make check` reports undefined symbols (must be empty — a missing `memset` shows up here),
relocations (must be none, or the kernel is not actually position-fixed), and the program headers.

## Running on real hardware

Safe, but not yet useful from a USB stick.

The bootloader records the FAT volume serial of the device it was loaded from, and the kernel
matches on it rather than assuming the first FAT volume it finds is the right one. That
assumption would have been dangerous: USB mass storage is invisible to the AHCI driver, so the
first FAT volume found booting off a stick is the **internal drive's EFI System Partition**, and
`Z:` would have pointed at the real system's boot files.

What happens now, booted from a stick, is that nothing matches — no drive is assigned, the prompt
reads `?:\>`, and the log says `BOOT VOLUME: NOT FOUND`. Correct and harmless, but there is
nothing to do until USB mass storage exists.

Booting from a SATA disk the AHCI driver can see works properly today. A spare drive is the
sensible way to try it; putting Koi-DOS on the EFI partition of a system you care about is not
recommended, since it can write.

A note on keyboards, since the obvious guess is backwards. A **laptop's internal keyboard is
usually PS/2** — it hangs off the embedded controller, which presents itself as an 8042 — so it
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
  keyboard.c           PS/2 keyboard on IRQ1
  acpi.c               RSDP/XSDT walk, hardware-presence questions
  rtc.c                CMOS clock, and FAT timestamp packing
  string.c             mem*/str*, and the glob matcher
  io.h                 port I/O
  timer.c              PIT, polled
  pci.c                PCI enumeration
  ahci.c               SATA/AHCI disk I/O
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
  hello.c cat.c save.c ls.c color.c
legacy/                the old UEFI Boot Services shell, kept for reference
linker.ld              kernel layout, fixed at 1 MiB
qemu.sh                image build + QEMU launch
```

## License

See [LICENSE](LICENSE).

---

**Author**: quartzz720
**Target**: UEFI x86_64
