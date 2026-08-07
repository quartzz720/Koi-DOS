#include "fat32.h"
#include "memory.h"
#include "string.h"
#include "rtc.h"

/* FAT32.
 *
 * The read side came first and was proven against a known image before any of
 * the write side was written, because FAT keeps two copies of its allocation
 * table plus an FSInfo sector and a half-correct writer corrupts a volume
 * rather than merely failing. */

#define SECTOR_SIZE 512
#define DIRECTORY_ENTRY_SIZE 32
#define ENTRIES_PER_SECTOR (SECTOR_SIZE / DIRECTORY_ENTRY_SIZE)

#define CLUSTER_FREE 0x00000000U
#define CLUSTER_END_MINIMUM 0x0FFFFFF8U
#define CLUSTER_MASK 0x0FFFFFFFU

#define ENTRY_FREE 0xE5
#define ENTRY_END 0x00

/* The FSInfo sector: a hint, kept next to the boot sector, saying how many
   clusters are free and where to start looking. Advisory by specification -
   the allocation table is the only authority - but reading it at mount is the
   difference between answering `dir` immediately and walking a table that is
   tens of megabytes long on a large volume. */
#define FSINFO_SECTOR 1
#define FSINFO_LEAD_SIGNATURE 0x41615252U
#define FSINFO_STRUCT_SIGNATURE 0x61417272U
#define FSINFO_TRAIL_SIGNATURE 0xAA550000U
#define FSINFO_FREE_COUNT_OFFSET 488
#define FSINFO_NEXT_FREE_OFFSET 492
#define FSINFO_UNKNOWN 0xFFFFFFFFU

#define LONG_NAME_LAST 0x40
#define LONG_NAME_SEQUENCE_MASK 0x1F
#define LONG_NAME_CHARS 13

/* Per-volume mount state. Volumes are few, so a parallel array keyed by index
   avoids growing VOLUME with filesystem-specific fields. */
typedef struct {
    VOLUME* volume;
    boot_uint32_t bytes_per_sector;
    boot_uint32_t sectors_per_cluster;
    boot_uint32_t reserved_sectors;
    boot_uint32_t fat_count;
    boot_uint32_t sectors_per_fat;
    boot_uint32_t root_cluster;
    boot_uint32_t total_sectors;
    boot_uint32_t first_data_sector;
    boot_uint32_t cluster_count;
    boot_uint64_t free_bytes;
    int free_bytes_known;
    /* Where the next search for a free cluster starts. Without it every
       allocation walks the table from the beginning, which on a large volume
       means re-reading megabytes to find a cluster the previous call had
       already walked past. Zero means "no hint yet". */
    boot_uint32_t search_hint;
    /* One sector of the allocation table, held in memory and written back when
       something else needs the buffer. 128 clusters share a sector, so a file
       of any size touches each sector of the table a great many times. */
    boot_uint8_t* fat_cache;
    boot_uint32_t fat_cache_sector;   /* index within one copy of the table */
    int fat_cache_valid;
    int fat_cache_dirty;
    int mounted;
} FAT_VOLUME;

static FAT_VOLUME mounts[VOLUME_MAX];

static FAT_VOLUME* mount_for(VOLUME* volume) {
    for (boot_uint32_t index = 0; index < VOLUME_MAX; index++)
        if (mounts[index].mounted && mounts[index].volume == volume)
            return &mounts[index];
    return (FAT_VOLUME*)0;
}

static boot_uint16_t read16(const boot_uint8_t* data) {
    return (boot_uint16_t)(data[0] | (data[1] << 8));
}

static boot_uint32_t read32(const boot_uint8_t* data) {
    return (boot_uint32_t)data[0] | ((boot_uint32_t)data[1] << 8) |
           ((boot_uint32_t)data[2] << 16) | ((boot_uint32_t)data[3] << 24);
}

/* Sectors are addressed relative to the volume; the device knows nothing about
   partitions. The multi-sector form is the one that matters for bulk transfer:
   every driver underneath splits a long request itself, and a request per
   sector costs a full command round trip for 512 bytes. */
static int read_volume_sectors(FAT_VOLUME* fat, boot_uint64_t sector,
                               boot_uint32_t count, void* buffer) {
    return block_read(fat->volume->device, fat->volume->first_sector + sector,
                      count, buffer);
}

static int read_volume_sector(FAT_VOLUME* fat, boot_uint64_t sector,
                              void* buffer) {
    return read_volume_sectors(fat, sector, 1, buffer);
}

static int write_volume_sectors(FAT_VOLUME* fat, boot_uint64_t sector,
                                boot_uint32_t count, const void* buffer) {
    return block_write(fat->volume->device, fat->volume->first_sector + sector,
                       count, buffer);
}

static int write_volume_sector(FAT_VOLUME* fat, boot_uint64_t sector,
                               const void* buffer) {
    return write_volume_sectors(fat, sector, 1, buffer);
}

/* ---- The allocation table, one sector at a time -------------------------
 *
 * Every cluster of every file has an entry here, and 128 entries share a
 * 512-byte sector, so a chain walks over the same sector again and again.
 * Reading it from the disk each time - and writing it back to both copies of
 * the table each time it changed - is what made growing a file cost four disk
 * commands per cluster. Holding one sector and writing it back only when
 * something else needs the buffer collapses that to four per 128 clusters.
 *
 * Write-back, so the disk is behind between calls. Every public operation that
 * can dirty it flushes before it returns, which is the same guarantee the
 * directory entry already had. */
static int fat_flush(FAT_VOLUME* fat) {
    int ok = 1;

    if (!fat->fat_cache || !fat->fat_cache_valid || !fat->fat_cache_dirty) return 1;
    for (boot_uint32_t copy = 0; copy < fat->fat_count; copy++) {
        boot_uint64_t target = fat->reserved_sectors +
                               (boot_uint64_t)copy * fat->sectors_per_fat +
                               fat->fat_cache_sector;
        if (!write_volume_sector(fat, target, fat->fat_cache)) { ok = 0; break; }
    }
    fat->fat_cache_dirty = 0;
    /* A half-written entry means the copies may now disagree; refuse to keep
       serving a buffer whose relationship to the disk is unknown. */
    if (!ok) fat->fat_cache_valid = 0;
    return ok;
}

/* The sector of the table holding a given entry, ready to be read or changed.
   NULL when it could not be brought in. */
static boot_uint8_t* fat_sector(FAT_VOLUME* fat, boot_uint32_t relative) {
    if (relative >= fat->sectors_per_fat) return (boot_uint8_t*)0;
    if (!fat->fat_cache) {
        fat->fat_cache = (boot_uint8_t*)alloc_page();
        if (!fat->fat_cache) return (boot_uint8_t*)0;
        fat->fat_cache_valid = 0;
        fat->fat_cache_dirty = 0;
    }
    if (fat->fat_cache_valid && fat->fat_cache_sector == relative)
        return fat->fat_cache;
    if (!fat_flush(fat)) return (boot_uint8_t*)0;
    if (!read_volume_sector(fat, fat->reserved_sectors + relative,
                            fat->fat_cache)) {
        fat->fat_cache_valid = 0;
        return (boot_uint8_t*)0;
    }
    fat->fat_cache_sector = relative;
    fat->fat_cache_valid = 1;
    return fat->fat_cache;
}

static boot_uint64_t cluster_first_sector(FAT_VOLUME* fat, boot_uint32_t cluster) {
    /* Cluster numbering starts at 2: entries 0 and 1 of the FAT are reserved
       for the media descriptor and flags, and never describe real data. */
    return fat->first_data_sector +
           (boot_uint64_t)(cluster - 2) * fat->sectors_per_cluster;
}

static int cluster_is_end(boot_uint32_t cluster) {
    return (cluster & CLUSTER_MASK) >= CLUSTER_END_MINIMUM;
}

/* Follow one link in the allocation chain. */
static boot_uint32_t next_cluster(FAT_VOLUME* fat, boot_uint32_t cluster) {
    boot_uint32_t offset = cluster * 4;
    boot_uint8_t* sector;

    if (cluster < 2 || cluster >= fat->cluster_count + 2) return CLUSTER_MASK;
    sector = fat_sector(fat, offset / fat->bytes_per_sector);
    if (!sector) return CLUSTER_MASK;
    return read32(sector + offset % fat->bytes_per_sector) & CLUSTER_MASK;
}

void fat32_unmount_all(void) {
    /* Anything still held for a volume goes out to the disk before the record
       of that volume disappears. */
    for (boot_uint32_t index = 0; index < VOLUME_MAX; index++) {
        if (!mounts[index].mounted) continue;
        (void)fat_flush(&mounts[index]);
        if (mounts[index].fat_cache) free_page(mounts[index].fat_cache);
    }
    memset(mounts, 0, sizeof(mounts));
}

