# Koi-DOS — Architecture

Koi-DOS is a DOS-like operating system for UEFI machines. "DOS-like" here means the look and
the character, not binary compatibility: drive letters, FAT32, 8.3 names, a familiar command
set, a monolith in ring 0 with no memory protection. Programs, however, are native 64-bit ones
with their own system-call interface — not real `.COM` files from the eighties.

The system volume is **`Z:`**, not `C:`. In MS-DOS, `A:` and `B:` were reserved for floppy
drives, and the system ended up on `C:` only because that was the first letter left over.
Floppies are gone, but handing the system `A:` is not appealing either — hence `Z:`, then `Y:`,
`X:` downward.

## Current state

The kernel boots on bare metal, leaves UEFI Boot Services behind, installs its own descriptor
tables and page tables, mounts the FAT32 volume it booted from, reaches a command prompt, and
runs programs loaded off that disk:

```
KOI DOS KERNEL
MEMORY ALLOC FREE: OK
MEMORY FREE: 1970 MB
CPU: GDT IDT PIC READY
PAGING: IDENTITY MAP 16384 MB, FRAMEBUFFER WRITE COMBINING
HEAP: 1023 KB, SELF TEST OK
KEYBOARD: PS/2 READY
TIMER: PIT POLLING 1000 HZ
APIC: LOCAL AT 0x00000000FEE00000, IO AT 0x00000000FEC00000
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

Z:\> hello
Hello from a Koi-DOS program.
Running on Koi-DOS 0.5
No arguments given.

Z:\> save notes.txt written by a koi dos program
28 bytes written to notes.txt
```

Almost every line of that log is a claim being checked rather than a step being announced. The
memory figure comes from an allocate-and-free round trip, the heap from a self test, and the two
timer lines are one clock measuring another — the second of them is the measurement the polled
PIT could not make at all.

`dir` (with `/w`), `cd`, `type`, `more`, `tree`, `attrib`, `copy`, `del`, `ren`, `md`, `rd`,
`vol`, `mem`, `date`, `time`, `echo`, `ver`, `cls` and `help` all work, plus `Z:` to change
drive. Patterns work where they should: `dir *.exe`, `copy *.txt backup`, `del *.bak`.

Two of those carry more than their DOS namesakes did, and deliberately. `mem` reports what each
driver found — PCI device count, block devices by name, volumes by letter, and which USB class
drivers claimed something — because the alternative to reading that off the screen is rebooting
with a serial cable attached. `ver` carries a build number taken from the commit count, with a
`+` on the hash when the tree was dirty, so a screenshot identifies the kernel that produced it.

The hardware and installation commands are `pci`, `disk`, `format`, `part`, `setup`, `shutdown`
and `reboot`. `disk` lists every partition including the ones with no filesystem this system
understands, because "no drive letter" and "empty" are not the same thing and an installer that
confuses them destroys data. `part` writes a GPT, `format` writes FAT32, and `setup` ties them
together into an installation the machine can boot on its own. The destructive ones require the
partition or disk to be typed back by name before they proceed — `y` is too easy to press by
reflex — and all of them refuse outright to touch the device the system is running from.

`shutdown` and `reboot` go through ACPI. There is no other way: the power button is a request to
the firmware rather than something software can press, and the APM calls are long gone.

Counting installed memory needed an allowlist of EFI memory types rather than "everything that
is not a device window". Firmware uses type 0 for both RAM held back and address space that is
not memory: QEMU describes a 12 GiB reserved region at 1012 GiB, which made a 2 GiB machine
report fourteen gigabytes.

The filesystem reads **and writes**. Reading was proven against a known image first, on purpose:
FAT keeps two copies of its allocation table plus an FSInfo sector, and a half-correct writer
corrupts a volume rather than merely failing. What the guest writes is checked from outside with
`fsck.fat` and `mtools` — see Verification below.

## Boot flow

```
UEFI Firmware
    ↓
BOOTX64.EFI  ← boot/bootloader.c (efi_main)
    ↓
    pick the GOP framebuffer, find the ACPI RSDP
    ↓
    load \BOOT\KERNEL.ELF: parse ELF64, place each PT_LOAD at its p_paddr
    ↓
    allocate the kernel stack, take the final memory map
    ↓
ExitBootServices()          ← no UEFI call is legal past this point
    ↓
kernel_main(BOOT_INFO*)     ← boot/bootloader.c: jump_to_kernel()
    ↓
    serial → memory → console → graphics → GDT → IDT → PIC → sti
    ↓
    paging → heap → ACPI → keyboard → PIT
    ↓
    HPET → Local APIC → interrupt-driven tick → IRQs onto the I/O APIC
    ↓
    PCI scan → AHCI → NVMe → every xHCI controller
    ↓
    block layer → partitions → FAT32 mount → find the system volume
    ↓
    command_run(): prompt, read a line, dispatch
```

`serial_init()` runs before anything else on purpose, and the GDT before the IDT — see the
notes under each module below.

### Why ELF rather than a flat image

The kernel is linked `-no-pie` at the fixed address `0x100000` and contains absolute addresses.
It used to be flattened into `KERNEL.BIN` with `objcopy` and loaded at an arbitrary address via
`AllocateAnyPages` — which worked only by accident, for as long as the compiler happened to emit
nothing but RIP-relative references. On top of that the entry point was taken to be the start of
the image, which holds only while `kernel_main` happens to be the first function in the first
object file. It no longer is: `e_entry` is `0x10004e`, not `0x100000`.

The bootloader now parses ELF64 itself: it allocates the entire `PT_LOAD` span in a single
`AllocateAddress`, **zeroes all of it**, copies the segments in, and jumps to `e_entry`. Zeroing
the whole span is what initialises `.bss`, which has no presence in the file at all.

## Kernel modules

