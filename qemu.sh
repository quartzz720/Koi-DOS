#!/usr/bin/env bash
# Build a real FAT32 ESP image and boot Koi-DOS in QEMU.
#
# The image is deliberately NOT `-drive file=fat:rw:...`. QEMU's virtual FAT
# synthesises a FAT16 volume on the fly, which the kernel's own FAT32 driver
# could never be tested against. mkfs.vfat -F 32 gives us the real thing.
set -euo pipefail

cd "$(dirname "$0")"

IMAGE=esp.img
IMAGE_MB=64
# KOI_SPLIT=1 builds the boot disk the way the installer will lay it out: a GPT
# with the loader on its own EFI System Partition and the system on a second
# one, marked by \BOOT\KOIDOS.SYS. Then Z: is the system partition and the
# loader's has no drive letter at all.
#
# Off by default, because the single-volume layout is what a quick-formatted
# USB stick looks like and that is how this has actually been booted on real
# hardware. Both need testing; neither is "the" configuration.
SPLIT=${KOI_SPLIT:-0}
SPLIT_MB=192
SPLIT_BOOT_MB=64
SPLIT_BOOT_SECTOR=2048
SPLIT_SYSTEM_SECTOR=$(( SPLIT_BOOT_SECTOR + SPLIT_BOOT_MB * 2048 ))
# A second FAT32 volume presented as a USB stick, so the mass storage driver
# has something to enumerate and the drive-letter code has more than one volume
# to hand out. 64 MiB because a smaller image does not have enough clusters to
# be a legal FAT32 volume.
STICK=stick.img
STICK_MB=64
# And a third volume on an emulated NVMe drive, so the newest storage driver
# has a namespace to read and the drive-letter code sees three volumes rather
# than two.
#
# This one carries a real GPT with two partitions - one FAT, one deliberately
# left as raw bytes - because every other image here is a filesystem written
# straight to the whole device. Without a partitioned disk on the bench, the
# GPT path, the EFI-System detection and the "partition we cannot read" case
# are all dead code that looks like it works.
NVME=nvme.img
NVME_MB=128
NVME_ESP_MB=48
# A second disk on the second SATA port and a second NVMe drive, because
# "the first one" was for a long time the only one any of these drivers found.
# Neither carries a filesystem: what is being tested is that they are seen at
# all, and an unformatted disk tests the "partition we cannot read" path too.
# A second USB stick, because the driver used to say "a second storage device
# is present and ignored" - a strange thing for an operating system to say to
# somebody holding two of them.
STICK2=stick2.img
STICK2_MB=64
SECOND=second.img
SECOND_MB=32
NVME2=nvme2.img
NVME2_MB=32
VARS=/tmp/koi-vars.fd

# OVMF lives in a different place on every distribution. Probe the known
# locations rather than hardcoding one; override with OVMF_CODE / OVMF_VARS
# in the environment if yours is somewhere else.
find_ovmf() {
    local name=$1 candidate
    for candidate in \
        "/usr/share/edk2/ovmf/OVMF_$name.fd" \
        "/usr/share/edk2/x64/OVMF_$name.4m.fd" \
        "/usr/share/OVMF/OVMF_$name.fd" \
        "/usr/share/OVMF/OVMF_$name"_4M.fd \
        "/usr/share/ovmf/x64/OVMF_$name.fd" \
        "/usr/share/qemu/edk2-x86_64-$name.fd"
    do
        [ -f "$candidate" ] && { echo "$candidate"; return 0; }
    done
    return 1
}

OVMF_CODE=${OVMF_CODE:-$(find_ovmf CODE || true)}
OVMF_VARS=${OVMF_VARS:-$(find_ovmf VARS || true)}

# Sound. An HD Audio controller with one output codec, which is the shape of
# every machine this could run on - AC'97 is emulated here and exists nowhere
# else, which makes the easy one a trap.
#
# KOI_AUDIO_WAV=out.wav records what the guest played into a file instead of
# sending it to the speakers. That is how the driver was checked: a sine wave
# is either at the frequency and amplitude that were asked for or it is not,
# and a file can be measured where a noise in the room cannot.