int fat32_mount(VOLUME* volume) {
    boot_uint8_t* sector;
    FAT_VOLUME* fat = (FAT_VOLUME*)0;
    boot_uint32_t root_entries;
    boot_uint32_t sectors_per_fat_16;

    if (!volume) return 0;
    for (boot_uint32_t index = 0; index < VOLUME_MAX; index++)
        if (!mounts[index].mounted) { fat = &mounts[index]; break; }
    if (!fat) return 0;

    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return 0;
    if (!block_read(volume->device, volume->first_sector, 1, sector)) {
        free_page(sector);
        return 0;
    }

    memset(fat, 0, sizeof(*fat));
    fat->volume = volume;
    fat->bytes_per_sector = read16(sector + 11);
    fat->sectors_per_cluster = sector[13];
    fat->reserved_sectors = read16(sector + 14);
    fat->fat_count = sector[16];
    root_entries = read16(sector + 17);
    sectors_per_fat_16 = read16(sector + 22);
    fat->total_sectors = read16(sector + 19);
    if (!fat->total_sectors) fat->total_sectors = read32(sector + 32);
    fat->sectors_per_fat = read32(sector + 36);
    fat->root_cluster = read32(sector + 44);

    /* FAT32 is identified by what it does NOT have: no fixed-size root
       directory and a 32-bit FAT size field. FAT12 and FAT16 both put a
       non-zero value in the 16-bit field and a real count in root_entries. */
    if (root_entries != 0 || sectors_per_fat_16 != 0 || !fat->sectors_per_fat) {
        free_page(sector);
        return 0;
    }
    if (fat->bytes_per_sector != SECTOR_SIZE || !fat->sectors_per_cluster ||
        !fat->reserved_sectors || !fat->fat_count || fat->root_cluster < 2) {
        free_page(sector);
        return 0;
    }

    fat->first_data_sector = fat->reserved_sectors +
                             fat->fat_count * fat->sectors_per_fat;
    if (fat->total_sectors <= fat->first_data_sector) {
        free_page(sector);
        return 0;
    }
    fat->cluster_count = (fat->total_sectors - fat->first_data_sector) /
                         fat->sectors_per_cluster;

    /* The label in the BPB is 11 space-padded characters at offset 71. The
       authoritative one lives in a root-directory entry, but this is enough
       for `vol` and costs nothing. */
    {
        int length = 0;
        for (int index = 0; index < 11; index++) {
            char character = (char)sector[71 + index];
            if (character == ' ') continue;
            if (length + 1 >= VOLUME_LABEL_LENGTH) break;
            volume->label[length++] = character;
        }
        volume->label[length] = 0;
    }

    fat->mounted = 1;

    /* Take the free-space figure and the search position from FSInfo when it
       is there and plausible. Everything that writes keeps both up to date
       from here on, so this is the only chance to avoid the full scan - and on
       a large volume that scan is the slowest thing the driver can do. A value
       that fails the sanity check simply leaves them unknown, and the scan
       happens the first time something asks. */
    if (read_volume_sector(fat, FSINFO_SECTOR, sector) &&
        read32(sector) == FSINFO_LEAD_SIGNATURE &&
        read32(sector + 484) == FSINFO_STRUCT_SIGNATURE) {
        boot_uint32_t free_clusters = read32(sector + FSINFO_FREE_COUNT_OFFSET);
        boot_uint32_t next_free = read32(sector + FSINFO_NEXT_FREE_OFFSET);

        if (free_clusters != FSINFO_UNKNOWN && free_clusters <= fat->cluster_count) {
            fat->free_bytes = (boot_uint64_t)free_clusters *
                              fat->sectors_per_cluster * fat->bytes_per_sector;
            fat->free_bytes_known = 1;
        }
        if (next_free >= 2 && next_free < fat->cluster_count + 2)
            fat->search_hint = next_free;
    }

    free_page(sector);
    return 1;
}

boot_uint64_t fat32_total_bytes(VOLUME* volume) {
    FAT_VOLUME* fat = mount_for(volume);
    if (!fat) return 0;
    return (boot_uint64_t)fat->cluster_count * fat->sectors_per_cluster *
           fat->bytes_per_sector;
}

/* How much of the table to read per command when it has to be counted. */
#define FREE_SCAN_PAGES 16

boot_uint64_t fat32_free_bytes(VOLUME* volume) {
    FAT_VOLUME* fat = mount_for(volume);
    boot_uint8_t* buffer;
    boot_uint64_t free_clusters = 0;
    boot_uint32_t total;
    boot_uint32_t pages = FREE_SCAN_PAGES;
    boot_uint32_t per_pass;

    if (!fat) return 0;
    total = fat->cluster_count + 2;
    /* The answer is computed once and then kept up to date by write_fat_entry
       rather than recomputed, because counting means reading the whole table.
       Getting that wrong is not a cosmetic bug: this used to be invalidated on
       every cluster allocation, and update_fsinfo asks for it after every
       write, so copying one file rescanned the table once per cluster. Mount
       usually seeds it from FSInfo and the count below never runs at all. */
    if (fat->free_bytes_known) return fat->free_bytes;

    /* Everything held for this volume goes out first: the count has to see the
       disk, and the disk is only complete once the cache is flushed. */
    if (!fat_flush(fat)) return 0;

    for (;;) {
        buffer = (boot_uint8_t*)alloc_pages(pages);
        if (buffer || pages == 1) break;
        pages /= 2;
    }
    if (!buffer) return 0;
    per_pass = pages * (boot_uint32_t)PAGE_SIZE / fat->bytes_per_sector;

    for (boot_uint32_t index = 0; index < fat->sectors_per_fat; index += per_pass) {
        boot_uint32_t run = fat->sectors_per_fat - index;
        boot_uint32_t entries;

        if (run > per_pass) run = per_pass;
        if (!read_volume_sectors(fat, fat->reserved_sectors + index, run, buffer))
            break;

        entries = run * fat->bytes_per_sector / 4;
        for (boot_uint32_t entry = 0; entry < entries; entry++) {
            boot_uint32_t cluster = index * (fat->bytes_per_sector / 4) + entry;
            if (cluster < 2) continue;
            if (cluster >= total) break;
            if ((read32(buffer + entry * 4) & CLUSTER_MASK) == CLUSTER_FREE)
                free_clusters++;
        }
    }
    free_pages(buffer, pages);

    fat->free_bytes = free_clusters * fat->sectors_per_cluster *
                      fat->bytes_per_sector;
    fat->free_bytes_known = 1;
    return fat->free_bytes;
}

/* Expand a space-padded 8.3 name into "NAME.EXT". */
static void format_short_name(const boot_uint8_t* raw, char* output) {
    int length = 0;

    for (int index = 0; index < 8 && raw[index] != ' '; index++)
        output[length++] = (char)raw[index];
    if (raw[8] != ' ') {
        output[length++] = '.';
        for (int index = 8; index < 11 && raw[index] != ' '; index++)
            output[length++] = (char)raw[index];
    }
    output[length] = 0;
}

/* The checksum that ties a run of long-name entries to the short entry that
   follows them. A mismatch means the long name belongs to a file that was
   deleted and partially overwritten, and must be discarded. */
static boot_uint8_t short_name_checksum(const boot_uint8_t* raw) {
    boot_uint8_t sum = 0;
    for (int index = 0; index < 11; index++)
        sum = (boot_uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + raw[index]);
    return sum;
}

/* Copy the 13 UTF-16 characters a long-name entry carries into `output` at
   the position its sequence number dictates. Characters outside Latin-1 are
   replaced rather than mangled - a full Unicode console is a later problem. */
static void gather_long_name(const boot_uint8_t* raw, char* output) {
    static const int offsets[LONG_NAME_CHARS] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    boot_uint8_t sequence = (boot_uint8_t)(raw[0] & LONG_NAME_SEQUENCE_MASK);
    int base;

    if (!sequence || sequence > FAT_NAME_MAX / LONG_NAME_CHARS) return;
    base = (sequence - 1) * LONG_NAME_CHARS;

    for (int index = 0; index < LONG_NAME_CHARS; index++) {
        boot_uint16_t character = read16(raw + offsets[index]);
        int position = base + index;
        if (position >= FAT_NAME_MAX - 1) break;
        if (character == 0x0000 || character == 0xFFFF) {
            output[position] = 0;
            return;
        }
        output[position] = character < 0x100 ? (char)character : '?';
    }
}