| File | Purpose |
|---|---|
| [kernel/kernel.c](kernel/kernel.c) | Entry point, init order, boot self-test |
| [kernel/serial.c](kernel/serial.c) | COM1 (0x3F8). Mirrors all console output |
| [kernel/cpu.c](kernel/cpu.c) | Our own GDT |
| [kernel/isr.S](kernel/isr.S) | Interrupt stubs for vectors 0-47 |
| [kernel/idt.c](kernel/idt.c) | IDT, interrupt dispatch, panic screen |
| [kernel/pic.c](kernel/pic.c) | 8259 remap and per-line masking |
| [kernel/paging.c](kernel/paging.c) | Our own page tables, 2 MiB identity map |
| [kernel/memory.c](kernel/memory.c) | Physical page allocator over a bitmap |
| [kernel/heap.c](kernel/heap.c) | `kmalloc`/`kfree` over the page allocator |
| [kernel/console.c](kernel/console.c) | Framebuffer text console |
| [kernel/font.c](kernel/font.c) | 8×16 font on the CP437 code page |
| [kernel/graphics.c](kernel/graphics.c) | Drawing primitives, and the screen a program can take |
| [kernel/hda.c](kernel/hda.c) | Intel HD Audio: the codec graph, and one endless output stream |
| [kernel/audio.c](kernel/audio.c) | The mixer: voices, resampling, volume. No hardware in it |
| [kernel/keyboard.c](kernel/keyboard.c) | PS/2 keyboard on IRQ1 |
| [kernel/acpi.c](kernel/acpi.c) | RSDP/XSDT walk, hardware-presence questions |
| [kernel/string.c](kernel/string.c) | `memset`, `memcpy`, `memmove`, `memcmp`, `strlen`, `strcmp` |
| [kernel/io.h](kernel/io.h) | Port I/O: `inb`/`outb`/`inl`/`outl` |
| [kernel/timer.c](kernel/timer.c) | The millisecond tick: PIT while polling, Local APIC once it is up |
| [kernel/rtc.c](kernel/rtc.c) | CMOS wall clock, and FAT timestamp packing |
| [kernel/pci.c](kernel/pci.c) | PCI enumeration; drivers look themselves up |
| [kernel/ahci.c](kernel/ahci.c) | SATA/AHCI, IDENTIFY, `disk_read` / `disk_write` |
| [kernel/nvme.c](kernel/nvme.c) | NVMe admin and I/O queues, Identify, read/write |
| [kernel/hpet.c](kernel/hpet.c) | The HPET, as a monotonic clock rather than a source of events |
| [kernel/apic.c](kernel/apic.c) | Local APIC timer and I/O APIC interrupt routing |
| [kernel/xhci.c](kernel/xhci.c) | USB 3 host controller: the HID boot keyboard, mass storage and RNDIS networking |
| [kernel/ehci.c](kernel/ehci.c) | USB 2.0 host controller: firmware handoff, ports, control transfers |
| [kernel/net.c](kernel/net.c) | ARP, IPv4, UDP, DHCP, DNS and ICMP echo — enough for `ping` |
| [kernel/block.c](kernel/block.c) | Sector-device abstraction over any controller |
| [kernel/partition.c](kernel/partition.c) | GPT, MBR and whole-device volumes; drive letters |
| [kernel/fat32.c](kernel/fat32.c) | FAT32 with VFAT long names, read and write |
| [kernel/command.c](kernel/command.c) | The command interpreter |
| [kernel/config.c](kernel/config.c) | Reads `\BOOT\userspace.cfg` at boot |
| [kernel/program.c](kernel/program.c) | Loads and runs programs |
| [kernel/syscall.c](kernel/syscall.c) | System call dispatch |
| [include/syscall.h](include/syscall.h) | The ABI, shared with programs |
| [programs/](programs/) | Programs and the header they are written against |

### USB

xHCI fails quietly. A controller set up incorrectly does not complain — it simply never posts an
event, and every later step waits for something that will not come. So the driver was built and
verified in slices, each with something observable at the end, rather than written whole and
switched on.

The checkpoint that matters is a **No-Op command**: a command that does nothing, whose completion
event proves the ring layout, the cycle bits, the doorbell and the event ring are all correct
together. Everything after it is about USB rather than about the controller.

Three things the slices caught that would have been miserable to find later:

**The event ring is shared.** Resetting a port posts a Port Status Change event, and it arrives
before the completion of whatever command was issued next. Taking the first event that turns up
therefore reads a port notification as a command result — and because the completion-code field
lands in the same bits for both, it looks like a success carrying nonsense. Commands wait for
their own event type.

**A 64-bit BAR can be assigned far above RAM.** QEMU puts xHCI at 768 GiB, well outside the
boot-time identity map, and the first register read was a page fault. Drivers now ask
`paging_map_device()` for their window before touching it; mapping everything up to 768 GiB
instead would have meant mapping most of a terabyte of nothing.

**`PORTSC` is a minefield of write-1-to-clear bits**, and bit 1 *disables* the port rather than
reporting it. Any read-modify-write of that register has to mask them out.

The firmware handoff capability is honoured before the reset. A BIOS that provided legacy USB
keyboard emulation still owns the controller when we arrive and will fight for it through SMM;
asking for it properly is the difference between a controller that works and one that behaves
erratically for invisible reasons.

The keyboard uses the HID **boot protocol** — the mode every USB keyboard supports so that a BIOS
can drive it without a report-descriptor parser. A boot report is a set rather than an event: it
lists which keys are down right now, so a key counts as newly pressed when it appears in this
report and not the previous one, which is also how repeat is avoided without any timing.

Keys go into the same ring buffer `keyboard.c` fills from IRQ1, so the shell cannot tell which
kind of keyboard produced them.

**Three levels of state, each with its own lifetime.** A machine has several controllers; a
controller has several devices; a device has several endpoints. Every one of those was a
file-scope variable at some point in this driver's life, and every one of them was proved wrong
by hardware rather than by reasoning:

- One device at a time failed the moment a keyboard and a stick were plugged in together.
- One controller at a time failed on the first real machine, which has two — one in the chipset
  and one in the processor — with the keyboard on one and the stick on the other. Which socket a
  device is plugged into is not something the OS has any say in.

That layering is what makes the shared event ring workable. Every event carries the slot and
endpoint it came from, and a wait for one endpoint steps over — and *services* — everything else,
so a keystroke arriving in the middle of a disk read is handed to the keyboard driver and the
read carries on waiting. The controller has to be part of the match as well: slot numbers are
per-controller, and on the two-controller test bench both devices are slot 1, so comparing slot
alone would read a disk completion as a keypress.

**Endpoint zero starts at a guessed size.** Every speed but full has exactly one legal packet size
for the control endpoint; a full-speed device may use 8, 16, 32 or 64 and only says which in the
descriptor that has to be read through that same endpoint. So enumeration reads the first eight
bytes — safe at any size — and issues an Evaluate Context command if the answer disagrees. A
SuperSpeed device reports the size as a power of two rather than a byte count, which is why that
case is excluded rather than "corrected" to nine bytes.

### EHCI, and why it is a separate driver

USB 2.0 controllers, for the machines where the sockets are not wired to xHCI. On a desktop made
in the last decade there is nothing here to do; on a laptop with two EHCI controllers in the
chipset it is the difference between half the sockets working and none of them.

Underneath it is nothing like xHCI. There are no rings and no event ring: there is a circular
list of queue heads the controller walks whenever it has nothing better to do, and a transfer is
finished when the Active bit in its descriptor goes away. Nothing is delivered. The only way to
know anything is to look.

Three things cost more than they look like they should.

**The firmware owns the controller and will not let go unless asked.** It has been driving it
since power-on so that a USB keyboard works in the setup screens, and it does that by watching
the registers through a system management interrupt — which is invisible from here. A driver that
starts writing registers without the handshake is fighting something it cannot see. The exchange
is one bit each way through a PCI capability: set "OS owned", wait for "BIOS owned" to clear.
Then every reason it had to be interrupted is switched off, because an SMI still firing for a
controller nobody is watching is a machine that pauses for no reason.

**EHCI cannot talk to a slow device at all.** A USB 1 device in a USB 2 socket is handled by a
second, physically separate controller sharing that port. A low speed device announces itself
before the reset by driving the line into K-state; a full speed one is only distinguishable
afterwards, by the port failing to enable. Both get handed over, and both are then present,
correct and invisible to this system — which is logged rather than left as an unexplained
absence.

**Alignment is not advisory, and it fails silently.** Every structure the controller reads must be
32-byte aligned. The C struct for a transfer descriptor is 52 bytes — the 32 the hardware defines
plus the upper halves of five buffer pointers — so an array of them puts the second at offset 52,
aligned to four and nothing else. Nothing reports this. The controller reads from the address with
the low bits masked off, lands mid-field, and the transfer never completes. That was the first
bug, and it looked exactly like a device that does not answer.