# Which codec is attached, because the codecs differ in the one way that
# matters to the driver: hda-output describes a line-out jack that cannot say
# whether anything is plugged into it, and hda-micro describes a built-in
# speaker. Those are the two branches of how an output gets chosen, and a
# laptop is the second one.
HDA_CODEC=${KOI_HDA_CODEC:-hda-output}

# A USB network adapter speaking RNDIS, which is the same protocol a phone
# offers when tethering - so the driver written against a phone is testable
# without one. Behind it is QEMU's user-mode network, and that is the useful
# part: it runs a real DHCP server at 10.0.2.2 handing out 10.0.2.15, answers
# DNS at 10.0.2.3, and passes pings through to the outside world. Every layer
# above the frames has something to talk to here.
#
# KOI_USB_NET=0 leaves it out.
# A USB hub with a keyboard behind it, because a device behind a hub is
# invisible to a driver that does not speak to hubs - which is how a machine
# with four keyboards in its BIOS showed this driver one. Two tiers deep on
# purpose: the route string is four bits per tier and a single tier would
# leave the shifting untested.
#
# KOI_USB_HUB=0 leaves it out.
if [ "${KOI_USB_HUB:-1}" = "0" ]; then
    HUB_ARGS=""
    STICK_PORT=""
else
    HUB_ARGS="-device usb-hub,id=hub1,bus=xhci1.0,port=3 -device usb-hub,id=hub2,bus=xhci1.0,port=3.1 -device usb-kbd,bus=xhci1.0,port=3.1.2"
    # And the stick moves behind the first hub, so a bulk transfer has to find
    # its way through a route string too. Enumerating is not the same as
    # working: the device that answered every descriptor and then delivered
    # nothing is a failure this driver has already had.
    STICK_PORT="port=3.2,"
fi

if [ "${KOI_USB_NET:-1}" = "0" ]; then
    NETWORK_ARGS="-net none"
else
    NETWORK_ARGS="-netdev user,id=koinet -device usb-net,bus=xhci1.0,netdev=koinet,id=usbnet"
fi

# And a real network card, which is what the laptop this is aimed at actually
# has: an Intel 82579LM. QEMU's e1000e is an 82574L - the same family, the same
# registers, and near enough that the driver written here is the driver that
# runs there.
#
# KOI_E1000=0 leaves it out, which is how the USB path above stays testable:
# with a card present the stack prefers it.
if [ "${KOI_E1000:-1}" = "0" ]; then
    ETHERNET_ARGS=""
else
    # tftp= makes slirp serve the package tree itself, on the port it already
    # intercepts. Better than pointing the guest at a server on this machine:
    # the client is then tested against somebody else's implementation of the
    # protocol rather than against its own author's.
    ETHERNET_ARGS="-netdev user,id=koieth,tftp=$HOME/TestOS/DOSGET -device e1000e,netdev=koieth,id=eth0"
fi

if [ -n "${KOI_AUDIO_WAV:-}" ]; then
    AUDIO_BACKEND="wav,id=koisnd,path=$KOI_AUDIO_WAV"
else
    # Whichever of these the host actually has. `none` last, so a machine with
    # no sound server still boots rather than refusing on the command line.
    AUDIO_BACKEND="none,id=koisnd"
    for driver in pipewire pa alsa; do
        if qemu-system-x86_64 -audiodev help 2>/dev/null | grep -qx "$driver"; then
            AUDIO_BACKEND="$driver,id=koisnd"
            break
        fi
    done
fi

if [ -z "$OVMF_CODE" ] || [ -z "$OVMF_VARS" ]; then
    echo "OVMF firmware not found. Install it (Fedora: edk2-ovmf, Debian/Ubuntu: ovmf," >&2
    echo "Arch: edk2-ovmf) or set OVMF_CODE and OVMF_VARS to its location." >&2
    exit 1
fi

make

