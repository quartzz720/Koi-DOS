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

/* Examine every registered block device and build the volume table.
   `boot_serial` is the FAT volume serial the bootloader recorded, and
   `serial_known` says whether it managed to read one. Returns the number of
   volumes found. */
boot_uint32_t partition_scan(boot_uint32_t boot_serial, int serial_known);

boot_uint32_t volume_count(void);
VOLUME* volume_at(boot_uint32_t index);

/* Look up by drive letter, case insensitive. NULL when there is no such
   drive. */
VOLUME* volume_by_letter(char letter);

/* The volume the system booted from - the one Z: refers to. NULL when it could
   not be identified, which is not the same as "there are no volumes": the
   caller must not fall back to picking one, because picking wrong means
   pointing the shell at somebody else's system disk. */
VOLUME* volume_boot(void);

#endif