The second bug is worth keeping too. The transfer queue head was linked into the schedule before
each transfer and unlinked after, which worked exactly once. Taking a queue head out of a running
schedule is not a pointer assignment: the controller may be inside it, and the doorbell handshake
for learning it has left exists precisely because there is no other way to know. A permanently
linked queue head that sits idle between transfers has none of that problem — an idle one has
nothing to fetch and is stepped over.

Enumeration works: ports reset, speeds sorted, descriptors read and validated, addresses
assigned. What does not work yet is anything above that. The keyboard, storage and network
drivers live in [kernel/xhci.c](kernel/xhci.c) and speak xHCI's rings directly, so a keyboard on
a USB 2.0 port is currently a device this system can name and not use. Making it type means
lifting those three drivers onto a transport interface both controllers can implement, which is
the next piece of work and the larger half of it.

### USB hubs

A hub is why a machine whose BIOS counts four keyboards showed this driver one. Everything
plugged into a hub is invisible until somebody asks the hub what it has, and the hub answers
class requests over endpoint zero — one exchange per downstream port, no register anywhere to
read.

**The route string** is how the controller reaches a device through a chain of hubs: four bits
per tier, the downstream port number at each one, five tiers deep and no further, which is also
USB's own limit. It goes into the slot context, and so does the root port the whole chain hangs
off. That is the reason the slot context is now built in one function: it was written out by
hand in four places, one per class driver plus addressing, each of them assuming a device on a
root port because that was the only kind there was. A route added in one and forgotten in the
other three is a device that answers every descriptor and then goes silent the moment a class
driver configures it.

**The transaction translator** is the other half. A full or low speed device cannot talk on a
high speed bus at all: the hub speaks slowly to the device and quickly to the controller, and the
controller has to be told which hub and which of its ports is doing that. A slow device on a slow
hub inherits whatever was already translating further up the chain.

Ports are switched on before they are looked at. A bus-powered hub may come out of reset with
them off, and a port with no power is indistinguishable from a port with nothing in it. The
descriptor says how long to wait afterwards, in two-millisecond units, with a floor under it
because some hubs report an optimistic zero.

Hot-plug is the same poll as the root ports, one tier down: hub ports are asked about their
connection state on every pass, and a hub that loses a port loses everything below it — found by
route prefix, because a device under a hub shares its route in the nibbles up to that tier.

Two tiers of hub are on the test bench, deliberately: a single tier would leave the four-bit
shifting untested. A keyboard sits behind both and a stick behind the first, because *enumerating*
is not *working* — the failure this driver has already had once was a device that answered every
descriptor and delivered nothing. Reading the stick's capacity and mounting its filesystem is a
bulk transfer finding its way through a route string, which is the thing worth proving.

The device lists reuse released entries. They only ever grew before, because a device only ever
arrived; plugging the same stick in thirty-two times would then fill the table with devices that
are not there, and nothing would notice until the thirty-third — where it would look like a limit
rather than a leak.

### USB mass storage

Bulk-only transport, which is what every USB stick made in the last two decades speaks: a 31-byte
command block out, an optional data stage, and a 13-byte status block in. The SCSI command sits
inside the command block, and a tag ties the three phases together — a status block carrying
somebody else's tag means the two ends have lost sync, which is worth detecting rather than
papering over.

`INQUIRY` is the checkpoint here, the way the No-Op was for the controller: a vendor string coming
back proves the whole transport works before any sector is at stake.

**Transfers bounce through a page of the driver's own.** A TRB's buffer may not cross a 64 KiB
boundary, and the block layer above hands down pointers from anywhere. A page-aligned bounce
buffer costs a copy per chunk and makes the rule impossible to break.

**A stall is not a failure.** It is the device saying no to one endpoint, and it stays halted until
both ends agree it is clear: the controller through Reset Endpoint and Set TR Dequeue Pointer, the
device through `CLEAR_FEATURE`. A stalled data stage still owes a status block, so recovery reads
one rather than abandoning the command. (This path is written but untested — QEMU's emulated
storage never stalls.)

A device that has just been given power answers "not ready" while it spins up, and holds a pending
condition that makes it refuse everything until somebody reads it. `TEST UNIT READY` and
`REQUEST SENSE` in a loop are that handshake; skipping it is a common way to make a working stick
look dead.

The result registers through `block.c` as an ordinary disk, so `partition.c` and `fat32.c` never
learn what kind of controller they are reading. Booting from a stick now assigns `Z:` to the stick
even with an internal disk carrying its own EFI System Partition — verified in QEMU with both
present.

### One of everything, which was the bug

Three drivers each held one device in file-scope variables, and each of them was saying something
untrue about the machines this runs on: one AHCI controller with one disk on it, one NVMe drive
with namespace 1, one USB stick. A desktop board has six SATA ports and people fill them; two M.2
sockets is ordinary; and the USB driver *said out loud* "a second storage device is present and
ignored", which is a strange thing for an operating system to tell somebody holding two sticks.

The missing disks were not failing. They were never being looked at — which is the harder failure
to notice, because nothing in the log is wrong.

AHCI became a table of disks, each holding its own port and command structures. The controller's
64-bit addressing flag moved in with them: two controllers can differ on it, and picking the wrong
answer is a DMA write to a truncated address.

NVMe and USB storage took a different shape, and it is worth saying why. Both have a register or
protocol layer threaded through thirty functions, and passing a context to every one of them buys
nothing here: neither is re-entrant, neither is interrupt-driven, and every completion is polled
by whoever submitted it. So each keeps a "current device" pointer set at its two entry points —
bringing a controller up, and a block transfer arriving from the layer above. `driver_data` on
the block device carries which one, which is what that field is for.

NVMe also walks its namespaces rather than assuming namespace 1. A namespace that is not there
answers Identify with zeroes rather than an error — the controller saying "no such thing" in the
only way the command allows — so absence is detected by a sector count of zero and reported as
absence, not as a driver that cannot cope with a block size of 2⁰.

The bench grew to match: a second SATA disk, a second NVMe drive and a second USB stick, none of
which existed while "the first one" was the only one any of this could find.

### Storage stack

Four layers, each ignorant of the one below it:

```
command.c        dir, cd, type
   ↓
fat32.c          clusters, directory entries, long names
   ↓
partition.c      GPT / MBR / whole device  →  volumes Z:, Y:, X:
   ↓
block.c          block_read(device, lba, count, buffer)
   ↓
ahci.c           SATA command FIS, DMA
xhci.c           SCSI over bulk-only transport
nvme.c           submission and completion queues
```

`block.c` earned itself twice over: USB mass storage and then NVMe both arrived as new drivers
under the same interface, and nothing above it changed either time.

NVMe was the quickest of the three, because its shape was already familiar — a ring of 64-byte
commands, a doorbell saying how far it has been filled, and a completion ring with a **phase
bit** marking whose entries are whose, which is the xHCI cycle bit under another name. Identify
Controller is its checkpoint, the role `INQUIRY` plays for USB storage: a model string coming
back proves queues, doorbells and phase bit together before a sector is at stake.

Two details that bite. A read or write command carries its block count **one less than it is**,
so zero means one block and a command can never ask for nothing. And a namespace's block size is
a base-two logarithm inside whichever of several declared formats it is currently using — read
the wrong format's entry and the number is plausible and wrong by a factor of eight. It also range-checks against the sector count
IDENTIFY reported — reading past the end of a disk returns garbage rather than an error on some
controllers, which becomes a baffling filesystem bug three layers up.

### Partition tables, in the order the formats demand

