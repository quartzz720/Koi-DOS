#include "partition.h"
#include "memory.h"
#include "string.h"

/* Partition table parsing, in the order the formats demand.
 *
 * GPT is checked first, and the reason is the protective MBR. A GPT disk
 * carries a fake MBR in sector 0 describing one partition of type 0xEE that
 * spans the whole disk, put there so that a pre-GPT tool sees a full disk and
 * declines to repartition it. Looking at the MBR first would therefore find
 * one enormous partition of an unrecognised type on every modern disk.
 *
 * The third case has no partition table at all: a filesystem written directly
 * to the whole device. Quick-formatted USB sticks look like this, and so does
 * the esp.img that qemu.sh builds with mkfs.vfat. */

#define SECTOR_SIZE 512
#define MBR_SIGNATURE_OFFSET 0x1FE
#define MBR_PARTITION_OFFSET 0x1BE
#define MBR_PARTITION_COUNT 4
#define MBR_TYPE_GPT_PROTECTIVE 0xEE
#define MBR_TYPE_EMPTY 0x00

#define GPT_HEADER_LBA 1
#define GPT_ENTRY_MAX 128

static VOLUME volumes[VOLUME_MAX];
static boot_uint32_t volumes_found;
static boot_uint32_t serials[VOLUME_MAX];

typedef struct __attribute__((packed)) {
    char signature[8];        /* "EFI PART" */
    boot_uint32_t revision;
    boot_uint32_t header_size;
    boot_uint32_t header_crc32;
    boot_uint32_t reserved;
    boot_uint64_t current_lba;
    boot_uint64_t backup_lba;
    boot_uint64_t first_usable_lba;
    boot_uint64_t last_usable_lba;
    boot_uint8_t disk_guid[16];
    boot_uint64_t entry_array_lba;
    boot_uint32_t entry_count;
    boot_uint32_t entry_size;
    boot_uint32_t entry_array_crc32;
} GPT_HEADER;

typedef struct __attribute__((packed)) {
    boot_uint8_t type_guid[16];
    boot_uint8_t unique_guid[16];
    boot_uint64_t first_lba;
    boot_uint64_t last_lba;
    boot_uint64_t attributes;
    boot_uint16_t name[36];   /* UTF-16 */
} GPT_ENTRY;

/* The serial sits at offset 67 of a FAT boot sector, and is only there when
   the extended-signature byte at 66 says so. */
static boot_uint32_t serial_of(const boot_uint8_t* sector) {
    if (sector[66] != 0x29) return 0;
    return (boot_uint32_t)sector[67] | ((boot_uint32_t)sector[68] << 8) |
           ((boot_uint32_t)sector[69] << 16) | ((boot_uint32_t)sector[70] << 24);
}

static int add_volume(BLOCK_DEVICE* device, boot_uint64_t first,
                      boot_uint64_t count, const boot_uint8_t* sector) {
    VOLUME* volume;

    if (volumes_found >= VOLUME_MAX || !count) return 0;
    volume = &volumes[volumes_found];
    memset(volume, 0, sizeof(*volume));
    volume->device = device;
    volume->first_sector = first;
    volume->sector_count = count;
    serials[volumes_found] = serial_of(sector);
    volumes_found++;
    return 1;
}

/* Does this sector look like a FAT boot sector? Checked before a volume is
   accepted, so that swap partitions and other non-FAT regions never take up a
   drive letter. */
static int looks_like_fat(const boot_uint8_t* sector) {
    boot_uint16_t bytes_per_sector;
    boot_uint8_t sectors_per_cluster;

    if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;
    bytes_per_sector = (boot_uint16_t)(sector[11] | (sector[12] << 8));
    sectors_per_cluster = sector[13];
    if (bytes_per_sector != 512 && bytes_per_sector != 1024 &&
        bytes_per_sector != 2048 && bytes_per_sector != 4096) return 0;
    /* Cluster size is always a power of two from 1 to 128 sectors. */
    if (!sectors_per_cluster || (sectors_per_cluster & (sectors_per_cluster - 1)))
        return 0;
    /* Reserved sector count is never zero on a FAT volume. */
    if (!(sector[14] | (sector[15] << 8))) return 0;
    return 1;
}

static int try_gpt(BLOCK_DEVICE* device, boot_uint8_t* sector) {
    GPT_HEADER header;
    boot_uint8_t* entries;
    boot_uint32_t entry_count;
    boot_uint32_t entry_size;
    boot_uint32_t per_sector;
    int found = 0;

    if (!block_read(device, GPT_HEADER_LBA, 1, sector)) return 0;
    memcpy(&header, sector, sizeof(header));
    if (memcmp(header.signature, "EFI PART", 8) != 0) return 0;

    entry_count = header.entry_count;
    entry_size = header.entry_size;
    if (!entry_size || entry_size > SECTOR_SIZE) return 0;
    if (entry_count > GPT_ENTRY_MAX) entry_count = GPT_ENTRY_MAX;

    entries = (boot_uint8_t*)alloc_page();
    if (!entries) return 0;
    per_sector = SECTOR_SIZE / entry_size;

    for (boot_uint32_t index = 0; index < entry_count; index++) {
        GPT_ENTRY entry;
        boot_uint64_t entry_lba = header.entry_array_lba + index / per_sector;
        boot_uint8_t* raw;

        if (!block_read(device, entry_lba, 1, entries)) break;
        raw = entries + (index % per_sector) * entry_size;
        memcpy(&entry, raw, sizeof(entry) < entry_size ? sizeof(entry) : entry_size);

        /* An all-zero type GUID marks an unused slot. */
        {
            int empty = 1;
            for (int byte = 0; byte < 16; byte++)
                if (entry.type_guid[byte]) { empty = 0; break; }
            if (empty) continue;
        }
        if (entry.last_lba < entry.first_lba) continue;

        if (!block_read(device, entry.first_lba, 1, sector)) continue;
        if (!looks_like_fat(sector)) continue;
        if (add_volume(device, entry.first_lba,
                       entry.last_lba - entry.first_lba + 1, sector)) found++;
    }

    free_page(entries);
    return found;
}