int fat32_opendir(VOLUME* volume, const char* path, FAT_DIRECTORY* directory) {
    FAT_VOLUME* fat = mount_for(volume);
    FAT_ENTRY entry;

    if (!fat || !directory) return 0;
    memset(directory, 0, sizeof(*directory));
    directory->volume = volume;

    if (!path || !path[0] || (path[0] == '\\' && !path[1])) {
        directory->cluster = fat->root_cluster;
        return 1;
    }
    if (!fat32_stat(volume, path, &entry)) return 0;
    if (!(entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) return 0;
    /* ".." in the root's children stores cluster 0, meaning "the root". */
    directory->cluster = entry.first_cluster ? entry.first_cluster
                                             : fat->root_cluster;
    return 1;
}

int fat32_readdir(FAT_DIRECTORY* directory, FAT_ENTRY* entry) {
    FAT_VOLUME* fat;
    boot_uint8_t* sector;
    boot_uint32_t entries_per_cluster;

    if (!directory || !entry) return 0;
    fat = mount_for(directory->volume);
    if (!fat || cluster_is_end(directory->cluster) || directory->cluster < 2)
        return 0;

    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return 0;
    entries_per_cluster = fat->sectors_per_cluster * ENTRIES_PER_SECTOR;

    for (;;) {
        boot_uint32_t index = directory->entry_index;
        boot_uint32_t sector_index;
        boot_uint64_t current_sector;
        boot_uint32_t current_offset;
        const boot_uint8_t* raw;

        if (index >= entries_per_cluster) {
            boot_uint32_t following = next_cluster(fat, directory->cluster);
            if (cluster_is_end(following) || following < 2) break;
            directory->cluster = following;
            directory->entry_index = 0;
            continue;
        }

        sector_index = index / ENTRIES_PER_SECTOR;
        current_sector = cluster_first_sector(fat, directory->cluster) + sector_index;
        if (!read_volume_sector(fat, current_sector, sector)) break;
        current_offset = (index % ENTRIES_PER_SECTOR) * DIRECTORY_ENTRY_SIZE;
        raw = sector + current_offset;
        directory->entry_index++;

        if (raw[0] == ENTRY_END) break;      /* nothing beyond this point */
        if (raw[0] == ENTRY_FREE) {
            directory->long_name_valid = 0;
            continue;
        }

        if ((raw[11] & FAT_ATTRIBUTE_LONG_NAME) == FAT_ATTRIBUTE_LONG_NAME) {
            /* Long-name entries are stored in reverse, last fragment first,
               with the final one flagged. */
            if (raw[0] & LONG_NAME_LAST) {
                memset(directory->long_name, 0, sizeof(directory->long_name));
                directory->long_name_checksum = raw[13];
                directory->long_name_valid = 1;
                directory->first_sector = current_sector;
                directory->first_offset = current_offset;
            }
            if (directory->long_name_valid && raw[13] != directory->long_name_checksum)
                directory->long_name_valid = 0;
            if (directory->long_name_valid)
                gather_long_name(raw, directory->long_name);
            continue;
        }

        /* A real entry. The volume label is not a file and is skipped. */
        if (raw[11] & FAT_ATTRIBUTE_VOLUME_LABEL) {
            directory->long_name_valid = 0;
            continue;
        }

        memset(entry, 0, sizeof(*entry));
        entry->entry_sector = current_sector;
        entry->entry_offset = current_offset;
        entry->first_entry_sector = current_sector;
        entry->first_entry_offset = current_offset;
        if (directory->long_name_valid &&
            directory->long_name_checksum == short_name_checksum(raw) &&
            directory->long_name[0]) {
            boot_uint64_t length = strlen(directory->long_name);
            memcpy(entry->name, directory->long_name, length + 1);
            entry->first_entry_sector = directory->first_sector;
            entry->first_entry_offset = directory->first_offset;
        } else {
            format_short_name(raw, entry->name);
        }
        directory->long_name_valid = 0;

        entry->attributes = raw[11];
        entry->modified_time = read16(raw + 22);
        entry->modified_date = read16(raw + 24);
        entry->size = read32(raw + 28);
        entry->first_cluster = ((boot_uint32_t)read16(raw + 20) << 16) |
                               read16(raw + 26);
        free_page(sector);
        return 1;
    }

    free_page(sector);
    return 0;
}

/* Case-insensitive comparison, because DOS never cared and neither do we. */
static int names_match(const char* left, const char* right) {
    while (*left && *right) {
        char a = *left >= 'a' && *left <= 'z' ? (char)(*left - 32) : *left;
        char b = *right >= 'a' && *right <= 'z' ? (char)(*right - 32) : *right;
        if (a != b) return 0;
        left++;
        right++;
    }
    return !*left && !*right;
}

int fat32_stat(VOLUME* volume, const char* path, FAT_ENTRY* entry) {
    FAT_VOLUME* fat = mount_for(volume);
    FAT_DIRECTORY directory;
    boot_uint32_t cluster;
    const char* cursor = path;

    if (!fat || !path || !entry) return 0;

    cluster = fat->root_cluster;
    memset(entry, 0, sizeof(*entry));
    entry->attributes = FAT_ATTRIBUTE_DIRECTORY;
    entry->first_cluster = cluster;
    entry->name[0] = '\\';

    while (*cursor == '\\') cursor++;
    if (!*cursor) return 1;   /* the root itself */

    for (;;) {
        char component[FAT_NAME_MAX];
        boot_uint64_t length = 0;
        int found = 0;

        while (*cursor && *cursor != '\\' && length + 1 < sizeof(component))
            component[length++] = *cursor++;
        component[length] = 0;
        while (*cursor == '\\') cursor++;
        if (!length) return 0;

        memset(&directory, 0, sizeof(directory));
        directory.volume = volume;
        directory.cluster = cluster;

        while (fat32_readdir(&directory, entry)) {
            if (!names_match(entry->name, component)) continue;
            found = 1;
            break;
        }
        if (!found) return 0;

        if (!*cursor) return 1;   /* last component, and it exists */
        if (!(entry->attributes & FAT_ATTRIBUTE_DIRECTORY)) return 0;
        cluster = entry->first_cluster ? entry->first_cluster : fat->root_cluster;
    }
}

boot_uint32_t fat32_read(VOLUME* volume, const FAT_ENTRY* entry,
                         boot_uint32_t offset, void* buffer,
                         boot_uint32_t length) {
    FAT_VOLUME* fat = mount_for(volume);
    boot_uint8_t* output = (boot_uint8_t*)buffer;
    boot_uint8_t* sector;
    boot_uint32_t cluster;
    boot_uint32_t cluster_bytes;
    boot_uint32_t copied = 0;

    if (!fat || !entry || !buffer || !length) return 0;
    if (offset >= entry->size) return 0;
    if (length > entry->size - offset) length = entry->size - offset;

    cluster = entry->first_cluster;
    cluster_bytes = fat->sectors_per_cluster * fat->bytes_per_sector;

    /* Skip whole clusters until the one holding `offset`. */
    while (offset >= cluster_bytes) {
        cluster = next_cluster(fat, cluster);
        if (cluster_is_end(cluster) || cluster < 2) return 0;
        offset -= cluster_bytes;
    }

    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return 0;

    /* `offset` is now the position within the current cluster, and stays so:
       each pass copies at most to the end of the cluster, and lands exactly on
       the boundary. Whole sectors are read straight into the caller's buffer,
       as many at a time as the cluster holds; only a partial sector at either
       end goes through the scratch page. */
    while (copied < length) {
        boot_uint32_t sector_index;
        boot_uint32_t sector_offset;
        boot_uint32_t chunk;
        boot_uint64_t first;

        if (offset >= cluster_bytes) {
            cluster = next_cluster(fat, cluster);
            if (cluster_is_end(cluster) || cluster < 2) break;
            offset -= cluster_bytes;
            continue;
        }

        sector_index = offset / fat->bytes_per_sector;
        sector_offset = offset % fat->bytes_per_sector;
        first = cluster_first_sector(fat, cluster) + sector_index;

        if (!sector_offset && length - copied >= fat->bytes_per_sector &&
            !((boot_uint64_t)(output + copied) & 3)) {
            boot_uint32_t run = (length - copied) / fat->bytes_per_sector;
            boot_uint32_t left = fat->sectors_per_cluster - sector_index;

            if (run > left) run = left;
            if (!read_volume_sectors(fat, first, run, output + copied)) break;
            chunk = run * fat->bytes_per_sector;
            copied += chunk;
            offset += chunk;
            continue;
        }

        if (!read_volume_sector(fat, first, sector)) break;

        chunk = fat->bytes_per_sector - sector_offset;
        if (chunk > length - copied) chunk = length - copied;
        memcpy(output + copied, sector + sector_offset, chunk);
        copied += chunk;
        offset += chunk;
    }

    free_page(sector);
    return copied;
}

/* ---- Writing -------------------------------------------------------------
 *
 * The invariant that matters: every copy of the allocation table is written,
 * always. FAT32 keeps two by default, and a volume whose second copy disagrees
 * with the first is one that chkdsk - or the next system to mount it - calls
 * corrupt. write_fat_entry() is therefore the only way anything here touches
 * the table.
 */

static void write16(boot_uint8_t* data, boot_uint16_t value) {
    data[0] = (boot_uint8_t)value;
    data[1] = (boot_uint8_t)(value >> 8);
}

static void write32(boot_uint8_t* data, boot_uint32_t value) {
    data[0] = (boot_uint8_t)value;
    data[1] = (boot_uint8_t)(value >> 8);
    data[2] = (boot_uint8_t)(value >> 16);
    data[3] = (boot_uint8_t)(value >> 24);
}

/* Set one FAT entry. The held sector belongs to every copy of the table at
   once - fat_flush writes it to all of them - so the two copies cannot drift
   apart by any route through here. */
static int write_fat_entry(FAT_VOLUME* fat, boot_uint32_t cluster,
                           boot_uint32_t value) {
    boot_uint32_t byte_offset = cluster * 4;
    boot_uint32_t within = byte_offset % fat->bytes_per_sector;
    boot_uint32_t previous_value;
    boot_uint8_t* sector;
    int was_free;
    int now_free;

    if (cluster < 2 || cluster >= fat->cluster_count + 2) return 0;
    sector = fat_sector(fat, byte_offset / fat->bytes_per_sector);
    if (!sector) {
        fat->free_bytes_known = 0;
        return 0;
    }

    previous_value = read32(sector + within) & CLUSTER_MASK;
    /* The top four bits of a FAT32 entry are reserved and must be kept as they
       were found, not overwritten with zeroes. */
    write32(sector + within,
            (read32(sector + within) & 0xF0000000U) | (value & CLUSTER_MASK));
    fat->fat_cache_dirty = 1;

    was_free = previous_value == CLUSTER_FREE;
    now_free = (value & CLUSTER_MASK) == CLUSTER_FREE;

    /* Space given back should be handed out again, so the next search starts
       no later than the cluster that was just released. */
    if (now_free && !was_free && cluster < fat->search_hint)
        fat->search_hint = cluster;

    /* Adjust the running free-space figure rather than throwing it away. One
       entry changed and we know which way, so a full rescan would be answering
       a question we already hold the answer to. */
    if (fat->free_bytes_known) {
        boot_uint64_t cluster_bytes = (boot_uint64_t)fat->sectors_per_cluster *
                                      fat->bytes_per_sector;
        if (was_free && !now_free) {
            fat->free_bytes = fat->free_bytes >= cluster_bytes
                            ? fat->free_bytes - cluster_bytes : 0;
        } else if (!was_free && now_free) {
            fat->free_bytes += cluster_bytes;
        }
    }
    return 1;
}

/* Finish a change to the volume: the held sector of the allocation table goes
 * out to the disk, and the FSInfo hint is brought back into line with it.
 *
 * Every public operation that can touch the table ends here, which is what
 * makes the write-back cache safe: between commands the disk holds everything,
 * and only within one command is it allowed to be behind. */
static void commit(FAT_VOLUME* fat);

/* Refresh the FSInfo hint. It is advisory - the FAT is authoritative - but a
   stale value makes other systems report nonsense free space. */
static void update_fsinfo(FAT_VOLUME* fat) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();
    if (!sector) return;
    if (read_volume_sector(fat, FSINFO_SECTOR, sector) &&
        read32(sector) == FSINFO_LEAD_SIGNATURE &&
        read32(sector + 484) == FSINFO_STRUCT_SIGNATURE) {
        boot_uint64_t free_bytes = fat32_free_bytes(fat->volume);
        boot_uint32_t clusters = (boot_uint32_t)(free_bytes /
            (fat->sectors_per_cluster * fat->bytes_per_sector));
        /* Only when something actually moved. A write that stays inside the
           clusters a file already owns changes neither, and that is most of
           them. */
        if (read32(sector + FSINFO_FREE_COUNT_OFFSET) != clusters ||
            read32(sector + FSINFO_NEXT_FREE_OFFSET) != fat->search_hint) {
            write32(sector + FSINFO_FREE_COUNT_OFFSET, clusters);
            write32(sector + FSINFO_NEXT_FREE_OFFSET, fat->search_hint);
            (void)write_volume_sector(fat, FSINFO_SECTOR, sector);
        }
    }
    free_page(sector);
}