GPT is checked before MBR, and the reason is the **protective MBR**. A GPT disk carries a fake
MBR in sector 0 describing one partition of type `0xEE` spanning the whole disk, put there so a
pre-GPT tool sees a full disk and declines to repartition it. Looking at the MBR first would
therefore find one enormous partition of an unrecognised type on every modern disk.

A third case has no partition table at all — a filesystem written directly to the device.
Quick-formatted USB sticks look like this, and so does the `esp.img` that `qemu.sh` builds.

Each candidate region is checked for a plausible FAT boot sector before it is given a drive
letter, so a swap partition never becomes `Y:`.

**The boot volume is identified, not guessed.** The kernel cannot work out on its own which
device the firmware loaded it from — it sees block devices through its own drivers, which know
nothing about the firmware's choice. So the bootloader reads the FAT volume serial from the
device it was loaded off and passes it in `BOOT_INFO`; the volume scan matches on it.

Guessing "the first FAT volume found" is right in a one-disk virtual machine and dangerous on a
real one. Booted from a USB stick, the first FAT volume the AHCI driver can see is the internal
drive's EFI System Partition — so `Z:` would be the real system's boot files, with `del` and
`copy` pointed there. When no volume matches, none is assigned, the prompt reads `?:\>` and the
boot log says so. Refusing is the only safe answer; there is nothing sensible to fall back to.

Now that USB mass storage exists this is more than a precaution: booting from a stick with an
internal disk also present puts `Z:` on the stick and the internal EFI System Partition on `Y:`,
which is what the serial match was written for.

### Long file names

Supported in full, read and displayed. The 8.3 limit was a consequence of 1980 hardware rather
than a design goal, and Koi-DOS is what DOS would look like if it turned up on a UEFI machine
today. Short names still exist underneath — every entry has one — but they are an implementation
detail rather than what the user sees.

Long-name entries are stored in reverse, last fragment first, and are tied to the short entry
that follows them by a checksum over its 8.3 name. A mismatch means the long name belongs to a
file that was deleted and partially overwritten, and the fragment is discarded rather than
trusted.

Paths use backslashes. Forward slash is not a separator: in DOS it introduced command switches
(`dir /w`), and that is a distinction worth keeping.

Creating a name is harder than reading one. Every entry needs a unique 8.3 name underneath —
`a long copied name.txt` becomes `ALONGC~1.TXT` — and the numeric tail has to be checked against
the directory rather than assumed, so `make_short_name()` is called in a loop until
`short_name_taken()` says no. Characters FAT forbids become underscores rather than being
dropped, so two different long names cannot collapse onto the same short one.

### Writing

One invariant governs the write side: **every copy of the allocation table is written, always**.
FAT32 keeps two by default, and a volume whose second copy disagrees with the first is one that
`chkdsk` — or the next system to mount it — calls corrupt. The held sector belongs to every copy
at once and `fat_flush()` writes it to all of them, so the copies cannot drift apart by any route
through this code. The top four bits of each entry are reserved and are preserved rather than
zeroed.

The FSInfo free-cluster hint is refreshed whenever it actually moves. It is advisory, since the
FAT is authoritative, but a stale value makes other systems report nonsense free space.

Two ordering decisions worth knowing:

- A newly allocated **directory** cluster is zeroed before use — it still holds whatever was
  there before, and that would read as a screenful of garbage entries. A file's cluster is not:
  nothing can read past a file's recorded size, and clearing every cluster before writing over it
  would mean writing the whole volume twice.
- `fat32_rename()` writes the new directory entry **before** erasing the old one. If the machine
  dies in between, the result is one file reachable by two names — recoverable. The other order
  would lose the data outright.

### Speed, which turned out to be a correctness question

Four things here were slow enough to look like faults, and none of them could show on a 64 MB
test image. They are recorded because each is a shape worth recognising rather than a number
worth tuning.

**The cluster size was chosen backwards.** The old rule was "the smallest cluster that is still
legal FAT32", on the argument that a small cluster wastes less on a short file. That argument
stops holding somewhere around a gigabyte. A 223 GB partition came out with 1 KiB clusters — 234
million of them, described by a table 936 MiB long, of which FAT32 keeps two. Nothing downstream
recovers from that. It is now chosen from the volume's size the way every other formatter does
it: 32 KiB above 32 GB, which turns those 234 million clusters into 7.3 million and that table
into 55 MiB for the pair.

**Free space was counted by walking the whole table, and the count was discarded constantly.**
`write_fat_entry` invalidated it on every allocation while the FSInfo refresh asked for it after
every write, so copying one file rescanned the entire table once per cluster. It is the same
reason `dir` used to freeze for a minute: it prints the listing, *then* asks for the free figure.
The count is now adjusted by one cluster when an entry changes, seeded from FSInfo at mount so
the full count usually never runs, and the count that remains reads 64 KiB per command.

**One sector of the allocation table is held in memory per volume.** 128 clusters share a
512-byte sector, so a chain walks over the same sector again and again; going to the disk for
each entry cost four commands per cluster for a sector that was about to be needed again. The
cache is write-back, and every public operation that can dirty it flushes before returning — so
the disk is only ever allowed to be behind *within* one operation, never between two.

**Whole sectors go straight to and from the caller's buffer**, as many at a time as the cluster
holds, rather than through a scratch sector one at a time. Only a partial sector at either end is
bounced. Every driver underneath splits a long request itself, so the block layer was always
willing; the filesystem simply never asked.

Together: a 16 MB file from a USB stick to NVMe did not finish in three minutes before, and takes
five seconds now.

### Verification

The guest's writes are checked from outside, because a filesystem that only satisfies its own
reader proves nothing:

```
./qemu.sh                 # write something from the Koi-DOS prompt, then quit
fsck.fat -n -v esp.img    # must report no errors at all
mdir -i esp.img ::/       # must show what the guest created
```

`KEEP_IMAGE=1 ./qemu.sh` reuses the existing image instead of rebuilding it, which is how you
check that what the guest wrote survives a reboot. The bootloader and kernel are refreshed even
then, so it never means booting a stale build.

### Interrupts

The GDT is installed before the IDT, because every gate stores a code selector and it has to be
one of ours rather than one the firmware left behind. The stubs in `isr.S` push a zero in place
of the error code for the exceptions that do not supply one, so the C handler always sees the
same frame layout.

Remapping the 8259s is not cosmetic. They power up delivering IRQ0-7 on vectors 8-15, which in
long mode are CPU exception vectors — a timer tick would arrive looking like a double fault.
`pic_init()` moves them to 32-47 and masks every line; drivers unmask their own as they come up.

An unhandled exception paints a panic screen with the vector, error code, `CR2` and a register
dump, and writes the same thing to COM1. Before this existed, any fault triple-faulted into a
silent reboot.

### Time, and which timer does what

Two clocks, with a deliberate division of labour that is the standard one. The **Local APIC timer
is the source of events**: on the processor die, so programming and reading it is cheap, and
per-CPU, which means its tick would need no locking if this ever grew a second processor. The
**HPET is the source of time**: out on the chipset, so every counter read is a slow bus round
trip, but monotonic and independent of the processor's clock — exactly what is needed to measure
how fast the APIC timer actually runs, since that frequency is not discoverable on AMD hardware.