static int try_mbr(BLOCK_DEVICE* device, boot_uint8_t* sector) {
    boot_uint8_t table[MBR_PARTITION_COUNT * 16];
    int found = 0;

    if (!block_read(device, 0, 1, sector)) return 0;
    if (sector[MBR_SIGNATURE_OFFSET] != 0x55 ||
        sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA) return 0;
    memcpy(table, sector + MBR_PARTITION_OFFSET, sizeof(table));

    for (int index = 0; index < MBR_PARTITION_COUNT; index++) {
        const boot_uint8_t* entry = table + index * 16;
        boot_uint8_t type = entry[4];
        boot_uint64_t first;
        boot_uint64_t count;

        if (type == MBR_TYPE_EMPTY) continue;
        /* Reaching a protective entry here means the GPT header was missing
           or corrupt. Treating it as a real partition would hand out a drive
           letter for the entire disk. */
        if (type == MBR_TYPE_GPT_PROTECTIVE) continue;

        first = (boot_uint64_t)entry[8] | ((boot_uint64_t)entry[9] << 8) |
                ((boot_uint64_t)entry[10] << 16) | ((boot_uint64_t)entry[11] << 24);
        count = (boot_uint64_t)entry[12] | ((boot_uint64_t)entry[13] << 8) |
                ((boot_uint64_t)entry[14] << 16) | ((boot_uint64_t)entry[15] << 24);
        if (!first || !count) continue;

        if (!block_read(device, first, 1, sector)) continue;
        if (!looks_like_fat(sector)) continue;
        if (add_volume(device, first, count, sector)) found++;
    }
    return found;
}

static int try_whole_device(BLOCK_DEVICE* device, boot_uint8_t* sector) {
    if (!block_read(device, 0, 1, sector)) return 0;
    if (!looks_like_fat(sector)) return 0;
    return add_volume(device, 0,
                      device->sector_count ? device->sector_count : 0xFFFFFFFFULL,
                      sector);
}

/* The boot volume takes Z:, and the rest follow at Y:, X: downward.
 *
 * It is found by matching the FAT serial the bootloader read from the device
 * the firmware actually loaded us from. Nothing falls back to "the first one":
 * on a machine booted from a USB stick, the first FAT volume the AHCI driver
 * can see is the internal disk's EFI System Partition, and handing that Z:
 * would point every command at the real system's boot files. */
static void assign_letters(boot_uint32_t boot_serial, int serial_known) {
    boot_uint32_t boot_index = VOLUME_MAX;
    char letter = 'Z';

    if (serial_known && boot_serial) {
        for (boot_uint32_t index = 0; index < volumes_found; index++)
            if (serials[index] == boot_serial) { boot_index = index; break; }
    }

    if (boot_index < volumes_found) {
        volumes[boot_index].is_boot_volume = 1;
        volumes[boot_index].letter = letter--;
    }
    for (boot_uint32_t index = 0; index < volumes_found; index++) {
        if (index == boot_index) continue;
        volumes[index].letter = letter--;
    }
}

boot_uint32_t partition_scan(boot_uint32_t boot_serial, int serial_known) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();

    volumes_found = 0;
    if (!sector) return 0;

    for (boot_uint32_t index = 0; index < block_device_count(); index++) {
        BLOCK_DEVICE* device = block_device(index);
        if (!device) continue;
        if (try_gpt(device, sector)) continue;
        if (try_mbr(device, sector)) continue;
        (void)try_whole_device(device, sector);
    }

    free_page(sector);
    assign_letters(boot_serial, serial_known);
    return volumes_found;
}

boot_uint32_t volume_count(void) {
    return volumes_found;
}

VOLUME* volume_at(boot_uint32_t index) {
    return index < volumes_found ? &volumes[index] : (VOLUME*)0;
}

VOLUME* volume_by_letter(char letter) {
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 32);
    for (boot_uint32_t index = 0; index < volumes_found; index++)
        if (volumes[index].letter == letter) return &volumes[index];
    return (VOLUME*)0;
}

VOLUME* volume_boot(void) {
    for (boot_uint32_t index = 0; index < volumes_found; index++)
        if (volumes[index].is_boot_volume) return &volumes[index];
    return (VOLUME*)0;
}
