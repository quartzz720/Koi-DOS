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
if [ "${KEEP_IMAGE:-0}" = "1" ] && [ -f "$IMAGE" ]; then
    echo "Reusing existing $IMAGE"
else
    # FAT32 needs enough clusters to be a legal FAT32 volume; 64 MiB clears it.
    rm -f "$IMAGE"
    dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_MB" status=none
    mkfs.vfat -F 32 -n KOI-DOS "$IMAGE" >/dev/null
    populate=1
fi

if [ "${populate:-0}" = "1" ]; then
mmd -i "$IMAGE" ::/EFI ::/EFI/BOOT ::/BOOT

# Files for `dir` and `type` to be checked against. The long names are not
# decoration: they are the only thing that exercises the VFAT long-name entries
# in the FAT32 driver, and every 8.3 name would leave that code untested.
mmd -i "$IMAGE" ::/TEST
printf 'hello from koi-dos\r\n' | mcopy -i "$IMAGE" - ::/TEST/HELLO.TXT
printf 'a file whose name needs three long-name entries\r\n' \
  | mcopy -i "$IMAGE" - "::/TEST/a very long file name indeed.txt"
mmd -i "$IMAGE" "::/Long Directory Name"
printf 'nested\r\n' | mcopy -i "$IMAGE" - "::/Long Directory Name/inside.txt"
mcopy -i "$IMAGE" README.md ::/README.MD
mcopy -i "$IMAGE" ARCHITECTURE.md "::/Architecture Notes.md"

# A startup batch file, so the AUTOEXEC.BAT path is exercised on every boot
# rather than only when someone remembers to write one.
printf '@rem Koi-DOS startup\r\n@echo Welcome to Koi-DOS.\r\nver\r\n' \
  | mcopy -i "$IMAGE" - ::/AUTOEXEC.BAT
fi

# The bootloader, kernel and programs are refreshed even when the image is
# being reused, so KEEP_IMAGE never means booting a stale build.
mcopy -o -i "$IMAGE" boot/efi/boot/BOOTX64.EFI ::/EFI/BOOT/
mcopy -o -i "$IMAGE" boot/efi/boot/KERNEL.ELF  ::/BOOT/
for program in build/*.EXE; do
    [ -f "$program" ] || continue
    mcopy -o -i "$IMAGE" "$program" "::/$(basename "$program" | tr 'a-z' 'A-Z')"
done

# Each run starts from a pristine variable store so stale boot entries from a
# previous kernel never shadow the one we just built.
cp -f "$OVMF_VARS" "$VARS"

# -m 2G is not arbitrary: with 256 MiB no allocation ever lands above 4 GiB,
# which is exactly the condition that hid the truncated AHCI DMA addresses.
exec qemu-system-x86_64 \
  -m 2G \
  -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -device ich9-ahci,id=ahci \
  -drive id=disk,format=raw,file="$IMAGE",if=none \
  -device ide-hd,drive=disk,bus=ahci.0 \
  -serial stdio \
  -net none \
  "$@"
