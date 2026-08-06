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

static int read_volume_sector(FAT_VOLUME* fat, boot_uint64_t sector,
                              void* buffer) {
    return block_read(fat->volume->device, fat->volume->first_sector + sector,
                      1, buffer);
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
    boot_uint8_t* sector;
    boot_uint32_t offset = cluster * 4;
    boot_uint64_t fat_sector = fat->reserved_sectors + offset / fat->bytes_per_sector;
    boot_uint32_t within = offset % fat->bytes_per_sector;
    boot_uint32_t value;

    if (cluster < 2 || cluster >= fat->cluster_count + 2) return CLUSTER_MASK;
    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return CLUSTER_MASK;
    if (!read_volume_sector(fat, fat_sector, sector)) {
        free_page(sector);
        return CLUSTER_MASK;
    }
    value = read32(sector + within) & CLUSTER_MASK;
    free_page(sector);
    return value;
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
    free_page(sector);
    return 1;
}

boot_uint64_t fat32_total_bytes(VOLUME* volume) {
    FAT_VOLUME* fat = mount_for(volume);
    if (!fat) return 0;
    return (boot_uint64_t)fat->cluster_count * fat->sectors_per_cluster *
           fat->bytes_per_sector;
}

boot_uint64_t fat32_free_bytes(VOLUME* volume) {
    FAT_VOLUME* fat = mount_for(volume);
    boot_uint8_t* sector;
    boot_uint64_t free_clusters = 0;
    boot_uint32_t entries_per_sector;

    if (!fat) return 0;
    /* Walking the whole FAT costs a read per sector of it, so the answer is
       computed once and kept. It stays valid until writing exists. */
    if (fat->free_bytes_known) return fat->free_bytes;

    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return 0;
    entries_per_sector = fat->bytes_per_sector / 4;

    for (boot_uint32_t index = 0; index < fat->sectors_per_fat; index++) {
        if (!read_volume_sector(fat, fat->reserved_sectors + index, sector)) break;
        for (boot_uint32_t entry = 0; entry < entries_per_sector; entry++) {
            boot_uint32_t cluster = index * entries_per_sector + entry;
            if (cluster < 2) continue;
            if (cluster >= fat->cluster_count + 2) break;
            if ((read32(sector + entry * 4) & CLUSTER_MASK) == CLUSTER_FREE)
                free_clusters++;
        }
    }
    free_page(sector);

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
       each pass copies at most to the end of one sector, and a cluster is a
       whole number of sectors, so it lands exactly on the cluster boundary. */
    while (copied < length) {
        boot_uint32_t sector_index;
        boot_uint32_t sector_offset;
        boot_uint32_t chunk;

        if (offset >= cluster_bytes) {
            cluster = next_cluster(fat, cluster);
            if (cluster_is_end(cluster) || cluster < 2) break;
            offset -= cluster_bytes;
            continue;
        }

        sector_index = offset / fat->bytes_per_sector;
        sector_offset = offset % fat->bytes_per_sector;
        if (!read_volume_sector(fat,
                cluster_first_sector(fat, cluster) + sector_index, sector)) break;

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

#define FSINFO_SECTOR 1
#define FSINFO_LEAD_SIGNATURE 0x41615252U
#define FSINFO_STRUCT_SIGNATURE 0x61417272U
#define FSINFO_TRAIL_SIGNATURE 0xAA550000U
#define FSINFO_FREE_COUNT_OFFSET 488
#define FSINFO_NEXT_FREE_OFFSET 492

static int write_volume_sector(FAT_VOLUME* fat, boot_uint64_t sector,
                               const void* buffer) {
    return block_write(fat->volume->device, fat->volume->first_sector + sector,
                       1, buffer);
}

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

/* Set one FAT entry in every copy of the table. */
static int write_fat_entry(FAT_VOLUME* fat, boot_uint32_t cluster,
                           boot_uint32_t value) {
    boot_uint8_t* sector;
    boot_uint32_t byte_offset = cluster * 4;
    boot_uint64_t relative = byte_offset / fat->bytes_per_sector;
    boot_uint32_t within = byte_offset % fat->bytes_per_sector;
    int ok = 1;

    if (cluster < 2 || cluster >= fat->cluster_count + 2) return 0;
    sector = (boot_uint8_t*)alloc_page();
    if (!sector) return 0;

    for (boot_uint32_t copy = 0; copy < fat->fat_count; copy++) {
        boot_uint64_t target = fat->reserved_sectors +
                               (boot_uint64_t)copy * fat->sectors_per_fat + relative;
        if (!read_volume_sector(fat, target, sector)) { ok = 0; break; }
        /* The top four bits of a FAT32 entry are reserved and must be kept as
           they were found, not overwritten with zeroes. */
        write32(sector + within,
                (read32(sector + within) & 0xF0000000U) | (value & CLUSTER_MASK));
        if (!write_volume_sector(fat, target, sector)) { ok = 0; break; }
    }

    free_page(sector);
    /* The cached free-space figure no longer reflects the disk. */
    fat->free_bytes_known = 0;
    return ok;
}

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
        write32(sector + FSINFO_FREE_COUNT_OFFSET, clusters);
        (void)write_volume_sector(fat, FSINFO_SECTOR, sector);
    }
    free_page(sector);
}