# mtools writes into the image without needing root or a loop mount.
export MTOOLS_SKIP_CHECK=1

# The image is rebuilt from scratch every run, so a test always starts from a
# known state. Set KEEP_IMAGE=1 to boot the existing one instead - that is how
# you check whether something the guest wrote actually survives a reboot.
if [ "${KEEP_IMAGE:-0}" = "1" ] && [ -f "$IMAGE" ] && [ -f "$STICK" ] \
   && [ -f "$NVME" ] && [ -f "$SECOND" ] && [ -f "$NVME2" ] \
   && [ -f "$STICK2" ]; then
    echo "Reusing existing $IMAGE, $STICK and $NVME"
else
    # FAT32 needs enough clusters to be a legal FAT32 volume; 64 MiB clears it.
    rm -f "$IMAGE" "$STICK" "$NVME" "$SECOND" "$NVME2" "$STICK2"
    dd if=/dev/zero of="$STICK2" bs=1M count="$STICK2_MB" status=none
    mkfs.vfat -F 32 -n KOI-TWO "$STICK2" >/dev/null
    dd if=/dev/zero of="$SECOND" bs=1M count="$SECOND_MB" status=none
    dd if=/dev/zero of="$NVME2" bs=1M count="$NVME2_MB" status=none
    if [ "$SPLIT" = "1" ]; then
        dd if=/dev/zero of="$IMAGE" bs=1M count="$SPLIT_MB" status=none
        sgdisk -n "1:${SPLIT_BOOT_SECTOR}:+${SPLIT_BOOT_MB}M" -t 1:EF00 \
               -c 1:"KOI-BOOT" "$IMAGE" >/dev/null
        sgdisk -n 2:0:0 -t 2:0700 -c 2:"KOI-SYSTEM" "$IMAGE" >/dev/null
        mkfs.vfat -F 32 -n KOI-BOOT --offset "$SPLIT_BOOT_SECTOR" "$IMAGE" \
            $(( SPLIT_BOOT_MB * 1024 )) >/dev/null
        mkfs.vfat -F 32 -n KOI-DOS --offset "$SPLIT_SYSTEM_SECTOR" "$IMAGE" \
            $(( (SPLIT_MB - SPLIT_BOOT_MB - 2) * 1024 )) >/dev/null
    else
        dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_MB" status=none
        mkfs.vfat -F 32 -n KOI-DOS "$IMAGE" >/dev/null
    fi
    dd if=/dev/zero of="$STICK" bs=1M count="$STICK_MB" status=none
    mkfs.vfat -F 32 -n KOI-STICK "$STICK" >/dev/null
    dd if=/dev/zero of="$NVME" bs=1M count="$NVME_MB" status=none
    sgdisk -n "1:2048:+${NVME_ESP_MB}M" -t 1:EF00 -c 1:"KOI-ESP" "$NVME" >/dev/null
    sgdisk -n 2:0:0 -t 2:8300 -c 2:"DATA" "$NVME" >/dev/null
    # The first partition gets a filesystem; the second stays raw on purpose.
    mkfs.vfat -F 32 -n KOI-NVME --offset 2048 "$NVME" \
        $(( NVME_ESP_MB * 1024 )) >/dev/null
    populate=1
fi

# Where the loader goes, and where everything else goes. The same image twice
# when there is only one volume.
if [ "$SPLIT" = "1" ]; then
    BOOTVOL="$IMAGE@@$(( SPLIT_BOOT_SECTOR * 512 ))"
    SYSVOL="$IMAGE@@$(( SPLIT_SYSTEM_SECTOR * 512 ))"
else
    BOOTVOL="$IMAGE"
    SYSVOL="$IMAGE"
fi

if [ "${populate:-0}" = "1" ]; then
mmd -i "$BOOTVOL" ::/EFI ::/EFI/BOOT ::/BOOT
if [ "$SPLIT" = "1" ]; then
    mmd -i "$SYSVOL" ::/BOOT
    # The marker that makes this the system volume rather than the loader's.
    printf 'Koi-DOS system volume.\r\n' | mcopy -i "$SYSVOL" - ::/BOOT/KOIDOS.SYS