static void commit(FAT_VOLUME* fat) {
    (void)fat_flush(fat);
    update_fsinfo(fat);
}

/* The most memory this will take to clear one cluster. Clusters run to 32 KB
   on a large volume and 64 KB is legal, so a cap keeps a rare large cluster
   from asking the allocator for an awkward run of pages. */
#define BLANK_MAX_PAGES 8

/* A freshly allocated cluster still holds whatever was there before. For a
   directory that would be read as a screenful of garbage entries, so directory
   clusters are cleared; a file's are not. Nothing can read past a file's
   recorded size, and clearing every cluster before writing over it would mean
   writing the whole volume twice. */
static void blank_cluster(FAT_VOLUME* fat, boot_uint32_t cluster) {
    boot_uint64_t first = cluster_first_sector(fat, cluster);
    boot_uint32_t cluster_bytes = fat->sectors_per_cluster * fat->bytes_per_sector;
    boot_uint32_t pages = (cluster_bytes + (boot_uint32_t)PAGE_SIZE - 1) /
                          (boot_uint32_t)PAGE_SIZE;
    boot_uint32_t per_pass;
    boot_uint8_t* blank;

    if (pages > BLANK_MAX_PAGES) pages = BLANK_MAX_PAGES;
    for (;;) {
        blank = (boot_uint8_t*)alloc_pages(pages);
        if (blank || pages == 1) break;
        pages /= 2;
    }
    if (!blank) return;

    memset(blank, 0, pages * (boot_uint32_t)PAGE_SIZE);
    per_pass = pages * (boot_uint32_t)PAGE_SIZE / fat->bytes_per_sector;
    for (boot_uint32_t done = 0; done < fat->sectors_per_cluster; done += per_pass) {
        boot_uint32_t chunk = fat->sectors_per_cluster - done;
        if (chunk > per_pass) chunk = per_pass;
        (void)write_volume_sectors(fat, first + done, chunk, blank);
    }
    free_pages(blank, pages);
}

/* Find a free cluster, claim it, and link it after `previous` when given.
 *
 * The search resumes where the last one stopped. Starting from the beginning
 * every time is correct but quadratic: filling a volume means walking an
 * ever-longer prefix of used entries to reach the first free one, and each
 * step of that walk is a disk read. */
static boot_uint32_t allocate_cluster(FAT_VOLUME* fat, boot_uint32_t previous,
                                      int blank) {
    boot_uint32_t entries_per_sector = fat->bytes_per_sector / 4;
    boot_uint32_t total = fat->cluster_count + 2;
    boot_uint32_t start;
    boot_uint32_t first_sector;
    boot_uint32_t found = 0;

    start = fat->search_hint >= 2 && fat->search_hint < total ? fat->search_hint : 2;
    first_sector = start / entries_per_sector;

    /* One lap of the table from the hint, wrapping back to the front, so a
       volume with free space anywhere still finds it. */
    for (boot_uint32_t step = 0; step < fat->sectors_per_fat && !found; step++) {
        boot_uint32_t index = first_sector + step;
        boot_uint8_t* sector;

        if (index >= fat->sectors_per_fat) index -= fat->sectors_per_fat;
        sector = fat_sector(fat, index);
        if (!sector) break;
        for (boot_uint32_t slot = 0; slot < entries_per_sector; slot++) {
            boot_uint32_t cluster = index * entries_per_sector + slot;
            if (cluster < 2) continue;
            if (cluster >= total) break;
            if ((read32(sector + slot * 4) & CLUSTER_MASK) == CLUSTER_FREE) {
                found = cluster;
                break;
            }
        }
    }
    if (!found) return 0;
    fat->search_hint = found + 1;

    if (!write_fat_entry(fat, found, CLUSTER_MASK)) return 0;   /* end of chain */
    if (previous && !write_fat_entry(fat, previous, found)) return 0;

    if (blank) blank_cluster(fat, found);
    return found;
}

static void free_chain(FAT_VOLUME* fat, boot_uint32_t cluster) {
    while (cluster >= 2 && !cluster_is_end(cluster) &&
           cluster < fat->cluster_count + 2) {
        boot_uint32_t following = next_cluster(fat, cluster);
        if (!write_fat_entry(fat, cluster, CLUSTER_FREE)) break;
        cluster = following;
    }
}

/* Read-modify-write one 32-byte directory entry in place. */
static int patch_entry(FAT_VOLUME* fat, boot_uint64_t sector_number,
                       boot_uint32_t offset, const boot_uint8_t* replacement) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();
    int ok;

    if (!sector) return 0;
    if (!read_volume_sector(fat, sector_number, sector)) {
        free_page(sector);
        return 0;
    }
    memcpy(sector + offset, replacement, DIRECTORY_ENTRY_SIZE);
    ok = write_volume_sector(fat, sector_number, sector);
    free_page(sector);
    return ok;
}

static int mark_entry_free(FAT_VOLUME* fat, boot_uint64_t sector_number,
                           boot_uint32_t offset) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();
    int ok;

    if (!sector) return 0;
    if (!read_volume_sector(fat, sector_number, sector)) {
        free_page(sector);
        return 0;
    }
    sector[offset] = ENTRY_FREE;
    ok = write_volume_sector(fat, sector_number, sector);
    free_page(sector);
    return ok;
}

/* Build the 8.3 name that sits underneath a long one.
 *
 * Every entry has a short name whether the user ever sees it or not, and it
 * must be unique within the directory - hence the ~1, ~2 tail. Characters FAT
 * forbids are replaced rather than dropped, so two different long names never
 * collapse to the same short one by accident. */