Neither is trusted blindly. The HPET is refused if its declared tick period is implausible or its
counter is not advancing — a stopped counter is worse than no counter, because every delay
written against it hangs forever. And the two clocks are checked against each other at boot: a
tick period misread by a factor of a thousand looks perfectly reasonable in isolation and is
obvious the moment it is compared.

**The polled PIT was losing time, and the comparison is what found it.** A 16-bit counter
reloading every millisecond can only show movement inside one period, so any stretch of work that
did not stop to poll subtracted itself from the clock — 100 ms once read back as 0. The
interrupt-driven tick is the fix, and the same measurement now reads 100.

Switching over has to happen all at once. The end-of-interrupt must follow the delivery, or the
APIC believes an interrupt is still in service and stops delivering that priority; the keyboard
IRQ has to move to the I/O APIC in the same breath or it silently stops arriving; and the 8259 is
masked entirely rather than left half-live.

Three things the current code is honest about not having: the xHCI interrupt is not routed, so a
USB keystroke is collected on the timer tick rather than delivered; USB devices are enumerated
once at boot, so plugging a stick in afterwards does nothing until a reboot; and devices behind a
**hub** are invisible, because the route string is hardcoded to zero.

### Console

Text is drawn into a back buffer in ordinary RAM and copied out a rectangle at a time. Scrolling
is why: it moves the whole screen up one line, and reading back across PCIe is orders of
magnitude slower than reading RAM. With a back buffer the move is a `memmove` and only the write
direction ever touches the device. If the buffer cannot be allocated, drawing falls back to the
framebuffer directly — degraded, never dark.

The framebuffer is mapped **write-combining**, through a PAT entry reprogrammed for it at boot.
It used to be write-through, which is also correct - the pixels reach the adapter either way -
but write-through sends every store across the bus on its own, and a full screen is a million of
them. Measured at 7.8 ms a frame, which is half the budget of anything that animates, before a
single pixel had been drawn. Write-combining lets the processor gather stores into whole
cache-line bursts first, and the same frame costs 1.2 ms. The boot log says which one the machine
ended up with, because a machine where it failed would otherwise show up only as everything
graphical being mysteriously slow.

The copies themselves are `rep movsq` and `rep stosq` rather than C loops. A loop over a volatile
pointer is the slowest thing that can be written: volatile forbids the compiler from merging
anything, so it emits one four-byte store per pixel. Clearing the screen that way measured as the
most expensive thing in a frame - more than sending the frame to the adapter - and became six
times cheaper as one instruction.

The UEFI pixel-format names describe *byte* order, not the value of the 32-bit word.
`PixelBlueGreenRedReserved` means bytes B,G,R,X, which on a little-endian machine reads back as
the familiar `0x00RRGGBB`. Reading that backwards swaps red and blue.

The font in `font.c` was written for this project rather than imported: the bitmap fonts that
ship on a typical Linux box are GPL, which does not fit this project's licence.

### Graphics

There is no mode switching, and there cannot be. UEFI chose the resolution before
`ExitBootServices` and the GOP is gone afterwards, so "graphics mode" here means the console
stops drawing and something else starts. A program calls `graphics_enter`, gets a buffer the size
of the display, draws, calls `graphics_present` to put a finished frame up, and `graphics_leave`
to give the screen back.

Three parts of that handover are the design rather than details, and each was a defect first:

- **The console keeps its back buffer the whole time.** Leaving is a repaint, not a redraw.
  Nothing the shell had on screen is lost, and text printed *during* graphics mode simply appears
  when the screen comes back.
- **`present()` in `console.c` returns early while a program holds the screen.** Without it the
  caret alone blinks a hole in every picture — waiting for a keypress is the last thing a
  graphics program does, and showing the caret is what waiting does.
- **The shell takes the screen back whether or not the program did.** A program that returns
  while still holding it would leave the shell invisible with no way to ask for it back. That is
  the failure DOS programs were famous for, and it is prevented in the one place that always
  runs.

`graphics_present_rect` sends one rectangle rather than the whole screen, and it matters more than
anything else here. The screen is whatever size the firmware chose; a program that uses a corner
of it and sends all of it sixty times a second spends more on that than on everything else put
together. The games collection draws into 640x480 of a 1280x800 screen, and pushing only that is
a third of the work.

One thing that follows from it, learned by screenshot rather than by reasoning: a program that
only ever sends its own corner has to send the whole screen **once**, or the console's text is
still sitting in the part it never touches. Entering the mode clears the buffer; something has to
make the clearing visible.

Primitives live in the kernel and are reached by system call, and the program is *also* handed a
raw pointer to the buffer. Both, deliberately: the calls mean a program need not know how a pixel
is laid out, and the pointer means it need not pay a system call per pixel when it does know.
This is a ring-0 monolith; pretending the buffer is out of reach would only make drawing slow
without making anything safer.

Colours are built by `graphics_color` rather than assembled by hand, for the reason the console
section gives — the channel order differs between machines, and code that guesses is wrong on
half of them. The gradient across the top of `demo` exists to make exactly that visible.

Every primitive clips. Drawing off the edge is what a program does while it is being written, and
a layer that treats it as an error either refuses to draw anything or writes past the buffer.

`show` reads BMP because it is the one image format that needs no decoder: no compression to
undo, no entropy coding, no tables. PNG would need inflate and JPEG a discrete cosine transform,
and neither belongs in the first thing that puts a picture on a screen. `BI_RGB` and
`BI_BITFIELDS` with the ordinary BGRA masks are read; anything else says what it is and stops,
because an image drawn from a format that was not understood is worse than no image. The file is
read forwards exactly once, with no seeking, which a BMP does not need.

Both were checked by screenshot taken from outside the guest — QEMU's `screendump` writes a PPM —
rather than by looking at them.

### Sound

Two files, and the split is the point. [kernel/hda.c](kernel/hda.c) knows about hardware and
nothing about sound; [kernel/audio.c](kernel/audio.c) knows about sound and nothing about
hardware. A second sound device would be a third file that fills a ring, not a second mixer.

**HD Audio rather than AC'97.** AC'97 is a much smaller driver and QEMU emulates it, which is
exactly what makes it a trap: no machine that could run this has one. The audio device in the
desk this was written at is an AMD HDA controller, and so is every laptop made since about 2005.

The controller is the easy half — two rings and a stream. The codec is not the controller: it is
a little graph of widgets, converters and mixers and selectors and the physical jacks, and
getting sound out of it means finding a route through that graph and unmuting every step. One
muted amplifier anywhere on that path is indistinguishable from a driver that never ran.

**Choosing the output is a question about laptops.** Ranking the pins by what the codec calls
them — line out, then headphones, then the built-in speaker — is right on a desk and wrong on a
laptop, where it picks the headphone socket and leaves the machine silent with nothing plugged
into it. What decides is whether anything is connected, asked of the pin itself with
`GET_PIN_SENSE`; the name only breaks ties:

| | |
|---|---|
| a jack with something in it | 7 — headphones win when they are being worn |
| the built-in speaker | 4 — always there, so always usable |
| a jack that cannot report | 3 — guessing "empty" would silence a machine that works |
| a jack reporting nothing in it | 0 — no point sending sound where nobody is listening |

If every pin scores zero the best one is driven anyway, because a socket nobody is using is
silent either way and reporting "no output" on a machine that plainly has one is worse. External
amplifiers get switched on where the codec has one (`EAPD`): a laptop whose speakers are wired
through one is silent without it while every register reads correctly.

