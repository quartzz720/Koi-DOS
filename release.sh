#!/usr/bin/env bash
# Build the installation medium: one image file, written to a USB stick with
# Rufus or balenaEtcher, booted, and installed from.
#
# The layout is a GPT with a single EFI System Partition holding everything -
# the loader, the kernel, the utilities and the packages. Firmware boots it
# because the partition is marked as an ESP, and `setup` installs from it onto
# a disk. One partition rather than two: the split layout belongs to an
# installed machine, where the loader's partition is deliberately kept without
# a drive letter, and a stick has no such need.
#
# What goes on it is everything except DOOM, and the reason is not size - all
# of it together is under four megabytes. It is that the network here is an
# Intel card or a phone over USB, and the machine that most needs the packages
# is the one whose wireless we do not drive. Anything left off the medium is
# unreachable on that machine, not merely later. DOOM is the exception because
# it cannot start without a WAD, and the WAD is not ours to ship.
set -euo pipefail

cd "$(dirname "$0")"

VERSION=${1:-0.9}
IMAGE=koi-dos-$VERSION.img
IMAGE_MB=96
PACKAGES=${PACKAGES:-$HOME/TestOS/DOSGET/packages}
ESP_SECTOR=2048

# Which packages travel. TOOLS is unpacked into \BIN rather than a directory of
# its own, exactly as dosget would install it.
CARRY="COMMANDER MIZU GAMES DOSFETCH"

for tool in mkfs.vfat sgdisk mcopy mmd; do
    command -v "$tool" >/dev/null || { echo "release.sh: $tool is missing" >&2; exit 1; }
done

echo "Building everything"
make >/dev/null
( cd "$HOME/TestOS/DOSGET" && ./publish.sh >/dev/null )

[ -d "$PACKAGES" ] || { echo "release.sh: no packages at $PACKAGES" >&2; exit 1; }

echo "Making $IMAGE"
rm -f "$IMAGE"
dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_MB" status=none
sgdisk -n "1:${ESP_SECTOR}:0" -t 1:EF00 -c 1:"KOI-DOS" "$IMAGE" >/dev/null

# The filesystem is told exactly how many blocks the partition has, taken from
# the partition table rather than worked out from the image size. Guessing left
# mkfs warning that it had found more room than it was given, which is a
# filesystem that does not describe its own partition - harmless today and the
# kind of thing that stops being harmless once something resizes.
ESP_SECTORS=$(sgdisk -i 1 "$IMAGE" | sed -n 's/^Partition size: \([0-9]*\) sectors.*/\1/p')
mkfs.vfat -F 32 -n KOI-DOS --offset "$ESP_SECTOR" "$IMAGE" \
    $(( ESP_SECTORS / 2 )) >/dev/null

VOL="$IMAGE@@$(( ESP_SECTOR * 512 ))"
export MTOOLS_SKIP_CHECK=1

mmd -i "$VOL" ::/EFI ::/EFI/BOOT ::/BOOT ::/BIN ::/BOOT/CONFIG ::/BOOT/DOSGET

echo "  the loader and the kernel"
mcopy -o -i "$VOL" boot/efi/boot/BOOTX64.EFI ::/EFI/BOOT/
mcopy -o -i "$VOL" boot/efi/boot/KERNEL.ELF  ::/BOOT/
mcopy -o -i "$VOL" LICENSE ::/LICENSE

echo "  the utilities"
for tool in "$PACKAGES"/TOOLS/*.EXE; do
    [ -f "$tool" ] || continue
    mcopy -o -i "$VOL" "$tool" "::/BIN/$(basename "$tool")"
done

# Each package into a directory of its own, with the record that says what it
# put there. The record is what `setup` reads to know which directories to
# carry onto the installed disk, and what `dosget remove` reads later - so it
# is written here rather than left for a first boot to invent.
PATHS=""
for name in $CARRY; do
    [ -d "$PACKAGES/$name" ] || { echo "  ($name is not built - skipped)"; continue; }
    echo "  $name"
    mmd -i "$VOL" "::/$name"
    RECORD=$(mktemp)
    printf 'directory = \\%s\r\n' "$name" > "$RECORD"
    for file in "$PACKAGES/$name"/*; do
        base=$(basename "$file")
        [ "$base" = MANIFEST ] && continue
        mcopy -o -i "$VOL" "$file" "::/$name/$base"
        printf 'file = %s\r\n' "$base" >> "$RECORD"
    done
    mcopy -o -i "$VOL" "$RECORD" "::/BOOT/DOSGET/$name.PKG"
    rm -f "$RECORD"
    PATHS="$PATHS;\\$name"

    version=$(sed -n 's/^version *= *//p' "$PACKAGES/$name/MANIFEST" | head -1)
    printf '%s %s\r\n' "$name" "${version:-0}" >> /tmp/koi-dosget-db.$$
done

# The search path, so a package can be run by name the moment the machine
# starts - on the medium and, once setup has carried this file across, on the
# installed disk too.
printf 'path = %s\r\n' "${PATHS#;}" > /tmp/koi-system-cfg.$$
mcopy -o -i "$VOL" /tmp/koi-system-cfg.$$ ::/BOOT/CONFIG/SYSTEM.CFG
rm -f /tmp/koi-system-cfg.$$

[ -f /tmp/koi-dosget-db.$$ ] && {
    mcopy -o -i "$VOL" /tmp/koi-dosget-db.$$ ::/BOOT/DOSGET.DB
    rm -f /tmp/koi-dosget-db.$$
}

# Where packages come from, so `dosget` works on a machine that has never been
# configured. It is read fresh on every invocation, not at boot, so anybody who
# runs their own server changes one line and needs no reboot.
printf 'source = %s\r\n' "${KOI_SERVER_ADDRESS:-195.133.195.44}" > /tmp/koi-dosget-cfg.$$
mcopy -o -i "$VOL" /tmp/koi-dosget-cfg.$$ ::/BOOT/dosget.cfg
rm -f /tmp/koi-dosget-cfg.$$

# What the medium says when it starts. It is an installer first: somebody who
# wrote this to a stick to try the system should be told how, and somebody who
# wants to look around first should not be forced through it.
cat <<'AUTOEXEC' > /tmp/koi-autoexec.$$
@echo off
@echo.
@echo Koi-DOS is running from the installation medium.
@echo.
@echo Type SETUP to install it onto a disk in this machine.
@echo Type HELP to see what else there is, or just look around.
@echo.
@echo NET START then DOSGET LIST fetches more programs, if there is a wire.
@echo.
AUTOEXEC
mcopy -o -i "$VOL" /tmp/koi-autoexec.$$ ::/AUTOEXEC.BAT
rm -f /tmp/koi-autoexec.$$

echo
echo "$IMAGE  $(du -h "$IMAGE" | cut -f1)"
echo
echo "Write it to a USB stick with Rufus or balenaEtcher, in DD/image mode,"
echo "then boot the machine from that stick and type SETUP."