static void make_short_name(const char* name, boot_uint8_t* output,
                            boot_uint32_t ordinal) {
    static const char forbidden[] = "\"*+,/:;<=>?[\\]|";
    const char* extension = (const char*)0;
    int base_length = 0;
    int needs_tail = 0;

    memset(output, ' ', 11);

    for (const char* cursor = name; *cursor; cursor++)
        if (*cursor == '.') extension = cursor + 1;
    /* A leading dot is part of the name, not an extension separator. */
    if (extension == name + 1) extension = (const char*)0;

    for (const char* cursor = name; *cursor && base_length < 8; cursor++) {
        char character = *cursor;
        if (extension && cursor >= extension - 1) break;
        if (character == ' ' || character == '.') { needs_tail = 1; continue; }
        for (const char* bad = forbidden; *bad; bad++)
            if (character == *bad) { character = '_'; break; }
        if (character >= 'a' && character <= 'z') character = (char)(character - 32);
        output[base_length++] = (boot_uint8_t)character;
    }
    if (!base_length) output[base_length++] = '_';

    if (extension) {
        int index = 0;
        for (const char* cursor = extension; *cursor && index < 3; cursor++) {
            char character = *cursor;
            for (const char* bad = forbidden; *bad; bad++)
                if (character == *bad) { character = '_'; break; }
            if (character >= 'a' && character <= 'z') character = (char)(character - 32);
            output[8 + index++] = (boot_uint8_t)character;
        }
    }

    /* Anything that did not fit, or that had to be altered, gets the numeric
       tail so it stays distinguishable. */
    if (ordinal || needs_tail || strlen(name) > 12) {
        char digits[8];
        int digit_count = 0;
        boot_uint32_t value = ordinal ? ordinal : 1;
        int position;

        do { digits[digit_count++] = (char)('0' + value % 10U); value /= 10U; }
        while (value);

        position = base_length;
        if (position + digit_count + 1 > 8) position = 8 - digit_count - 1;
        if (position < 1) position = 1;
        output[position++] = '~';
        while (digit_count--) output[position++] = (boot_uint8_t)digits[digit_count];
    }
}

/* Does `short_name` already exist in the directory starting at `cluster`? */
static int short_name_taken(FAT_VOLUME* fat, boot_uint32_t cluster,
                            const boot_uint8_t* short_name) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();
    boot_uint32_t entries_per_cluster;
    int taken = 0;

    if (!sector) return 1;
    entries_per_cluster = fat->sectors_per_cluster * ENTRIES_PER_SECTOR;

    while (cluster >= 2 && !cluster_is_end(cluster) && !taken) {
        for (boot_uint32_t index = 0; index < entries_per_cluster && !taken; index++) {
            const boot_uint8_t* raw;
            if (index % ENTRIES_PER_SECTOR == 0 &&
                !read_volume_sector(fat, cluster_first_sector(fat, cluster) +
                                    index / ENTRIES_PER_SECTOR, sector)) {
                taken = 1;
                break;
            }
            raw = sector + (index % ENTRIES_PER_SECTOR) * DIRECTORY_ENTRY_SIZE;
            if (raw[0] == ENTRY_END) { cluster = CLUSTER_MASK; break; }
            if (raw[0] == ENTRY_FREE) continue;
            if ((raw[11] & FAT_ATTRIBUTE_LONG_NAME) == FAT_ATTRIBUTE_LONG_NAME) continue;
            if (memcmp(raw, short_name, 11) == 0) taken = 1;
        }
        if (cluster == CLUSTER_MASK) break;
        cluster = next_cluster(fat, cluster);
    }
    free_page(sector);
    return taken;
}

/* How many 32-byte slots a name needs: one short entry plus a long-name entry
   per 13 characters. A name that fits 8.3 exactly still gets a long entry when
   its case would otherwise be lost. */
static boot_uint32_t slots_needed(const char* name) {
    boot_uint64_t length = strlen(name);
    return 1 + (boot_uint32_t)((length + LONG_NAME_CHARS - 1) / LONG_NAME_CHARS);
}

/* Find `count` consecutive free slots, extending the directory if needed.
   Returns the entry index within the chain, and the cluster holding it. */
static int find_free_slots(FAT_VOLUME* fat, boot_uint32_t directory_cluster,
                           boot_uint32_t count, boot_uint32_t* out_cluster,
                           boot_uint32_t* out_index) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();
    boot_uint32_t entries_per_cluster;
    boot_uint32_t cluster = directory_cluster;
    boot_uint32_t run = 0;
    boot_uint32_t run_cluster = 0;
    boot_uint32_t run_index = 0;

    if (!sector) return 0;
    entries_per_cluster = fat->sectors_per_cluster * ENTRIES_PER_SECTOR;

    for (;;) {
        for (boot_uint32_t index = 0; index < entries_per_cluster; index++) {
            const boot_uint8_t* raw;
            if (index % ENTRIES_PER_SECTOR == 0 &&
                !read_volume_sector(fat, cluster_first_sector(fat, cluster) +
                                    index / ENTRIES_PER_SECTOR, sector)) {
                free_page(sector);
                return 0;
            }
            raw = sector + (index % ENTRIES_PER_SECTOR) * DIRECTORY_ENTRY_SIZE;
            if (raw[0] == ENTRY_END || raw[0] == ENTRY_FREE) {
                if (!run) { run_cluster = cluster; run_index = index; }
                if (++run == count) {
                    free_page(sector);
                    *out_cluster = run_cluster;
                    *out_index = run_index;
                    return 1;
                }
            } else {
                run = 0;
            }
        }
        {
            boot_uint32_t following = next_cluster(fat, cluster);
            if (cluster_is_end(following) || following < 2) {
                /* Out of room: grow the directory by one cluster. A run that
                   was building at the tail cannot continue across the join,
                   because the new cluster is not adjacent in the chain sense
                   this loop assumes - so it restarts there. */
                boot_uint32_t added = allocate_cluster(fat, cluster, 1);
                free_page(sector);
                if (!added) return 0;
                *out_cluster = added;
                *out_index = 0;
                return count <= fat->sectors_per_cluster * ENTRIES_PER_SECTOR;
            }
            cluster = following;
        }
    }
}

/* Write the long-name entries and the short entry into consecutive slots. */
static int write_directory_entries(FAT_VOLUME* fat, boot_uint32_t cluster,
                                   boot_uint32_t index, const char* name,
                                   const boot_uint8_t* short_name,
                                   boot_uint8_t attributes,
                                   boot_uint32_t first_cluster,
                                   boot_uint32_t size,
                                   boot_uint16_t date, boot_uint16_t time,
                                   FAT_ENTRY* result) {
    static const int offsets[LONG_NAME_CHARS] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    boot_uint64_t length = strlen(name);
    boot_uint32_t long_entries = (boot_uint32_t)
        ((length + LONG_NAME_CHARS - 1) / LONG_NAME_CHARS);
    boot_uint8_t checksum = short_name_checksum(short_name);
    boot_uint32_t entries_per_cluster =
        fat->sectors_per_cluster * ENTRIES_PER_SECTOR;
    boot_uint8_t entry[DIRECTORY_ENTRY_SIZE];
    boot_uint64_t first_sector = 0;
    boot_uint32_t first_offset = 0;

    /* Long-name entries come first on disk but hold the tail of the name:
       they are stored in reverse, highest sequence number first. */
    for (boot_uint32_t piece = 0; piece < long_entries; piece++) {
        boot_uint32_t sequence = long_entries - piece;
        boot_uint32_t base = (sequence - 1) * LONG_NAME_CHARS;
        boot_uint32_t slot = index + piece;
        boot_uint64_t sector_number;

        if (slot >= entries_per_cluster) return 0;
        memset(entry, 0, sizeof(entry));
        entry[0] = (boot_uint8_t)(sequence | (piece == 0 ? LONG_NAME_LAST : 0));
        entry[11] = FAT_ATTRIBUTE_LONG_NAME;
        entry[13] = checksum;

        for (int character = 0; character < LONG_NAME_CHARS; character++) {
            boot_uint32_t position = base + (boot_uint32_t)character;
            boot_uint16_t value;
            if (position < length) value = (boot_uint16_t)(unsigned char)name[position];
            else if (position == length) value = 0x0000;
            else value = 0xFFFF;
            write16(entry + offsets[character], value);
        }

        sector_number = cluster_first_sector(fat, cluster) + slot / ENTRIES_PER_SECTOR;
        if (piece == 0) {
            first_sector = sector_number;
            first_offset = (slot % ENTRIES_PER_SECTOR) * DIRECTORY_ENTRY_SIZE;
        }
        if (!patch_entry(fat, sector_number,
                         (slot % ENTRIES_PER_SECTOR) * DIRECTORY_ENTRY_SIZE, entry))
            return 0;
    }

    {
        boot_uint32_t slot = index + long_entries;
        boot_uint64_t sector_number;
        boot_uint32_t offset;

        if (slot >= entries_per_cluster) return 0;
        memset(entry, 0, sizeof(entry));
        memcpy(entry, short_name, 11);
        entry[11] = attributes;
        write16(entry + 14, time);      /* creation time */
        write16(entry + 16, date);      /* creation date */
        write16(entry + 18, date);      /* last access date */
        write16(entry + 20, (boot_uint16_t)(first_cluster >> 16));
        write16(entry + 22, time);      /* modification time */
        write16(entry + 24, date);      /* modification date */
        write16(entry + 26, (boot_uint16_t)first_cluster);
        write32(entry + 28, size);

        sector_number = cluster_first_sector(fat, cluster) + slot / ENTRIES_PER_SECTOR;
        offset = (slot % ENTRIES_PER_SECTOR) * DIRECTORY_ENTRY_SIZE;
        if (!patch_entry(fat, sector_number, offset, entry)) return 0;

        if (result) {
            memset(result, 0, sizeof(*result));
            memcpy(result->name, name, length + 1);
            result->attributes = attributes;
            result->size = size;
            result->first_cluster = first_cluster;
            result->modified_date = date;
            result->modified_time = time;
            result->entry_sector = sector_number;
            result->entry_offset = offset;
            result->first_entry_sector = long_entries ? first_sector : sector_number;
            result->first_entry_offset = long_entries ? first_offset : offset;
        }
    }
    return 1;
}