From the chosen pin the driver walks the connection lists backwards until it reaches a converter,
unmuting behind itself and telling each selector which of its inputs to pass. A controller may
carry more than one codec and the first to answer is not necessarily the one the speakers are on,
so a codec with no analogue output is skipped rather than being the end of it — which is also
what happens to the HDA controller inside a graphics card, whose outputs are all HDMI.

`sound` prints the whole table, because "there is no sound" has several completely different
causes that are identical from a chair.

**The bug that only real hardware could show.** On the first machine that was not QEMU, the
controller came up, the codec answered a hundred verbs, the widget walk found the built-in
speaker and its converter — and the stream started and never moved. Every stream register read
back exactly what was written; no error bit was set; the FIFO never became ready; all four output
streams behaved identically. The bus was clean: memory space and bus mastering enabled, traffic
class selector zero, controller out of reset.

One bit never matched. `SDnCTL`'s traffic priority bit read set having been written clear, as a
byte and as a dword, on every descriptor — it is not writable on that controller. That bit marks
the stream's transfers as a different class of traffic, and the pairing that goes with it, in
every driver that sets it deliberately, is that those transfers **do not snoop the processor's
caches**. Linux sets it exactly when snooping is off, and allocates its buffers uncached in the
same breath.

That is the whole shape of the symptom. The command rings worked because they are not a stream
and the bit does not apply to them. The stream read its descriptor list from memory, and the
descriptor list had never left the cache — so it read a list of zero-length buffers, transferred
nothing, and reported no error, because a buffer of zero bytes is not an error.

`cpu_flush_cache()` — `mfence`, `clflush` by line, `mfence`, with the line size read from CPUID
rather than assumed — is applied to the descriptor list, the ring and the position buffer. Two or
three cache lines per millisecond as the mixer fills ahead of the hardware. Where DMA does snoop
it costs those three lines and changes nothing.

**Presence detection needs the pin powered.** The same machine reported its headphone sockets
empty while headphones were in them: only the chosen pin was being powered, and the choosing
happens after every pin has been asked. A sleeping pin answers, and answers "nothing there". Each
pin is powered before it is asked now, and triggered first where it says its measurement is not
continuous.

**The bug worth writing down.** The controller stops fetching commands once it has produced
`RINTCNT` responses, and what releases it is software clearing the response bit in `RIRBSTS`. But
it only ever *sets* that bit if the response interrupt is enabled — so with interrupts off, as
they are here, the bit never appears, clearing it does nothing, and the command ring stalls after
exactly one verb. The symptom was a codec that answered once and then went quiet, which reads
like a mapping or ring-layout mistake and is neither. The fix is to enable the response interrupt
while leaving the global interrupt enable off: the bit gets set, we clear it, the count resets,
and no interrupt is ever delivered.

The stream is started once and never stopped. Starting one per sound is where clicks come from,
and there is nothing to gain: an idle stream plays whatever silence the mixer wrote.

**The mixer runs from the timer interrupt**, which is the one design decision here worth arguing
about. Mixing from wherever the system is looping is simpler and works right up until a program
stops making system calls, at which point sound stops with it — a game rendering a frame is
exactly that case. Forty-eight frames a millisecond is a few microseconds of arithmetic. Voices
are edited with interrupts held, and released to exactly what they were rather than unconditionally
enabled, because start-up and interrupt handlers both reach this code with interrupts already
off.

Sixteen voices, each with a rate, a volume and a pan, resampled in 32.32 fixed point. No floating
point anywhere: programs and the kernel are both `-mgeneral-regs-only`, since nothing configures
SSE state after `ExitBootServices`. Nearest-sample rather than interpolated — against effects
recorded at 11 kHz in 1993 the difference is inaudible and the cost is a second fetch per voice
per frame.

**A voice holds a pointer into the program's own memory and is not copied.** That is what makes
firing the same effect twenty times a second free, and it is why every voice is stopped when a
program exits: left running, the mixer would keep reading an address that now belongs to
something else, a thousand times a second, forever.

A voice already playing can be retuned - `SYS_SOUND_PARAMS`. That call exists because DOOM asked
for it: the game adjusts every sound's volume and stereo position every tic as the player moves,
and without it a rocket stays where it was fired.

Verified from outside the guest rather than by ear, because a file can be measured and a noise in
the room cannot. QEMU records what the machine played, and four things were checked in it: a tone
asked for as 440 Hz for one second measures as 440 Hz for 1.000 seconds; a tone held across a
graphics program starting, drawing and exiting has not one silent 20 ms block in it; six pistol
shots in DOOM land at the six moments the keys were sent, each the length `DSPISTOL` is at
11025 Hz, since a wrong sample rate would show as a wrong duration; and a voice panned across
while playing is silent in the far channel at each end and full in both at the centre.

### Networking

The network device is a phone. That is not a limitation being apologised for, it is the design:
this runs on two machines, neither of which has an Ethernet socket, and both of which have a USB
port and somebody holding a phone. A phone sharing its connection presents itself as a USB
network adapter speaking RNDIS, so RNDIS is what is driven.

**RNDIS is a remote procedure call wearing a network adapter's coat.** Configuration does not
happen through descriptors; it happens by sending Microsoft's NDIS messages *inside control
transfers* and fetching the answers with a second control transfer that has to be asked for
separately. Every message carries a request id and the reply carries it back, which is the only
thing distinguishing an answer to this question from an answer to the last one.

Three mistakes on the way there, each with a symptom that pointed somewhere else:

- A message byte-for-byte correct was stalled, because a class request is addressed to an
  *interface* and a device with no configuration selected has no interfaces yet.
- The reply's fields were read one field early, which took the device flags for the medium and
  announced that an ordinary Ethernet adapter was not Ethernet. The offsets are written out in a
  comment now.
- A query carried an information-buffer offset pointing one byte past the end of a message with
  no information buffer in it. A query sends no data; it asks for some, and the offset belongs at
  zero. The phone let this pass and QEMU's adapter stalled it — the emulator was the stricter of
  the two and the correct one.

**A stalled control endpoint stays stalled**, and clearing it is the controller's business, not
the device's. The first attempt sent CLEAR_FEATURE to the device, which cannot work: the only
road to the device is the endpoint that is halted. What the log showed was every request after
the first failing, each with a different message, none of them naming the one that actually went
wrong. Reset Endpoint plus Set TR Dequeue Pointer puts the endpoint back; USB clears endpoint
zero on the device side at the next SETUP without being asked.

**Frames go inside a 44-byte header** whose real content is where the frame starts and how long
it is. The device batches several into one bus transfer, up to a limit *we* state in the
initialize message — so that limit is one page, because one page is what the receive buffer is
and a device that sends more arrives as babble on an endpoint that then has to be reset. One
receive request is kept outstanding at all times: a bulk IN endpoint hands over nothing until it
is asked, and an endpoint with nothing queued is a host that has stopped listening. Completions
are copied into a queue rather than parsed in place, because re-arming overwrites the page.

Above that, [kernel/net.c](kernel/net.c) is ARP, IPv4, UDP, DHCP, DNS and ICMP echo, and stops
there. No TCP, no sockets — those belong to whichever program first needs them. Addresses are
held in host order throughout and swapped only at the wire, so the byte swaps live in four
functions instead of being scattered where a wrong one looks like an address nobody recognises.

Routing is one comparison: an address on this wire goes to its own hardware address, and
everything else goes to the gateway's, with the packet's own destination unchanged. There is one
neighbour cache entry, because there is one gateway.

