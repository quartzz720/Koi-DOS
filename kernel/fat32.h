#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include "partition.h"

/* Long names are supported in full, both read and displayed.
 *
 * The 8.3 limit was a consequence of 1980 hardware, not a design goal, and
 * Koi-DOS is what DOS would look like if it turned up on a UEFI machine today.
 * Short names still exist underneath - every entry has one - but they are an
 * implementation detail rather than what the user sees. */
#define FAT_NAME_MAX 256

#define FAT_ATTRIBUTE_READ_ONLY 0x01
#define FAT_ATTRIBUTE_HIDDEN 0x02
#define FAT_ATTRIBUTE_SYSTEM 0x04
#define FAT_ATTRIBUTE_VOLUME_LABEL 0x08
#define FAT_ATTRIBUTE_DIRECTORY 0x10
#define FAT_ATTRIBUTE_ARCHIVE 0x20
#define FAT_ATTRIBUTE_LONG_NAME 0x0F

typedef struct {
    char name[FAT_NAME_MAX];   /* long name when present, else the 8.3 name */
    boot_uint8_t attributes;
    boot_uint32_t size;
    boot_uint32_t first_cluster;
    boot_uint16_t modified_date;
    boot_uint16_t modified_time;

    /* Where the short entry itself lives, so that size, cluster and timestamp
       can be updated in place without searching for it again. Sector numbers
       are relative to the start of the volume. */
    boot_uint64_t entry_sector;
    boot_uint32_t entry_offset;
    /* First sector/offset of the long-name run preceding it, so deleting a
       file can mark those entries free too. Equal to entry_sector/offset when
       there is no long name. */
    boot_uint64_t first_entry_sector;
    boot_uint32_t first_entry_offset;
} FAT_ENTRY;

/* Iteration state for reading a directory one entry at a time. */
typedef struct {
    VOLUME* volume;
    boot_uint32_t cluster;      /* cluster currently being read */
    boot_uint32_t entry_index;  /* index of the next entry within it */
    char long_name[FAT_NAME_MAX];
    int long_name_valid;
    boot_uint8_t long_name_checksum;
    /* Where the current long-name run began, carried into FAT_ENTRY so a
       delete can free those entries as well as the short one. */
    boot_uint64_t first_sector;
    boot_uint32_t first_offset;
} FAT_DIRECTORY;

/* Read the BPB and take the volume label. Returns 1 when the volume holds a
   FAT32 filesystem this driver understands. */
int fat32_mount(VOLUME* volume);

/* Total and free bytes, for `dir` and `mem`. Counting free space walks the
   whole FAT, so the result is cached after the first call. */
boot_uint64_t fat32_total_bytes(VOLUME* volume);
boot_uint64_t fat32_free_bytes(VOLUME* volume);

/* Resolve a path to its directory entry. `path` is absolute within the volume
   and uses backslashes: "\", "\SYSTEM", "\SYSTEM\CONFIG.SYS". */
int fat32_stat(VOLUME* volume, const char* path, FAT_ENTRY* entry);

/* Directory iteration. fat32_opendir() accepts the same paths as
   fat32_stat(); fat32_readdir() returns 0 when the directory is exhausted. */
int fat32_opendir(VOLUME* volume, const char* path, FAT_DIRECTORY* directory);
int fat32_readdir(FAT_DIRECTORY* directory, FAT_ENTRY* entry);

/* Read up to `length` bytes from `offset` within a file. Returns the number
   of bytes actually read. */
boot_uint32_t fat32_read(VOLUME* volume, const FAT_ENTRY* entry,
                         boot_uint32_t offset, void* buffer,
                         boot_uint32_t length);

/* ---- Writing -------------------------------------------------------------
 *
 * Every one of these keeps all copies of the allocation table in step. FAT
 * carries two by default, and a writer that updates only the first leaves a
 * volume that chkdsk - or the next operating system to mount it - will call
 * corrupt. The FSInfo free-cluster hint is refreshed too, since a stale one
 * makes other systems report the wrong free space.
 */

/* Create an empty file, or a directory when `directory` is non-zero. Fails if
   the name already exists. On success `entry` describes the new object. */
int fat32_create(VOLUME* volume, const char* path, int directory,
                 FAT_ENTRY* entry);

/* Write `length` bytes at `offset`, extending the file as needed. Returns the
   number of bytes written and updates the on-disk size and timestamp. */
boot_uint32_t fat32_write(VOLUME* volume, FAT_ENTRY* entry,
                          boot_uint32_t offset, const void* buffer,
                          boot_uint32_t length);

/* Remove a file, or an empty directory. Refuses a directory with anything
   left in it. */
int fat32_remove(VOLUME* volume, const char* path);

/* Rename within the same directory, or move between directories. */
int fat32_rename(VOLUME* volume, const char* from, const char* to);

/* Replace the attribute byte of an existing entry. */
int fat32_set_attributes(VOLUME* volume, const FAT_ENTRY* entry,
                         boot_uint8_t attributes);

#endif