/* Split "\A\B\C" into the parent directory's cluster and the final name. */
static int split_path(FAT_VOLUME* fat, VOLUME* volume, const char* path,
                      boot_uint32_t* parent_cluster, char* leaf) {
    const char* last = path;
    const char* cursor;
    char parent[FAT_NAME_MAX];
    boot_uint64_t parent_length;
    FAT_ENTRY entry;

    for (cursor = path; *cursor; cursor++)
        if (*cursor == '\\') last = cursor + 1;
    if (!*last) return 0;

    {
        boot_uint64_t leaf_length = strlen(last);
        if (leaf_length >= FAT_NAME_MAX) return 0;
        memcpy(leaf, last, leaf_length + 1);
    }

    parent_length = (boot_uint64_t)(last - path);
    while (parent_length > 1 && path[parent_length - 1] == '\\') parent_length--;
    if (parent_length >= FAT_NAME_MAX) return 0;
    memcpy(parent, path, parent_length);
    parent[parent_length] = 0;

    if (!parent[0] || (parent[0] == '\\' && !parent[1])) {
        *parent_cluster = fat->root_cluster;
        return 1;
    }
    if (!fat32_stat(volume, parent, &entry)) return 0;
    if (!(entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) return 0;
    *parent_cluster = entry.first_cluster ? entry.first_cluster : fat->root_cluster;
    return 1;
}

int fat32_create(VOLUME* volume, const char* path, int directory,
                 FAT_ENTRY* entry) {
    FAT_VOLUME* fat = mount_for(volume);
    char leaf[FAT_NAME_MAX];
    boot_uint32_t parent_cluster;
    boot_uint32_t slot_cluster;
    boot_uint32_t slot_index;
    boot_uint8_t short_name[11];
    boot_uint32_t first_cluster = 0;
    FAT_ENTRY existing;
    RTC_TIME now;
    boot_uint16_t date;
    boot_uint16_t time;

    if (!fat || !path || !entry) return 0;
    if (fat32_stat(volume, path, &existing)) return 0;   /* already there */
    if (!split_path(fat, volume, path, &parent_cluster, leaf)) return 0;

    for (boot_uint32_t ordinal = 0; ordinal < 1000; ordinal++) {
        make_short_name(leaf, short_name, ordinal);
        if (!short_name_taken(fat, parent_cluster, short_name)) break;
        if (ordinal == 999) return 0;
    }

    if (!find_free_slots(fat, parent_cluster, slots_needed(leaf),
                         &slot_cluster, &slot_index)) return 0;

    if (directory) {
        first_cluster = allocate_cluster(fat, 0, 1);
        if (!first_cluster) return 0;
    }

    rtc_read(&now);
    date = rtc_fat_date(&now);
    time = rtc_fat_time(&now);

    if (!write_directory_entries(fat, slot_cluster, slot_index, leaf, short_name,
                                 (boot_uint8_t)(directory ? FAT_ATTRIBUTE_DIRECTORY
                                                          : FAT_ATTRIBUTE_ARCHIVE),
                                 first_cluster, 0, date, time, entry)) {
        if (first_cluster) free_chain(fat, first_cluster);
        return 0;
    }

    if (directory) {
        /* A new directory needs its own "." and ".." before anything can
           traverse it. ".." pointing at the root is stored as cluster 0, which
           is the convention every FAT implementation expects. */
        boot_uint8_t dot[DIRECTORY_ENTRY_SIZE];
        boot_uint32_t parent_link =
            parent_cluster == fat->root_cluster ? 0 : parent_cluster;

        memset(dot, 0, sizeof(dot));
        memset(dot, ' ', 11);
        dot[0] = '.';
        dot[11] = FAT_ATTRIBUTE_DIRECTORY;
        write16(dot + 20, (boot_uint16_t)(first_cluster >> 16));
        write16(dot + 22, time);
        write16(dot + 24, date);
        write16(dot + 26, (boot_uint16_t)first_cluster);
        if (!patch_entry(fat, cluster_first_sector(fat, first_cluster), 0, dot))
            return 0;

        memset(dot, 0, sizeof(dot));
        memset(dot, ' ', 11);
        dot[0] = '.';
        dot[1] = '.';
        dot[11] = FAT_ATTRIBUTE_DIRECTORY;
        write16(dot + 20, (boot_uint16_t)(parent_link >> 16));
        write16(dot + 22, time);
        write16(dot + 24, date);
        write16(dot + 26, (boot_uint16_t)parent_link);
        if (!patch_entry(fat, cluster_first_sector(fat, first_cluster),
                         DIRECTORY_ENTRY_SIZE, dot))
            return 0;
    }

    commit(fat);
    return 1;
}

/* Push the current size, first cluster and timestamp back into the entry. */
static int flush_entry(FAT_VOLUME* fat, FAT_ENTRY* entry) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();
    boot_uint8_t* raw;
    int ok;

    if (!sector) return 0;
    if (!read_volume_sector(fat, entry->entry_sector, sector)) {
        free_page(sector);
        return 0;
    }
    raw = sector + entry->entry_offset;
    write16(raw + 20, (boot_uint16_t)(entry->first_cluster >> 16));
    write16(raw + 22, entry->modified_time);
    write16(raw + 24, entry->modified_date);
    write16(raw + 26, (boot_uint16_t)entry->first_cluster);
    write32(raw + 28, entry->size);
    ok = write_volume_sector(fat, entry->entry_sector, sector);
    free_page(sector);
    return ok;
}

boot_uint32_t fat32_write(VOLUME* volume, FAT_ENTRY* entry,
                          boot_uint32_t offset, const void* buffer,
                          boot_uint32_t length) {
    FAT_VOLUME* fat = mount_for(volume);
    const boot_uint8_t* input = (const boot_uint8_t*)buffer;
    boot_uint8_t* sector;
    boot_uint32_t cluster;
    boot_uint32_t cluster_bytes;
    boot_uint32_t written = 0;
    boot_uint32_t start_offset = offset;
    RTC_TIME now;

    if (!fat || !entry || !buffer || !length) return 0;
    if (entry->attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;

    cluster_bytes = fat->sectors_per_cluster * fat->bytes_per_sector;

    if (!entry->first_cluster) {
        entry->first_cluster = allocate_cluster(fat, 0, 0);
        if (!entry->first_cluster) return 0;
    }
    cluster = entry->first_cluster;

    /* Walk to the cluster holding `offset`, growing the chain if the write
       starts past the current end. */
    while (offset >= cluster_bytes) {
        boot_uint32_t following = next_cluster(fat, cluster);
        if (cluster_is_end(following) || following < 2) {
            following = allocate_cluster(fat, cluster, 0);
            if (!following) return 0;
        }
        cluster = following;
        offset -= cluster_bytes;
    }

    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return 0;

    while (written < length) {
        boot_uint32_t sector_index;
        boot_uint32_t sector_offset;
        boot_uint32_t chunk;
        boot_uint64_t sector_number;

        if (offset >= cluster_bytes) {
            boot_uint32_t following = next_cluster(fat, cluster);
            if (cluster_is_end(following) || following < 2) {
                following = allocate_cluster(fat, cluster, 0);
                if (!following) break;
            }
            cluster = following;
            offset -= cluster_bytes;
            continue;
        }

        sector_index = offset / fat->bytes_per_sector;
        sector_offset = offset % fat->bytes_per_sector;
        sector_number = cluster_first_sector(fat, cluster) + sector_index;

        /* Whole sectors go straight from the caller's buffer, as many at a
           time as the cluster holds. */
        if (!sector_offset && length - written >= fat->bytes_per_sector &&
            !((boot_uint64_t)(input + written) & 3)) {
            boot_uint32_t run = (length - written) / fat->bytes_per_sector;
            boot_uint32_t left = fat->sectors_per_cluster - sector_index;

            if (run > left) run = left;
            if (!write_volume_sectors(fat, sector_number, run, input + written))
                break;
            chunk = run * fat->bytes_per_sector;
            written += chunk;
            offset += chunk;
            continue;
        }

        chunk = fat->bytes_per_sector - sector_offset;
        if (chunk > length - written) chunk = length - written;

        /* A partial sector has to be read before it is written, or the bytes
           either side of the chunk would be lost. */
        if (chunk != fat->bytes_per_sector) {
            if (!read_volume_sector(fat, sector_number, sector)) break;
        }
        memcpy(sector + sector_offset, input + written, chunk);
        if (!write_volume_sector(fat, sector_number, sector)) break;

        written += chunk;
        offset += chunk;
    }
    free_page(sector);

    if (written) {
        /* The file end is where the caller's write finished, not where the
           local cursor ended up - `offset` was consumed walking the chain. */
        boot_uint32_t end = start_offset + written;
        if (end > entry->size) entry->size = end;
        rtc_read(&now);
        entry->modified_date = rtc_fat_date(&now);
        entry->modified_time = rtc_fat_time(&now);
        (void)flush_entry(fat, entry);
        commit(fat);
    }
    return written;
}

/* Mark the short entry and every long-name entry ahead of it as free. */
static int erase_entries(FAT_VOLUME* fat, const FAT_ENTRY* entry) {
    boot_uint64_t sector_number = entry->first_entry_sector;
    boot_uint32_t offset = entry->first_entry_offset;

    for (;;) {
        if (!mark_entry_free(fat, sector_number, offset)) return 0;
        if (sector_number == entry->entry_sector && offset == entry->entry_offset)
            return 1;
        offset += DIRECTORY_ENTRY_SIZE;
        if (offset >= fat->bytes_per_sector) {
            offset = 0;
            sector_number++;
        }
        /* Guard against a malformed run walking off into the volume. */
        if (sector_number > entry->entry_sector + 2) return 0;
    }
}

static int directory_is_empty(VOLUME* volume, const FAT_ENTRY* entry) {
    FAT_DIRECTORY directory;
    FAT_ENTRY child;

    memset(&directory, 0, sizeof(directory));
    directory.volume = volume;
    directory.cluster = entry->first_cluster;

    while (fat32_readdir(&directory, &child)) {
        if (child.name[0] == '.' && !child.name[1]) continue;
        if (child.name[0] == '.' && child.name[1] == '.' && !child.name[2]) continue;
        return 0;
    }
    return 1;
}

int fat32_remove(VOLUME* volume, const char* path) {
    FAT_VOLUME* fat = mount_for(volume);
    FAT_ENTRY entry;

    if (!fat || !path) return 0;
    if (!fat32_stat(volume, path, &entry)) return 0;
    /* Refusing the root is not pedantry: it has no directory entry to erase,
       so the code below would corrupt whatever sector zero happens to be. */
    if (!entry.entry_sector) return 0;

    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) {
        if (!directory_is_empty(volume, &entry)) return 0;
    }
    if (entry.first_cluster) free_chain(fat, entry.first_cluster);
    if (!erase_entries(fat, &entry)) return 0;
    commit(fat);
    return 1;
}