fi

# Files for `dir` and `type` to be checked against. The long names are not
# decoration: they are the only thing that exercises the VFAT long-name entries
# in the FAT32 driver, and every 8.3 name would leave that code untested.
mmd -i "$SYSVOL" ::/TEST
printf 'hello from koi-dos\r\n' | mcopy -i "$SYSVOL" - ::/TEST/HELLO.TXT
printf 'a file whose name needs three long-name entries\r\n' \
  | mcopy -i "$SYSVOL" - "::/TEST/a very long file name indeed.txt"
mmd -i "$SYSVOL" "::/Long Directory Name"
printf 'nested\r\n' | mcopy -i "$SYSVOL" - "::/Long Directory Name/inside.txt"
mcopy -i "$SYSVOL" README.md ::/README.MD
mcopy -i "$SYSVOL" ARCHITECTURE.md "::/Architecture Notes.md"

# A startup batch file, so the AUTOEXEC.BAT path is exercised on every boot
# rather than only when someone remembers to write one.
printf '@rem Koi-DOS startup\r\n@echo Welcome to Koi-DOS.\r\nver\r\n' \
  | mcopy -i "$SYSVOL" - ::/AUTOEXEC.BAT

# A WAD, if there is one on this machine, so the DOOM package has data to find.
#
# Copied from ~/TestOS/doom and never from the package tree: the port is
# GPL-2.0 and the game data is not ours to ship. It lands in the root rather
# than in \DOOM, so that a test can move it and prove where the program looks.
# Nothing here fails when the file is absent.
if [ -f "$HOME/TestOS/doom/DOOM.WAD" ]; then
    mcopy -o -i "$SYSVOL" "$HOME/TestOS/doom/DOOM.WAD" ::/DOOM.WAD
fi

# A batch file that runs something which does not stop, so Ctrl+C can be shown
# to end the batch rather than only the command inside it.
printf '@echo before\r\n@spin\r\n@echo AFTER - this line must not run\r\n' \
  | mcopy -i "$SYSVOL" - ::/LOOP.BAT

# Where packages come from, on this bench.
#
# 10.0.2.2 is slirp's own address, and slirp is already serving the package
# tree over TFTP - see the `tftp=` on the ethernet netdev below. Without this
# file dosget falls back to the address of the cable this is developed on,
# which nothing in QEMU can reach, so the whole install path was untestable
# here and only ever tried on real hardware.
printf 'source = 10.0.2.2\r\n' | mcopy -i "$SYSVOL" - ::/BOOT/dosget.cfg

# The stick gets its own files, named so that a `dir` on it cannot be confused
# with a `dir` on the system volume.
mmd -i "$STICK" ::/STICK
printf 'this file lives on the usb stick\r\n' \
  | mcopy -i "$STICK" - ::/STICK/README.TXT
printf 'and so does this one, with a long name\r\n' \
  | mcopy -i "$STICK" - "::/STICK/carried on a usb stick.txt"

mmd -i "$NVME@@1M" ::/NVME
printf 'this file lives on the nvme drive\r\n' \
  | mcopy -i "$NVME@@1M" - ::/NVME/README.TXT
fi

