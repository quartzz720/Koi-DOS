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
   && [ -f "$NVME" ]; then
    echo "Reusing existing $IMAGE, $STICK and $NVME"
else
    # FAT32 needs enough clusters to be a legal FAT32 volume; 64 MiB clears it.
    rm -f "$IMAGE" "$STICK" "$NVME"
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
    mcopy -o -i "$SYSVOL" "$program" "::/BIN/$(basename "$program" | tr 'a-z' 'A-Z')"
done

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
exec qemu-system-x86_64 \
  -m 2G \
  -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -device ich9-ahci,id=ahci \
  -drive id=disk,format=raw,file="$IMAGE",if=none \
  -device ide-hd,drive=disk,bus=ahci.0 \
  -device qemu-xhci,id=xhci0 \
  -device usb-kbd,bus=xhci0.0 \
  -device qemu-xhci,id=xhci1 \
  -drive id=stick,format=raw,file="$STICK",if=none \
  -device usb-storage,bus=xhci1.0,drive=stick \
  -drive id=ssd,format=raw,file="$NVME",if=none \
  -device nvme,drive=ssd,serial=KOI0001 \
  -audiodev "$AUDIO_BACKEND" \
  -device ich9-intel-hda,id=hda \
  -device hda-output,bus=hda.0,audiodev=koisnd \
  -serial stdio \
  -net none \
  "$@"
