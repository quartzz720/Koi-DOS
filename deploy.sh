#!/usr/bin/env bash
# Put the current build onto a USB stick.
#
# The stick wanders between /dev/sdc and /dev/sdf depending on what else is
# plugged in, and typing the wrong letter once is how a disk gets ruined. So
# this finds it rather than being told, refuses anything that is not removable,
# refuses the disk the running system is on, and shows what it found before it
# writes a byte.
#
# It only ever COPIES FILES onto a filesystem that is already there. It does
# not format, does not partition, does not touch a boot sector. If the stick
# needs preparing, do that once by hand - see README.
#
#   ./deploy.sh              find the stick, ask, copy
#   ./deploy.sh -y           do not ask (the checks still run)
#   ./deploy.sh /dev/sdd     use this device, still checked and still asked
#   ./deploy.sh -l           list candidates and stop
set -uo pipefail

cd "$(dirname "$0")"

ASSUME_YES=0
LIST_ONLY=0
WANTED=""

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) ASSUME_YES=1 ;;
        -l|--list) LIST_ONLY=1 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        /dev/*) WANTED=$1 ;;
        *) echo "deploy.sh: do not understand '$1'" >&2; exit 1 ;;
    esac
    shift
done

say() { printf '%s\n' "$*"; }
die() { printf '\n%s\n' "$*" >&2; exit 1; }

# ---- What must never be written to --------------------------------------
#
# The disks carrying /, /boot and /boot/efi. Asking the kernel which device
# backs a mount point is the only answer that cannot be out of date; a list of
# names written down here would be wrong the first time the machine changed.
protected_disks() {
    local point source
    for point in / /boot /boot/efi /home; do
        mountpoint -q "$point" 2>/dev/null || continue
        source=$(findmnt -no SOURCE --target "$point" 2>/dev/null) || continue
        lsblk -no PKNAME "$source" 2>/dev/null
        # A partition's parent is the disk; a whole-device mount has no parent
        # and is its own disk.
        lsblk -no KNAME "$source" 2>/dev/null
    done | sort -u
}

is_protected() {
    local name=$1
    local guard
    while read -r guard; do
        [ -n "$guard" ] || continue
        [ "$name" = "$guard" ] && return 0
    done <<< "$PROTECTED"
    return 1
}

# ---- Finding the stick ---------------------------------------------------
#
# Removable, or attached over USB. Both, because a card reader reports
# removable without being USB and some enclosures report USB without setting
# the removable flag.
candidates() {
    lsblk -dno NAME,RM,TRAN,SIZE,MODEL 2>/dev/null | while read -r name rm tran size model; do
        [ "$rm" = "1" ] || [ "$tran" = "usb" ] || continue
        is_protected "$name" && continue
        # A stick with no size is a card reader with no card in it.
        [ "$size" = "0B" ] && continue
        printf '%s\t%s\t%s\n' "$name" "$size" "${model:-unknown}"
    done
}

PROTECTED=$(protected_disks)

say "Protected (the system is on these): $(echo "$PROTECTED" | tr '\n' ' ')"
say ""

FOUND=$(candidates)

if [ -z "$FOUND" ]; then
    die "No removable disk found.

Plug the stick in and try again. If it is plugged in, check that it appears:

    lsblk -o NAME,RM,TRAN,SIZE,MODEL"
fi

say "Removable disks:"
printf '%s\n' "$FOUND" | while IFS=$'\t' read -r name size model; do
    say "    /dev/$name   $size   $model"
done
say ""

[ "$LIST_ONLY" = "1" ] && exit 0

# ---- Choosing one --------------------------------------------------------

if [ -n "$WANTED" ]; then
    DEVICE=${WANTED#/dev/}
    # Given a partition, take its disk.
    PARENT=$(lsblk -no PKNAME "/dev/$DEVICE" 2>/dev/null | head -1)
    [ -n "$PARENT" ] && DEVICE=$PARENT
    printf '%s\n' "$FOUND" | grep -q "^$DEVICE	" \
        || die "/dev/$DEVICE is not one of the removable disks above. Refusing."
elif [ "$(printf '%s\n' "$FOUND" | wc -l)" = "1" ]; then
    DEVICE=$(printf '%s\n' "$FOUND" | cut -f1)
else
    die "More than one removable disk. Name the one you mean:

    ./deploy.sh /dev/sdX"
fi

is_protected "$DEVICE" && die "/dev/$DEVICE carries the running system. Refusing."

# ---- Finding the partition to write to -----------------------------------
#
# The first FAT partition on it. Koi-DOS media is one FAT volume, or two with
# the loader on the first; either way the loader lives on the first one, and
# that is where everything goes.
PARTITION=""
while read -r part fstype; do
    case "$fstype" in
        vfat|fat|fat32|fat16) PARTITION=$part; break ;;
    esac
done < <(lsblk -lno NAME,FSTYPE "/dev/$DEVICE" 2>/dev/null | tail -n +2)

if [ -z "$PARTITION" ]; then
    # Some sticks are formatted with no partition table at all.
    if [ "$(lsblk -dno FSTYPE "/dev/$DEVICE" 2>/dev/null)" = "vfat" ]; then
        PARTITION=$DEVICE
    else
        die "/dev/$DEVICE has no FAT filesystem on it.

This script only copies files; it will not create one. Prepare the stick once:

    sudo mkfs.vfat -F 32 -n KOI-DOS /dev/${DEVICE}1"
    fi
fi

# ---- What is going to be written -----------------------------------------

MISSING=""
[ -f boot/efi/boot/BOOTX64.EFI ] || MISSING="$MISSING boot/efi/boot/BOOTX64.EFI"
[ -f boot/efi/boot/KERNEL.ELF ] || MISSING="$MISSING boot/efi/boot/KERNEL.ELF"
[ -f LICENSE ] || MISSING="$MISSING LICENSE"
[ -n "$MISSING" ] && die "Not built yet - missing:$MISSING

    make"

PROGRAMS=(build/*.EXE)
[ -e "${PROGRAMS[0]}" ] || die "No programs built. Run: make"

# Programs from the sibling projects, when they are there and current. Not an
# error when they are not - they are separate repositories and may not exist.
EXTRA=()
for extra in ../Games/GAMES.EXE ../DOSFETCH/DOSFETCH.EXE; do
    [ -f "$extra" ] && EXTRA+=("$extra")
done

MODEL=$(printf '%s\n' "$FOUND" | grep "^$DEVICE	" | cut -f3)
SIZE=$(printf '%s\n' "$FOUND" | grep "^$DEVICE	" | cut -f2)
LABEL=$(lsblk -no LABEL "/dev/$PARTITION" 2>/dev/null | head -1)

say "Target"
say "    disk       /dev/$DEVICE   $SIZE   $MODEL"
say "    partition  /dev/$PARTITION   ${LABEL:-no label}"
say ""
say "Will copy"
say "    EFI/BOOT/BOOTX64.EFI"
say "    BOOT/KERNEL.ELF"
say "    LICENSE"
say "    BIN/  ${#PROGRAMS[@]} programs$([ ${#EXTRA[@]} -gt 0 ] && printf ' + %d from sibling projects' "${#EXTRA[@]}")"
say ""
say "Nothing is formatted, partitioned or erased. Existing files of the same"
say "name are replaced; everything else on the stick is left alone."
say ""

if [ "$ASSUME_YES" != "1" ]; then
    printf 'Write to /dev/%s? [y/N] ' "$PARTITION"
    read -r answer
    case "$answer" in
        y|Y|yes|YES) ;;
        *) die "Stopped. Nothing was written." ;;
    esac
fi

# ---- Mounting ------------------------------------------------------------
#
# The desktop may have mounted it already, in which case use that rather than
# fighting it - trying to mount a busy device is where "Device or resource
# busy" comes from.
MOUNTED_HERE=0
MOUNT=$(findmnt -nro TARGET "/dev/$PARTITION" 2>/dev/null | head -1)

if [ -n "$MOUNT" ]; then
    say "Already mounted at $MOUNT"
else
    MOUNT=$(mktemp -d /tmp/koi-deploy.XXXXXX)
    say "Mounting /dev/$PARTITION at $MOUNT"
    sudo mount -o "uid=$(id -u),gid=$(id -g)" "/dev/$PARTITION" "$MOUNT" \
        || die "Could not mount /dev/$PARTITION. Nothing was written."
    MOUNTED_HERE=1
fi

cleanup() {
    if [ "$MOUNTED_HERE" = "1" ]; then
        sync
        sudo umount "$MOUNT" 2>/dev/null && rmdir "$MOUNT" 2>/dev/null
    else
        sync
    fi
}
trap cleanup EXIT

# ---- Copying -------------------------------------------------------------

copy() {
    local source=$1 destination=$2
    mkdir -p "$(dirname "$destination")" || return 1
    cp -f "$source" "$destination" || return 1
    printf '    %-28s %s\n' "$(basename "$destination")" "$(du -h "$source" | cut -f1)"
}

say ""
say "Copying"

copy boot/efi/boot/BOOTX64.EFI "$MOUNT/EFI/BOOT/BOOTX64.EFI" || die "Copy failed."
copy boot/efi/boot/KERNEL.ELF  "$MOUNT/BOOT/KERNEL.ELF"      || die "Copy failed."
copy LICENSE                   "$MOUNT/LICENSE"              || die "Copy failed."

for program in "${PROGRAMS[@]}" "${EXTRA[@]:-}"; do
    [ -f "$program" ] || continue
    name=$(basename "$program" | tr 'a-z' 'A-Z')
    copy "$program" "$MOUNT/BIN/$name" || die "Copy failed."
done

sync

say ""
say "Done. The stick can be pulled once this prompt returns."
say ""
say "Built from:"
say "    kernel   $(git describe --always --dirty 2>/dev/null || echo 'not a git checkout')"
say "    on       $(date '+%Y-%m-%d %H:%M')"