/* Find a free cluster, claim it, and link it after `previous` when given. */
static boot_uint32_t allocate_cluster(FAT_VOLUME* fat, boot_uint32_t previous) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();
    boot_uint32_t entries_per_sector;
    boot_uint32_t found = 0;

    if (!sector) return 0;
    entries_per_sector = fat->bytes_per_sector / 4;

    for (boot_uint32_t index = 0; index < fat->sectors_per_fat && !found; index++) {
        if (!read_volume_sector(fat, fat->reserved_sectors + index, sector)) break;
        for (boot_uint32_t slot = 0; slot < entries_per_sector; slot++) {
            boot_uint32_t cluster = index * entries_per_sector + slot;
            if (cluster < 2) continue;
            if (cluster >= fat->cluster_count + 2) break;
            if ((read32(sector + slot * 4) & CLUSTER_MASK) == CLUSTER_FREE) {
                found = cluster;
                break;
            }
        }
    }
    free_page(sector);
    if (!found) return 0;

    if (!write_fat_entry(fat, found, CLUSTER_MASK)) return 0;   /* end of chain */
    if (previous && !write_fat_entry(fat, previous, found)) return 0;

    /* A freshly allocated cluster still holds whatever was there before. For a
       directory that would be read as a screenful of garbage entries. */
    {
        boot_uint8_t* blank = (boot_uint8_t*)alloc_page();
        if (blank) {
            memset(blank, 0, fat->bytes_per_sector);
            for (boot_uint32_t s = 0; s < fat->sectors_per_cluster; s++)
                (void)write_volume_sector(fat, cluster_first_sector(fat, found) + s,
                                          blank);
            free_page(blank);
        }
    }
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
                boot_uint32_t added = allocate_cluster(fat, cluster);
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
        first_cluster = allocate_cluster(fat, 0);
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

    update_fsinfo(fat);
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
        entry->first_cluster = allocate_cluster(fat, 0);
        if (!entry->first_cluster) return 0;
    }
    cluster = entry->first_cluster;

    /* Walk to the cluster holding `offset`, growing the chain if the write
       starts past the current end. */
    while (offset >= cluster_bytes) {
        boot_uint32_t following = next_cluster(fat, cluster);
        if (cluster_is_end(following) || following < 2) {
            following = allocate_cluster(fat, cluster);
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
                following = allocate_cluster(fat, cluster);
                if (!following) break;
            }
            cluster = following;
            offset -= cluster_bytes;
            continue;
        }

        sector_index = offset / fat->bytes_per_sector;
        sector_offset = offset % fat->bytes_per_sector;
        sector_number = cluster_first_sector(fat, cluster) + sector_index;
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
        update_fsinfo(fat);
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
    update_fsinfo(fat);
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
    update_fsinfo(fat);
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