`ping google.com` exercises the whole stack at once — DHCP for the address, ARP for the gateway,
DNS over UDP for the name, ICMP for the echo — which is why it is the test:

```
Z:\> net start
Asking for an address...
Address 10.0.2.15, gateway 10.0.2.2
Z:\> ping google.com
Pinging google.com [142.251.127.139] with 32 bytes of data:
Reply from 142.251.127.139: time=48ms
```

QEMU's user-mode network is a real DHCP server, a real resolver and a path to the outside world,
so all of that is testable without a phone plugged in — `qemu.sh` attaches one by default.

### Keyboard

IRQ1, scancode set 1, with shift, ctrl, alt and caps lock. `keyboard_getchar()` idles on `hlt`
rather than spinning, so an idle guest stops burning a host core.

**Two queues, not one.** Characters go in one; key-down and key-up events go in the other. The
shell wants to know what was typed; a game wants to know what is being *held*, which a character
stream cannot express at all — it has no idea a key is still down and no idea when it stopped
being. Both drivers could always see releases and threw them away one line before delivering
them.

Mixing the two into a single stream with a flag would break both ends: line editing would find
release events where it expected characters, and a game would find shifted characters that never
match what it saw go down. So the event queue reports the *unshifted* identity, and a key reads
the same in both directions — `a` is `a` whether or not shift was held. Draining one queue
consumes nothing from the other, so the shell and a program can each have what they need.

`keyboard_init()` reports three outcomes, not two, and the middle one is the interesting case:
no controller, a controller with a keyboard that answered, or a controller with nothing attached.

It asks ACPI whether an 8042 exists before touching a port. On a machine with no
PS/2 controller, reads from port 0x60 return `0xFF`, and every sanity check in the init sequence
would "pass" on garbage. When the FADT is absent or too old to carry the flag, the answer is yes,
which is right for the machines that predate it.

But the ACPI flag describes the **controller**, not the socket. An 8042 exists in the chipset's
silicon whether or not the board wired it to a connector, so plenty of modern desktop boards
declare one and have nowhere to plug a keyboard in. To tell the difference the driver resets the
device on port 1 and waits for the two replies a keyboard makes — `0xFA` to acknowledge, then
`0xAA` when its own power-on test finishes. An empty socket says nothing. Without that probe the
driver announces a keyboard that cannot exist.

Which machines actually have one is the opposite of the obvious guess. A **laptop's internal
keyboard is usually PS/2**: it hangs off the embedded controller, which presents itself as an
8042, and that is still true of recent machines. A **desktop with a USB keyboard is the harder
case** — the firmware's legacy emulation generally stops working once boot services are gone. A
desktop board with a PS/2 port and a PS/2 keyboard plugged into it works today.

### Paging

The kernel builds its own PML4 from 2 MiB pages. UEFI is required to identity-map memory while
boot services are alive, but nothing obliges the firmware to keep those tables valid afterwards.

The first 4 GiB is mapped unconditionally — PCI windows, the local APIC and the HPET live in
holes the memory map does not describe — and then each descriptor is mapped on its own. Mapping
straight from zero to the highest descriptor instead would swallow the gap between the top of
RAM and the 64-bit PCI window, a terabyte of it on QEMU's q35, and a pointer strayed into that
gap would silently succeed rather than fault.

### The log, for machines with no serial port

Every driver reports what it found and why it stopped over COM1, and it is the only channel that
survives a failure before anything can be drawn. It is also absent from every machine made this
century — and a laptop is precisely where the drivers meet hardware that QEMU never emulates, so
the explanation is lost exactly where it is needed. An HD Audio controller that would not start
is what prompted this: the reason existed, in a register write nobody could see.

So the same bytes are captured into 64 KiB of `.bss` on the way past, before the port is even
consulted, and `log` prints them or writes them to a file. The buffer fills and then stops rather
than wrapping: the interesting part of a boot log is the beginning, and a ring eats that first.

Drivers now keep their failure reason as well as printing it, so `sound` can say *why* there is
no sound rather than only that there is none, and the boot line says it too. On a machine with no
serial port that one line is the entire explanation there will ever be.

### COM1 comes up first

`serial_init()` runs before everything else, memory initialisation included. If the kernel dies
before it can draw anything, the serial port is the only witness left. QEMU exposes it through
`-serial stdio`, and `qemu.sh` always passes that.

Initialisation performs a loopback test and records the result in `serial_present`. Without it,
a machine with no physical COM1 would make every `serial_putchar()` wait forever for a
transmit-empty bit that never arrives — the debug channel would hang the system it exists to
diagnose.

### Memory

`memory_init()` builds a bitmap: one bit per 4 KiB page, so 2 MiB of bitmap covers 64 GiB of
physical memory. Only `EfiConventionalMemory` pages start free; everything else starts taken.
It then explicitly reserves page zero, **the whole kernel image including `.bss`**
(`kernel_image_base`/`kernel_image_size`), the kernel stack, the framebuffer, and the memory-map
buffer.

Reserving the image rather than the file size is the whole point: `.bss` is 2 MiB and does not
appear in the file, so reserving by file size would leave the allocator handing out the kernel's
own memory as free.

`alloc_page_low()` returns a page strictly below 4 GiB. Devices with 32-bit address registers
need this — in AHCI the command list, FIS area and command table all have them.

### Disk I/O

`pci_find_ahci()` looks for a class `01:06:01` controller, `ahci_init()` enables bus mastering,
configures the port holding an ATA disk, and starts the command engine. Transfers use
`READ/WRITE DMA EXT` with polling on the `CI` bit.

DMA addresses are written in full, both halves. Bit 31 of the `CAP` register says whether the
controller supports 64-bit addressing; when it does not, a buffer above 4 GiB is rejected
outright rather than silently truncated to 32 bits.

### Programs

A program is an ELF64 image linked `-no-pie` at 16 MiB — the same trade the kernel makes, for
the same reason. One program runs at a time, in ring 0, in the same address space, so there is
nothing to relocate around. `memory_init()` reserves that window so the page allocator never
hands out memory a program is about to be loaded into, and the loader refuses any segment that
falls outside it. With no memory protection, that bounds check is the only thing between a
malformed file and the kernel.

Programs get their own stack at the top of the window. A runaway program wrecks that one rather
than the kernel's, and with the double-fault stack in place even that is reported instead of
rebooting the machine.

The shell tries its built-in commands first and treats anything left over as a file to load,
appending `.EXE` if the user did not. The search order is the current directory, then the root of
the drive, then `\BIN` — a three-entry PATH, which is as much as a system without one needs.
`\BIN` is where an installation puts them: the root of the system volume is for the user's files,
and a pile of `.EXE` in it turns `dir` into a search. Built-in commands stay built in; only
programs live there.

### Wildcards

`glob_match()` lives in `string.c` rather than the shell, because the directory-enumeration
system calls need exactly the same answer and two implementations would eventually disagree.

`*` matches any run of characters and `?` exactly one. DOS gave `*` a narrower meaning — it
padded the 8.3 field with question marks, so `*.TXT` was really `????????.TXT` and could not
match a name with two dots. With long names that reading only surprises people.

The matcher is iterative, not recursive: a pattern of alternating stars against a long name
would nest a recursive matcher hundreds of frames deep, on a kernel stack.

