#ifndef KERNEL_PARTITION_H
#define KERNEL_PARTITION_H

#include "block.h"

#define VOLUME_MAX 8
#define VOLUME_LABEL_LENGTH 12

/* A filesystem-bearing region of a block device, and the drive letter it
   answers to.
 *
 * Koi-DOS numbers drives downward from Z:. In MS-DOS, A: and B: were reserved
 * for floppies and the system landed on C: only because it was the first
 * letter left over; with no floppies to reserve for, the system volume takes
 * Z: and further volumes take Y:, X: and so on. Changing that scheme means
 * changing assign_letters() and nothing else. */
typedef struct {
    BLOCK_DEVICE* device;
    boot_uint64_t first_sector;
    boot_uint64_t sector_count;
    char letter;                       /* 'Z', 'Y', ... */
    int is_boot_volume;
    char label[VOLUME_LABEL_LENGTH];    /* from the filesystem, once mounted */
} VOLUME;

/* Every partition found, whether or not it carries a filesystem we understand.
 *
 * The volume table above is the shell's view: things with drive letters. This
 * is the disk's view, and the two are deliberately different. A tool that
 * formats or repartitions has to address a region that has no filesystem yet -
 * by definition it has no letter - and it has to be able to say what is
 * currently there before destroying it. Naming what you are about to erase is
 * the whole safety mechanism. */
#define PARTITION_MAX 32

#define PARTITION_SCHEME_NONE 0     /* the filesystem sits on the whole device */
#define PARTITION_SCHEME_GPT 1
#define PARTITION_SCHEME_MBR 2

typedef struct {
    BLOCK_DEVICE* device;
    boot_uint32_t device_index;
    boot_uint32_t number;              /* 1-based, the way every tool names them */
    boot_uint64_t first_sector;
    boot_uint64_t sector_count;
    boot_uint8_t scheme;
    boot_uint8_t type;                 /* the MBR type byte; 0 for GPT */
    int is_fat;                        /* has a plausible FAT boot sector */
    int is_efi_system;                 /* GPT type GUID says EFI System */
    char letter;                       /* drive letter, or 0 when unmounted */
} PARTITION;

boot_uint32_t partition_count(void);
PARTITION* partition_at(boot_uint32_t index);

/* Examine every registered block device and build the volume table.
   `boot_serial` is the FAT volume serial the bootloader recorded, and
   `serial_known` says whether it managed to read one. Returns the number of
   volumes found. */
boot_uint32_t partition_scan(boot_uint32_t boot_serial, int serial_known);

/* Lay a fresh GPT over a whole device, destroying whatever was there.
 *
 * One request per partition, in order from the start of the disk. A request
 * with `sector_count` zero takes everything left. Partitions are aligned to
 * one mebibyte, which is what every tool has done since alignment started
 * mattering for flash.
 *
 * This writes a partition table and nothing else: the partitions it creates
 * have no filesystem until something formats them.
 *
 * Returns 0 without writing anything if the requests do not fit. */
#define GPT_REQUEST_MAX 4

typedef struct {
    boot_uint64_t sector_count;        /* 0 means "the rest of the disk" */
    int is_efi_system;
    const char* name;                  /* shown by other systems' tools */
} GPT_REQUEST;

int partition_write_gpt(BLOCK_DEVICE* device, const GPT_REQUEST* requests,
                        boot_uint32_t count);

/* Look at the disks again, after something changed what is on them.
   Every VOLUME pointer handed out before this call is stale afterwards. */
boot_uint32_t partition_rescan(void);

boot_uint32_t volume_count(void);
VOLUME* volume_at(boot_uint32_t index);

/* Look up by drive letter, case insensitive. NULL when there is no such
   drive. */
VOLUME* volume_by_letter(char letter);

/* The file that marks a volume as the system one.
 *
 * On a two-partition install the firmware loads from the EFI System Partition,
 * so that is the volume the boot-serial match finds - and it is exactly the
 * volume we want out of reach, since a stray `del` in it takes the machine's
 * ability to start. This file, on the volume beside it, says "the system lives
 * here"; that volume takes Z: and the loader's partition gets no letter at all.
 *
 * A file rather than the GPT partition name, because a volume written to a
 * whole device - a quick-formatted USB stick - has no partition table to carry
 * a name. Nothing marked means nothing changes: the boot volume stays Z:, and
 * a single-partition install keeps working exactly as before. */
#define SYSTEM_VOLUME_MARKER "\\BOOT\\KOIDOS.SYS"

/* Hand Z: to `system`, and take the letter away from the volume the firmware
   loaded from. Everything else shifts down from Y:. */
void partition_set_system_volume(VOLUME* system);

/* The volume the system booted from - the one Z: refers to. NULL when it could
   not be identified, which is not the same as "there are no volumes": the
   caller must not fall back to picking one, because picking wrong means
   pointing the shell at somebody else's system disk. */
VOLUME* volume_boot(void);

#endif