int fat32_rename(VOLUME* volume, const char* from, const char* to) {
    FAT_VOLUME* fat = mount_for(volume);
    FAT_ENTRY source;
    FAT_ENTRY target;
    FAT_ENTRY created;
    char leaf[FAT_NAME_MAX];
    boot_uint32_t parent_cluster;
    boot_uint32_t slot_cluster;
    boot_uint32_t slot_index;
    boot_uint8_t short_name[11];

    if (!fat || !from || !to) return 0;
    if (!fat32_stat(volume, from, &source)) return 0;
    if (fat32_stat(volume, to, &target)) return 0;      /* destination exists */
    if (!source.entry_sector) return 0;                 /* not the root */
    if (!split_path(fat, volume, to, &parent_cluster, leaf)) return 0;

    for (boot_uint32_t ordinal = 0; ordinal < 1000; ordinal++) {
        make_short_name(leaf, short_name, ordinal);
        if (!short_name_taken(fat, parent_cluster, short_name)) break;
        if (ordinal == 999) return 0;
    }
    if (!find_free_slots(fat, parent_cluster, slots_needed(leaf),
                         &slot_cluster, &slot_index)) return 0;

    /* Write the new entry before erasing the old one. If the machine dies in
       between, the result is one file reachable by two names - recoverable.
       The other order would lose the data outright. */
    if (!write_directory_entries(fat, slot_cluster, slot_index, leaf, short_name,
                                 source.attributes, source.first_cluster,
                                 source.size, source.modified_date,
                                 source.modified_time, &created))
        return 0;
    if (!erase_entries(fat, &source)) return 0;
    commit(fat);
    return 1;
}

int fat32_set_attributes(VOLUME* volume, const FAT_ENTRY* entry,
                         boot_uint8_t attributes) {
    FAT_VOLUME* fat = mount_for(volume);
    boot_uint8_t* sector;
    int ok;

    if (!fat || !entry || !entry->entry_sector) return 0;
    /* The directory bit describes what the entry is, not how it may be used;
       changing it would turn a file into a directory that has no . or .. */
    attributes = (boot_uint8_t)((attributes & ~FAT_ATTRIBUTE_DIRECTORY) |
                                (entry->attributes & FAT_ATTRIBUTE_DIRECTORY));

    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return 0;
    if (!read_volume_sector(fat, entry->entry_sector, sector)) {
        free_page(sector);
        return 0;
    }
    sector[entry->entry_offset + 11] = attributes;
    ok = write_volume_sector(fat, entry->entry_sector, sector);
    free_page(sector);
    return ok;
}

/* ---- Making a filesystem ------------------------------------------------ */

#define FORMAT_RESERVED_SECTORS 32
#define FORMAT_FAT_COPIES 2
#define FORMAT_BACKUP_BOOT_SECTOR 6
#define FORMAT_ROOT_CLUSTER 2
#define FORMAT_MEDIA_DESCRIPTOR 0xF8

/* The bounds that make a filesystem FAT32 rather than FAT16 or nothing.
 *
 * The lower one is not a preference: a volume with fewer clusters than this is
 * FAT16 by definition, whatever the boot sector claims, and every other
 * implementation will read it as FAT16 and see rubbish. */
#define FAT32_MINIMUM_CLUSTERS 65525U
#define FAT32_MAXIMUM_CLUSTERS 268435445U

/* How large the table has to be to describe every cluster it creates - which
   is circular, because the table's own size reduces the space left for
   clusters. This is the closed form from the specification rather than a
   loop that converges. */
static boot_uint32_t format_fat_size(boot_uint64_t total_sectors,
                                     boot_uint32_t sectors_per_cluster) {
    boot_uint64_t usable = total_sectors - FORMAT_RESERVED_SECTORS;
    boot_uint64_t denominator =
        ((256ULL * sectors_per_cluster) + FORMAT_FAT_COPIES) / 2ULL;

    if (!denominator) return 0;
    return (boot_uint32_t)((usable + denominator - 1ULL) / denominator);
}

static boot_uint32_t format_cluster_count(boot_uint64_t total_sectors,
                                          boot_uint32_t sectors_per_cluster,
                                          boot_uint32_t fat_size) {
    boot_uint64_t data = total_sectors - FORMAT_RESERVED_SECTORS -
                         (boot_uint64_t)fat_size * FORMAT_FAT_COPIES;
    if (data > total_sectors) return 0;      /* underflowed: nothing left */
    return (boot_uint32_t)(data / sectors_per_cluster);
}

static int format_cluster_is_legal(boot_uint64_t total_sectors,
                                   boot_uint32_t sectors_per_cluster) {
    boot_uint32_t fat_size = format_fat_size(total_sectors, sectors_per_cluster);
    boot_uint32_t clusters =
        format_cluster_count(total_sectors, sectors_per_cluster, fat_size);

    if (!fat_size || !clusters) return 0;
    return clusters >= FAT32_MINIMUM_CLUSTERS && clusters <= FAT32_MAXIMUM_CLUSTERS;
}

/* Cluster size by volume size - the same table every other FAT32 formatter
 * uses, and for the same reason.
 *
 * This code used to pick the smallest cluster that was still legal, on the
 * argument that a small cluster wastes less on short files. That argument only
 * holds for small volumes. A 223 GB partition formatted with 1 KiB clusters
 * has 234 million of them, and the table describing them is 936 MiB - per
 * copy, of which FAT32 keeps two. Merely creating such a volume means zeroing
 * nearly two gigabytes, and every file written afterwards pays for a chain
 * hundreds of times longer than it needed to be. The wasted tail of a cluster
 * is measured in kilobytes; getting this wrong is measured in minutes. */
static boot_uint32_t format_default_cluster(boot_uint64_t total_sectors) {
    boot_uint64_t megabytes = total_sectors / 2048ULL;

    if (megabytes <= 260) return 1;         /* 512 B  - the FAT32 floor */
    if (megabytes <= 8192) return 8;        /* 4 KiB  - up to 8 GiB */
    if (megabytes <= 16384) return 16;      /* 8 KiB  - up to 16 GiB */
    if (megabytes <= 32768) return 32;      /* 16 KiB - up to 32 GiB */
    return 64;                              /* 32 KiB - everything larger */
}

/* Start from the table, then move in whichever direction legality demands:
   up when the count would exceed what FAT32 can address, down when it falls
   below the minimum that distinguishes FAT32 from FAT16. */
static boot_uint32_t format_choose_cluster(boot_uint64_t total_sectors) {
    boot_uint32_t wanted = format_default_cluster(total_sectors);

    for (boot_uint32_t size = wanted; size <= 128; size *= 2)
        if (format_cluster_is_legal(total_sectors, size)) return size;
    for (boot_uint32_t size = wanted / 2; size >= 1; size /= 2) {
        if (format_cluster_is_legal(total_sectors, size)) return size;
        if (size == 1) break;
    }
    return 0;
}

