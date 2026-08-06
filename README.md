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
| `vol`, `date`, `time` | volume label and the clock |
| `mem` | memory, and what the drivers actually found |
| `pci` | every function on the PCI bus, with its class |
| `disk` | disks and partitions, whether or not they have a drive letter |
| `format <part>` | make a new filesystem — destroys everything on it |
| `part <disk>` | replace the partition table — destroys the whole disk |
| `echo`, `ver`, `cls`, `help` | the usual |
| `Z:` | change drive |

`mem` doubles as the system's self-description, because the alternative to a line reading
`USB : keyboard, storage` is rebooting with a serial cable attached to find out whether a driver
came up:

```
Z:\> mem
Physical memory : 2047 MB
Available       : 1978 MB

Kernel image    : 2152 KB
Page tables     : 80 KB
Identity map    : 16386 MB (RAM and device windows)
Heap            : 1023 KB
Heap free       : 1023 KB

PCI devices     : 7
Disks           : 2  ahci0, usb0
Volumes         : 2  Z:, Y:
USB             : keyboard, storage (2 of 8 ports in use)
```

`ver` carries a build number, which during development is worth more than the version — two
kernels both claiming 0.5 differ by whatever happened in between. The number is the commit count,
so it only moves when history does, and a trailing `+` means the tree had uncommitted changes
when that kernel was built:

```
Z:\> ver
Koi-DOS 0.5 Alpha
Kernel 0.5.20, built 2026-08-06 (65230ca+)
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

A third proof lives outside this repository on purpose. **DOSFETCH** — a system summary in the
spirit of neofetch — is built with nothing but the SDK, in its own project. If it stops building,
the SDK is broken for everybody, rather than only for programs that happen to sit in this tree.

```
Z:\> color                 show the presets and the palette
Z:\> color amber           a preset: dos, mono, amber, green, paper, night
Z:\> color /b black        just the background
Z:\> color yellow blue     text and background
```

### Writing programs without the kernel source

[sdk/](sdk/) holds the four files a program needs — the header, the entry stub,
the linker script and the system call definitions — plus a one-line build
script, so a program can be written and built by someone who does not have the
kernel checked out:

```bash
cd sdk && ./koicc mytool.c        # produces MYTOOL.EXE
```

No special compiler: a Koi-DOS program is a freestanding ELF64 binary, and an
ordinary x86-64 GCC produces one. `sdk/README.md` documents the ABI.

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
function number, `RDI`/`RSI`/`RDX`/`RCX` the arguments, `RAX` the result. Twenty calls cover
console I/O, files, directory enumeration, the command line, exit codes, and what the system
knows about itself; they are listed in
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

Working: the boot chain, framebuffer console with an 8×16 CP437 font, exception handling with a
register dump instead of a silent reboot, physical page allocator, `kmalloc` heap, own page
tables, AHCI, GPT/MBR/whole-device partitioning, FAT32 read **and** write with long names, the
command interpreter, programs and system calls.

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

Missing: **graphics**, then audio and networking. There is no `edit` — the line editor is
the one command from the old shell not carried over, and it belongs as a program now that
programs exist. `chkdsk` does not exist, so an interrupted write has to be repaired from another
system. The xHCI interrupt is not routed anywhere, so waiting for a USB key spins rather than
sleeps; USB devices are enumerated once at boot, so plugging a stick in afterwards does nothing
until a reboot; and anything behind a USB hub is invisible.

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

`qemu.sh` does the whole cycle: runs `make`, creates two 64 MiB FAT32 images with `mkfs.vfat`,
populates them with `mtools`, copies a fresh OVMF variable store to `/tmp/koi-vars.fd`, and
launches QEMU. Extra arguments pass straight through:

```bash
./qemu.sh -display none     # no window, serial log in the terminal only
./qemu.sh -s -S             # wait for gdb on :1234
KEEP_IMAGE=1 ./qemu.sh      # reuse the existing images instead of rebuilding them
```

Two images, because one device of a kind never exercises the code that handles two. `esp.img` is
the system volume on an emulated SATA disk; `stick.img` is presented as a USB stick, so the mass
storage driver has something to enumerate and the drive-letter code has more than one volume to
hand out.

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
  keyboard.c           PS/2 keyboard on IRQ1
  acpi.c               RSDP/XSDT walk, hardware-presence questions
  rtc.c                CMOS clock, and FAT timestamp packing
  string.c             mem*/str*, and the glob matcher
  io.h                 port I/O
  timer.c              PIT, polled
  pci.c                PCI enumeration
  ahci.c               SATA/AHCI disk I/O
  nvme.c               NVMe queues, Identify, read/write
  hpet.c               the HPET, as a monotonic clock
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