# The bootloader, kernel and programs are refreshed even when the image is
# being reused, so KEEP_IMAGE never means booting a stale build.
mcopy -o -i "$BOOTVOL" boot/efi/boot/BOOTX64.EFI ::/EFI/BOOT/
mcopy -o -i "$BOOTVOL" boot/efi/boot/KERNEL.ELF  ::/BOOT/
# Utilities live in \BIN rather than the root: the root of the system volume is
# where the user's files go, and a pile of .EXE files in it turns `dir` into a
# search. The shell looks there after the current directory and the root.
mmd -i "$SYSVOL" ::/BIN 2>/dev/null || true
for program in build/*.EXE; do
    [ -f "$program" ] || continue
    # Mizu is not one of the system's utilities. It is a package, installed
    # into a directory of its own by dosget, and it goes there below - both
    # because that is where it will actually live on a real machine, and so
    # that testing it here tests the layout it is shipped in.
    case "$(basename "$program")" in
        commander.EXE|cmdrcfg.EXE) continue ;;
    esac
    mcopy -o -i "$SYSVOL" "$program" "::/BIN/$(basename "$program" | tr 'a-z' 'A-Z')"
done

# Koi-Commander, where `dosget install commander` would have put it. On release
# media it is absent entirely: it arrives over the wire, and a system without
# it is the same system.
if [ -f build/commander.EXE ]; then
    mmd -i "$SYSVOL" ::/COMMANDER 2>/dev/null || true
    mcopy -o -i "$SYSVOL" build/commander.EXE ::/COMMANDER/COMMANDER.EXE
    # The first-run questions travel with it, not in \BIN: they are part of
    # the package and mean nothing on a machine that has not got it.
    [ -f build/cmdrcfg.EXE ] && \
        mcopy -o -i "$SYSVOL" build/cmdrcfg.EXE ::/COMMANDER/CMDRCFG.EXE
fi

# Mizu is not built here at all any more: it is a package in a repository of
# its own, and it arrives with `dosget install mizu`. Its absence from this
# image is the check that Koi-DOS is complete without it.

# The licence, for the installer to show and for anyone who looks.
mcopy -o -i "$SYSVOL" LICENSE ::/LICENSE

# Each run starts from a pristine variable store so stale boot entries from a
# previous kernel never shadow the one we just built.
cp -f "$OVMF_VARS" "$VARS"

# -m 2G is not arbitrary: with 256 MiB no allocation ever lands above 4 GiB,
# which is exactly the condition that hid the truncated AHCI DMA addresses.
#
# Two xHCI controllers, with the keyboard on one and the stick on the other,
# because that is what the first real machine turned out to look like: a
# chipset controller and a processor one, and no say in which socket a device
# gets plugged into. A single controller made every event unambiguous by
# accident and hid the bug completely.
#
# The stick carries an id so it can be pulled out and pushed back in from the
# monitor - `device_del usbstick`, then `device_add usb-storage,bus=xhci1.0,
# drive=stick,id=usbstick`. That is the only way to test hot-plug without
# standing next to the machine.
exec qemu-system-x86_64 \
  -m 2G \
  -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -device ich9-ahci,id=ahci \
  -drive id=disk,format=raw,file="$IMAGE",if=none \
  -device ide-hd,drive=disk,bus=ahci.0 \
  -drive id=disk2,format=raw,file="$SECOND",if=none \
  -device ide-hd,drive=disk2,bus=ahci.1 \
  -device usb-ehci,id=ehci \
  -device usb-tablet,bus=ehci.0 \
  -device usb-kbd,bus=ehci.0 \
  -device usb-mouse,bus=ehci.0 \
  -device qemu-xhci,id=xhci0 \
  -device usb-kbd,bus=xhci0.0 \
  -device qemu-xhci,id=xhci1 \
  $HUB_ARGS \
  -drive id=stick,format=raw,file="$STICK",if=none \
  -device usb-storage,bus=xhci1.0,${STICK_PORT:-}drive=stick,id=usbstick \
  -drive id=stick2,format=raw,file="$STICK2",if=none \
  -device usb-storage,bus=xhci1.0,drive=stick2,id=usbstick2 \
  -drive id=ssd,format=raw,file="$NVME",if=none \
  -device nvme,drive=ssd,serial=KOI0001 \
  -drive id=ssd2,format=raw,file="$NVME2",if=none \
  -device nvme,drive=ssd2,serial=KOI0002 \
  -audiodev "$AUDIO_BACKEND" \
  -device ich9-intel-hda,id=hda \
  -device "$HDA_CODEC",bus=hda.0,audiodev=koisnd \
  -serial stdio \
  $NETWORK_ARGS \
  $ETHERNET_ARGS \
  "$@"