/* Clear a run of sectors, many at a time.
 *
 * One sector per command is the obvious way to write this and it is unusable.
 * A 223 GB volume needs an allocation table of about 1.8 million sectors, and
 * two copies of it - three and a half million round trips to the controller,
 * which is minutes at best and an hour at worst. On the 64 MB volumes a test
 * bench uses it is two thousand writes and finishes instantly, which is
 * exactly why the cost stayed invisible until it met a real disk.
 *
 * The block layer has always taken a count. This just uses it. */
#define FORMAT_CHUNK_SECTORS 256        /* 128 KiB per command */

static int format_fill(BLOCK_DEVICE* device, boot_uint64_t first,
                       boot_uint64_t count, const boot_uint8_t* zeros,
                       FAT_FORMAT_PROGRESS progress, boot_uint64_t done,
                       boot_uint64_t total) {
    boot_uint64_t written = 0;

    while (written < count) {
        boot_uint64_t chunk = count - written;
        if (chunk > FORMAT_CHUNK_SECTORS) chunk = FORMAT_CHUNK_SECTORS;
        if (!block_write(device, first + written, (boot_uint32_t)chunk, zeros))
            return 0;
        written += chunk;
        /* Often enough to look alive, rarely enough not to be the slow part. */
        if (progress && (written % (FORMAT_CHUNK_SECTORS * 64)) == 0)
            progress(done + written, total);
    }
    return 1;
}

int fat32_format(BLOCK_DEVICE* device, boot_uint64_t first_sector,
                 boot_uint64_t sector_count, const char* label,
                 boot_uint32_t serial, FAT_FORMAT_PROGRESS progress) {
    boot_uint8_t* sector;
    boot_uint8_t* zeros;
    boot_uint64_t cleared = 0;
    boot_uint64_t to_clear;
    boot_uint32_t sectors_per_cluster;
    boot_uint32_t fat_size;
    boot_uint32_t clusters;
    int ok = 0;

    if (!device || device->sector_size != SECTOR_SIZE) return 0;
    if (sector_count < FORMAT_RESERVED_SECTORS + 4) return 0;

    sectors_per_cluster = format_choose_cluster(sector_count);
    if (!sectors_per_cluster) return 0;      /* too small to be legal FAT32 */
    fat_size = format_fat_size(sector_count, sectors_per_cluster);
    clusters = format_cluster_count(sector_count, sectors_per_cluster, fat_size);

    sector = (boot_uint8_t*)alloc_page();
    /* One buffer of zeroes, reused for every chunk. Contiguous because the
       block layer hands it straight to a controller. */
    zeros = (boot_uint8_t*)alloc_pages(
        FORMAT_CHUNK_SECTORS * SECTOR_SIZE / PAGE_SIZE);
    if (!sector || !zeros) {
        if (sector) free_page(sector);
        if (zeros) free_pages(zeros, FORMAT_CHUNK_SECTORS * SECTOR_SIZE / PAGE_SIZE);
        return 0;
    }
    memset(zeros, 0, FORMAT_CHUNK_SECTORS * SECTOR_SIZE);
    to_clear = (boot_uint64_t)fat_size * FORMAT_FAT_COPIES + sectors_per_cluster;

    /* The boot sector, and its backup at sector 6. Both are written because a
       reader that finds the first one damaged looks for the second, and a
       volume with only one is a volume with no spare. */
    memset(sector, 0, SECTOR_SIZE);
    sector[0] = 0xEB; sector[1] = 0x58; sector[2] = 0x90;   /* jump, then nop */
    memcpy(sector + 3, "MSWIN4.1", 8);   /* what every implementation expects */
    write16(sector + 11, SECTOR_SIZE);
    sector[13] = (boot_uint8_t)sectors_per_cluster;
    write16(sector + 14, FORMAT_RESERVED_SECTORS);
    sector[16] = FORMAT_FAT_COPIES;
    write16(sector + 17, 0);             /* no fixed root directory on FAT32 */
    write16(sector + 19, 0);             /* the 16-bit total is unused here */
    sector[21] = FORMAT_MEDIA_DESCRIPTOR;
    write16(sector + 22, 0);             /* nor the 16-bit FAT size */
    write16(sector + 24, 63);            /* geometry nothing reads any more, */
    write16(sector + 26, 255);           /* but tools complain when it is 0 */
    write32(sector + 28, (boot_uint32_t)first_sector);
    write32(sector + 32, (boot_uint32_t)sector_count);
    write32(sector + 36, fat_size);
    write16(sector + 40, 0);             /* both FATs live, first is active */
    write16(sector + 42, 0);             /* filesystem version 0.0 */
    write32(sector + 44, FORMAT_ROOT_CLUSTER);
    write16(sector + 48, FSINFO_SECTOR);
    write16(sector + 50, FORMAT_BACKUP_BOOT_SECTOR);
    sector[64] = 0x80;                   /* drive number, by convention */
    sector[66] = 0x29;                   /* says the three fields below exist */
    write32(sector + 67, serial);
    {
        /* Eleven bytes, space padded, never terminated. */
        int index = 0;
        for (; index < 11 && label && label[index]; index++)
            sector[71 + index] = (boot_uint8_t)label[index];
        for (; index < 11; index++) sector[71 + index] = ' ';
    }
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (!block_write(device, first_sector, 1, sector)) goto done;
    if (!block_write(device, first_sector + FORMAT_BACKUP_BOOT_SECTOR, 1, sector))
        goto done;

    /* FSInfo, and its backup alongside the backup boot sector. The free count
       is what makes `dir` able to report free space without walking the whole
       table on every call. */
    memset(sector, 0, SECTOR_SIZE);
    write32(sector + 0, FSINFO_LEAD_SIGNATURE);
    write32(sector + 484, FSINFO_STRUCT_SIGNATURE);
    write32(sector + FSINFO_FREE_COUNT_OFFSET, clusters - 1);  /* root took one */
    write32(sector + FSINFO_NEXT_FREE_OFFSET, FORMAT_ROOT_CLUSTER + 1);
    write32(sector + 508, FSINFO_TRAIL_SIGNATURE);

    if (!block_write(device, first_sector + FSINFO_SECTOR, 1, sector)) goto done;
    if (!block_write(device, first_sector + FORMAT_BACKUP_BOOT_SECTOR + FSINFO_SECTOR,
                     1, sector)) goto done;

    /* Clear both allocation tables. Every cluster free, before the first three
       entries are given their fixed meanings. This is the part that takes the
       time on a large volume, which is why it is the part that reports. */
    memset(sector, 0, SECTOR_SIZE);
    for (boot_uint32_t copy = 0; copy < FORMAT_FAT_COPIES; copy++) {
        boot_uint64_t start = first_sector + FORMAT_RESERVED_SECTORS +
                              (boot_uint64_t)copy * fat_size;
        if (!format_fill(device, start, fat_size, zeros, progress,
                         cleared, to_clear)) goto done;
        cleared += fat_size;
    }

    /* Entry 0 carries the media descriptor, entry 1 is reserved, and entry 2
       is the root directory: one cluster, already at the end of its chain. */
    write32(sector + 0, 0x0FFFFF00U | FORMAT_MEDIA_DESCRIPTOR);
    write32(sector + 4, 0x0FFFFFFFU);
    write32(sector + 8, 0x0FFFFFFFU);
    for (boot_uint32_t copy = 0; copy < FORMAT_FAT_COPIES; copy++) {
        boot_uint64_t start = first_sector + FORMAT_RESERVED_SECTORS +
                              (boot_uint64_t)copy * fat_size;
        if (!block_write(device, start, 1, sector)) goto done;
    }

    /* And an empty root directory. Zeroed rather than left as it was: a stale
       byte here reads as a directory entry. */
    memset(sector, 0, SECTOR_SIZE);
    {
        boot_uint64_t root = first_sector + FORMAT_RESERVED_SECTORS +
                             (boot_uint64_t)fat_size * FORMAT_FAT_COPIES;
        if (!format_fill(device, root, sectors_per_cluster, zeros, progress,
                         cleared, to_clear)) goto done;
        cleared += sectors_per_cluster;

        /* The label goes in twice, and both are needed. The copy in the boot
           sector is what this driver reads; the one here, as a directory entry
           with the volume-label attribute, is what mtools, Windows and every
           other implementation read. A volume with only the first reports
           itself as unlabelled everywhere except at home. */
        if (label && label[0]) {
            int index = 0;
            for (; index < 11 && label[index]; index++)
                sector[index] = (boot_uint8_t)label[index];
            for (; index < 11; index++) sector[index] = ' ';
            sector[11] = FAT_ATTRIBUTE_VOLUME_LABEL;
            if (!block_write(device, root, 1, sector)) goto done;
        }
    }

    if (progress) progress(to_clear, to_clear);
    ok = 1;
done:
    free_page(sector);
    free_pages(zeros, FORMAT_CHUNK_SECTORS * SECTOR_SIZE / PAGE_SIZE);
    return ok;
}