`del` with a pattern asks first, the way DOS prompted on `del *.*`, and deletes by finding one
match at a time and starting over. That costs a pass per file, but it avoids reasoning about what
a directory walk does to entries vanishing underneath it.

### System calls

`int 0x40`, in the spirit of DOS's INT 21h. The vector is not `0x21` because in protected mode
that one is taken: the 8259s are remapped to 32-47, which puts the keyboard IRQ exactly there.
DOS never had the collision — it lived in real mode.

A software interrupt rather than `SYSCALL`/`SYSRET` is deliberate. The whole value of `SYSRET` is
a fast ring 3 → ring 0 transition, which does not happen in a ring-0 monolith. `INT` needs three
fewer MSRs configured and gives an ABI that does not depend on where the kernel is linked.

The gate is a **trap** gate, not an interrupt gate, so system calls run with interrupts enabled.
That matters for `SYS_EXIT`, which never returns through `iretq`: it unwinds straight back into
the shell through a saved resume point, and an interrupt gate would have left interrupts off
with nothing to turn them back on.

`RAX` carries the function number and `RDI`/`RSI`/`RDX`/`RCX` the arguments — one register out of
step with the C convention, so the stub shifts them all along by one. The shifts have to run in
that order; each reads a register the next overwrites. Getting that wrong was a real bug during
development, caught because the fourth argument silently destroyed the third.

The calls cover console I/O, file open/read/write/close, directory enumeration, arguments, exit,
what the system knows about itself, and taking the screen. `ls.EXE` exists to prove the
enumeration calls are enough to write a directory listing as an ordinary program, with no
privilege the shell does not also lack.

Relative paths resolve from where the shell was standing, not from the root of the drive. The
syscall layer is told both the volume and the directory — `syscall_set_location` — because being
told only the volume is what made `ls` list the root wherever the user was, and `cat FILE.TXT`
work only from it.

The convention carries four arguments and a line needs five numbers, so a graphics point travels
as a pair packed into one 64-bit word. Widening the convention for one call would have left every
other call working around the change; the SDK wrappers hide the packing entirely, and a program
written against them never sees it.

**Programs are stamped with the interface version they were built against**, in a `.koi_abi`
section the linker script places at the load address, and the kernel reads it before deciding to
run anything. The check runs in both directions, and the second half is the surprising one. A
program built for a *newer* interface would call functions this kernel does not have — obvious.
A program built for an *older* one is refused too, while the interface is still ALPHA, because
function numbers may have been reused since: a program calling a number that has changed meaning
does not fail, it quietly does the wrong thing. `KOI_ABI_MINIMUM` is what makes that a decision
rather than a rule; the day the numbering is frozen it stops moving, and from then on function
numbers are never reused — a removed call leaves a hole.

The SDK in [sdk/](sdk/) is the same four files programs here are built from, refreshed by the
build so the two cannot drift apart. **DOSFETCH** lives outside this repository on purpose and is
built with nothing but the SDK: if it stops building, the SDK is broken for everybody rather than
only for programs that happen to sit in this tree.

### Configuration

`\BOOT\userspace.cfg` is read once at boot and applied before the shell paints anything.
`key = value` per line, `#` or `;` starts a comment, unknown keys are ignored and a missing file
is not an error — a configuration file that refused to boot the system it configures would be a
poor trade.

The direction is deliberate: **programs write it, the kernel reads it**. `color.exe` changes the
colours through a system call for the current session and writes the file for the next boot.
Nothing in the kernel knows how a program phrased a setting, and no program reaches into kernel
state to make a change stick.

The colours themselves live in a `CONSOLE_THEME` rather than as constants in the shell, which is
what made them changeable at all — they were scattered through `command.c` before.

`SYS_SETTHEME` returns the resulting theme packed into its return value. That is not a
convenience: a program changing one colour has to write the whole theme back to the file, and
without the read-back `color /b black` would save a default foreground over whatever was there.

A theme change clears the screen. A text console only paints a cell when it writes a character
to it, so otherwise the new background would arrive one character at a time and the old one
would stay everywhere else — which reads as a fault rather than a setting.

### Batch files

A `.BAT` file is run a line at a time. `@` suppresses the echo, `rem` and `:` are comments and
labels — the three pieces of syntax worth having before variables and control flow exist.
`AUTOEXEC.BAT` at the root of the boot drive runs at startup.

Nesting is refused rather than supported. A batch file that called itself would recurse on the
kernel stack with nothing to stop it; DOS needed `CALL` for the same reason.

## Bootloader → kernel ABI

[include/bootinfo.h](include/bootinfo.h) is the only interface between the two. It deliberately
declares its own integer types and does not include `efi.h`: EFI types stay a bootloader detail
and never leak into the kernel. It carries the framebuffer, the final memory map, the ACPI RSDP
pointer, the kernel image range and the kernel stack range.

## Build

```
make          # BOOTX64.EFI + KERNEL.ELF + build/*.EXE
make check    # undefined symbols, relocations, program headers
./qemu.sh     # build a FAT32 image and boot it under QEMU
```

Programs build with the same compiler as the kernel and their own linker script. Adding one means
dropping a `.c` file into `programs/` — the Makefile picks it up.

The bootloader is built with **mingw-gcc**: UEFI uses the Microsoft x64 ABI while the kernel uses
System V. `jump_to_kernel()` is the boundary between them — it passes the argument in `%rdi`,
where the kernel expects it.

Kernel flags that matter:

- `-fno-pie -no-pie` — the image is placed at its link address, so no relocation is needed;
- `-mgeneral-regs-only` — no SSE: after `ExitBootServices` nobody has configured its state, and
  GCC would otherwise emit SSE for struct copies;
- `-mno-red-zone` — the red zone is incompatible with interrupt handlers;
- `-Wl,-z,max-page-size=0x1000` — otherwise segments align to 2 MiB and the image balloons with
  megabytes of zeroes.

`qemu.sh` builds a real volume with `mkfs.vfat -F 32` instead of using `-drive fat:rw:`: QEMU's
virtual FAT is FAT16, and a FAT32 driver of our own could never be tested against it. It runs
with `-m 2G` on purpose — at 256 MiB no allocation ever lands above 4 GiB, which is exactly the
condition under which truncated DMA addresses stay invisible.

## Installation

`setup` lays down two partitions: an EFI System Partition for the loader, and a system partition
for everything else. Two rather than one because every other operating system does it that way,
and because it removes a migration that would otherwise have to happen later.

The loader's partition gets **no drive letter at all**. That is not security — any other
operating system sees an ordinary partition — but it does mean a stray `del` cannot reach the
files the machine needs to start, which is the accident worth preventing. The kernel finds the
system volume instead by looking for `\BOOT\KOIDOS.SYS` on the device it booted from, and only on
that device: a marker on some other disk describes some other installation.

It was verified the only way that means anything — installing to a blank disk, removing the
media, and booting the machine from what was written.

## What comes next

Networking. It is not one project but four: USB plug-and-play first (xHCI interrupt routing,
hot-plug enumeration, hub support), then a class driver, then a TCP/IP stack, then something that
uses it. The Wi-Fi half depends on the vendor and may never be worth it.

Sound has its device and its mixer; what it does not have is music. DOOM's is MUS, which needs a
synthesiser rather than a mixer.

## Historical

[legacy/](legacy/) holds the first shell, the one that ran inside UEFI Boot Services. It is not
built and is not part of the OS; it is kept as a reference for the command semantics that have
to be reproduced on top of the new kernel.
