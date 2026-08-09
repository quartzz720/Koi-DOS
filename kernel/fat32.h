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

/* Forget every mounted filesystem.
 *
 * Required before the volume table is rebuilt, and not merely tidy. The table
 * is a static array, so a rescan refills the same addresses with different
 * partitions. A mount record left behind still points at one of those
 * addresses and still holds the previous filesystem's geometry - so the next
 * write computes where to put a directory entry using one filesystem's layout
 * and sends it to another filesystem's device. That is not a stale read. It is
 * one volume's metadata written into the middle of another volume. */
void fat32_unmount_all(void);

/* Called while the allocation tables are being cleared, which on a large volume
   is tens of seconds of work. Without it the machine looks hung, and a person
   watching a hung installer reaches for the power switch. */
typedef void (*FAT_FORMAT_PROGRESS)(boot_uint64_t done, boot_uint64_t total);

/* Write a fresh, empty FAT32 filesystem over a region of a block device.
 *
 * Everything already on it is gone. This is the most destructive thing the
 * system can do, so the decision to call it belongs to the caller and the
 * caller alone - nothing here asks for confirmation, and nothing here knows
 * whether the region is the one the user meant.
 *
 * `label` may be empty. `serial` is the volume serial that identifies a volume
 * afterwards; the boot volume is matched by it, so two volumes sharing one
 * would be a genuine problem. Returns 0 when the region cannot hold a legal
 * FAT32 filesystem, or when a write fails.
 *
 * The cluster size is chosen from the volume's size, not from what would waste
 * the least - a large volume given small clusters gets an allocation table
 * bigger than everything that will ever be stored on it. */
int fat32_format(BLOCK_DEVICE* device, boot_uint64_t first_sector,
                 boot_uint64_t sector_count, const char* label,
                 boot_uint32_t serial, FAT_FORMAT_PROGRESS progress);

/* Total and free bytes, for `dir` and `mem`.
 *
 * The free figure is seeded from the FSInfo sector at mount and kept current
 * as clusters are claimed and released. Counting it properly means reading the
 * whole allocation table - tens of thousands of sectors on a large volume - so
 * that only happens when FSInfo is missing or says something impossible. */
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

/* ---- Checking a volume ---------------------------------------------------
 *
 * A filesystem driver that can only ever have been right needs no checker.
 * This one has already been wrong: a directory that grew past its first
 * cluster kept an end-of-directory marker in the middle of its chain, and
 * every file beyond that point was written perfectly and could not be found
 * again by anybody. The writer no longer does that, but volumes it wrote
 * before the fix are still out there, and nothing in the system could either
 * see the damage or undo it.
 *
 * So this exists to answer two questions that a driver alone cannot: is what
 * is on the disk self-consistent, and if not, can it be put back. It reads
 * the whole allocation table and walks every directory, which is why it is a
 * command a person runs and not something a mount does.
 */
typedef enum {
    FAT_FAULT_TERMINATOR,        /* end-of-directory marker hiding live entries */
    FAT_FAULT_ORPHAN_LONG_NAME,  /* long-name entries with no short entry */
    FAT_FAULT_BAD_LINK,          /* a chain pointing outside the volume */
    FAT_FAULT_CROSS_LINKED,      /* one cluster claimed twice, or a loop */
    FAT_FAULT_SIZE_TOO_LARGE,    /* recorded size exceeds the clusters held */
    FAT_FAULT_SIZE_TOO_SMALL,    /* clusters held exceed the recorded size */
    FAT_FAULT_PARENT_WRONG,      /* ".." not pointing at the parent */
    FAT_FAULT_LOST_CLUSTERS,     /* allocated, belonging to nothing */
    FAT_FAULT_FAT_COPIES_DIFFER, /* the copies of the table disagree */
    FAT_FAULT_FREE_COUNT_WRONG,  /* the FSInfo hint is not what the table says */
    FAT_FAULT_TOO_COMPLEX        /* the check itself ran out of room */
} FAT_FAULT;

/* Called once per fault as it is found, so a long check says something while
   it runs rather than only at the end. `where` is a path when the fault has
   one and the volume otherwise; `number` carries the cluster, the count or
   the size the fault is about; `repaired` says whether it was put right. */
typedef void (*FAT_CHECK_REPORT)(FAT_FAULT fault, const char* where,
                                 boot_uint64_t number, int repaired);

typedef struct {
    boot_uint32_t directories;
    boot_uint32_t files;
    boot_uint64_t bytes_in_files;
    boot_uint32_t clusters_total;
    boot_uint32_t clusters_used;
    boot_uint32_t clusters_free;
    boot_uint32_t clusters_lost;
    /* Reported because it is the number that decides how much has to be in a
       directory before it grows a second cluster - which is the only condition
       under which a whole class of damage can happen at all. On a volume with
       large clusters a directory may never grow in its life. */
    boot_uint32_t cluster_bytes;
    boot_uint32_t entries_per_cluster;
    boot_uint32_t faults;
    boot_uint32_t repaired;
    /* Zero when a pass had to be given up - out of memory, or a read that
       failed. A check that could not finish must not be reported as a clean
       volume, which is the whole reason this field exists. */
    int complete;
} FAT_CHECK_RESULT;

/* Read the volume and report what is wrong with it. With `repair` set, put
   right what can be put right; with it clear, change nothing at all. Returns
   0 when the volume could not be checked (not mounted, or out of memory) -
   which is not the same as a volume with faults. */
int fat32_check(VOLUME* volume, int repair, FAT_CHECK_REPORT report,
                FAT_CHECK_RESULT* result);

#endif
