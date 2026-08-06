#include "partition.h"
#include "memory.h"
#include "string.h"
#include "rtc.h"

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

static PARTITION partitions[PARTITION_MAX];
static boot_uint32_t partitions_found;

/* Remembered from the first scan, so a rescan after something changed the
   disks still identifies the boot volume the same way. The bootloader is the
   only source of this and it is not coming back. */
static boot_uint32_t remembered_boot_serial;
static int remembered_serial_known;

/* The GPT type GUID for an EFI System Partition, in the byte order it is
   stored: the first three fields are little-endian, the last two are not. */
static const boot_uint8_t ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

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

/* Record a partition whether or not we can read what is on it.
 *
 * Everything the disk carries is written down here, including the regions we
 * have no filesystem for: a tool that repartitions has to be able to say
 * "this is a 400 GB partition of an unknown type" rather than not mentioning
 * it at all, because silence is what makes people erase the wrong thing. */
static void add_partition(BLOCK_DEVICE* device, boot_uint32_t device_index,
                          boot_uint64_t first, boot_uint64_t count,
                          boot_uint8_t scheme, boot_uint8_t type,
                          int is_fat, int is_efi_system) {
    PARTITION* partition;

    if (partitions_found >= PARTITION_MAX || !count) return;
    partition = &partitions[partitions_found];
    memset(partition, 0, sizeof(*partition));
    partition->device = device;
    partition->device_index = device_index;
    partition->first_sector = first;
    partition->sector_count = count;
    partition->scheme = scheme;
    partition->type = type;
    partition->is_fat = is_fat;
    partition->is_efi_system = is_efi_system;

    /* Numbered per device, the way every partitioning tool names them. */
    partition->number = 1;
    for (boot_uint32_t index = 0; index < partitions_found; index++)
        if (partitions[index].device == device) partition->number++;

    partitions_found++;
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

static int try_gpt(BLOCK_DEVICE* device, boot_uint32_t device_index,
                   boot_uint8_t* sector) {
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

        {
            boot_uint64_t count = entry.last_lba - entry.first_lba + 1;
            int readable = block_read(device, entry.first_lba, 1, sector);
            int is_fat = readable && looks_like_fat(sector);

            add_partition(device, device_index, entry.first_lba, count,
                          PARTITION_SCHEME_GPT, 0, is_fat,
                          memcmp(entry.type_guid, ESP_TYPE_GUID, 16) == 0);
            if (is_fat && add_volume(device, entry.first_lba, count, sector))
                found++;
        }
    }

    free_page(entries);
    return found;
}

static int try_mbr(BLOCK_DEVICE* device, boot_uint32_t device_index,
                   boot_uint8_t* sector) {
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

        {
            int readable = block_read(device, first, 1, sector);
            int is_fat = readable && looks_like_fat(sector);

            add_partition(device, device_index, first, count,
                          PARTITION_SCHEME_MBR, type, is_fat, 0);
            if (is_fat && add_volume(device, first, count, sector)) found++;
        }
    }
    return found;
}

static int try_whole_device(BLOCK_DEVICE* device, boot_uint32_t device_index,
                            boot_uint8_t* sector) {
    boot_uint64_t count =
        device->sector_count ? device->sector_count : 0xFFFFFFFFULL;

    if (!block_read(device, 0, 1, sector)) return 0;
    if (!looks_like_fat(sector)) return 0;
    add_partition(device, device_index, 0, count, PARTITION_SCHEME_NONE, 0, 1, 0);
    return add_volume(device, 0, count, sector);
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

/* Fill in which partitions ended up with drive letters, now that they have
   been assigned. Done here rather than in add_partition because the letters do
   not exist until every volume is known. */
static void label_partitions(void) {
    for (boot_uint32_t p = 0; p < partitions_found; p++) {
        partitions[p].letter = 0;
        for (boot_uint32_t v = 0; v < volumes_found; v++) {
            if (volumes[v].device == partitions[p].device &&
                volumes[v].first_sector == partitions[p].first_sector) {
                partitions[p].letter = volumes[v].letter;
                break;
            }
        }
    }
}

boot_uint32_t partition_scan(boot_uint32_t boot_serial, int serial_known) {
    boot_uint8_t* sector = (boot_uint8_t*)alloc_page();

    remembered_boot_serial = boot_serial;
    remembered_serial_known = serial_known;

    volumes_found = 0;
    partitions_found = 0;
    if (!sector) return 0;

    for (boot_uint32_t index = 0; index < block_device_count(); index++) {
        BLOCK_DEVICE* device = block_device(index);
        if (!device) continue;
        if (try_gpt(device, index, sector)) continue;
        /* A GPT that described partitions but none we could mount still counts
           as read: falling through to the MBR would find the protective entry
           and describe the whole disk as one unknown partition. */
        if (partitions_found && partitions[partitions_found - 1].device == device &&
            partitions[partitions_found - 1].scheme == PARTITION_SCHEME_GPT)
            continue;
        if (try_mbr(device, index, sector)) continue;
        if (partitions_found && partitions[partitions_found - 1].device == device &&
            partitions[partitions_found - 1].scheme == PARTITION_SCHEME_MBR)
            continue;
        (void)try_whole_device(device, index, sector);
    }

    free_page(sector);
    assign_letters(boot_serial, serial_known);
    label_partitions();
    return volumes_found;
}

boot_uint32_t partition_count(void) {
    return partitions_found;
}

PARTITION* partition_at(boot_uint32_t index) {
    return index < partitions_found ? &partitions[index] : (PARTITION*)0;
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

void partition_set_system_volume(VOLUME* system) {
    VOLUME* loader = volume_boot();
    char letter = 'Z';

    if (!system || system == loader) return;

    for (boot_uint32_t index = 0; index < volumes_found; index++)
        volumes[index].is_boot_volume = 0;
    system->is_boot_volume = 1;
    system->letter = letter--;

    for (boot_uint32_t index = 0; index < volumes_found; index++) {
        if (&volumes[index] == system) continue;
        /* The loader's volume is given no letter rather than the next one.
           This is not protection - any other operating system sees an ordinary
           partition - but a name is how the shell reaches something, and what
           has no name cannot be deleted by accident. */
        if (&volumes[index] == loader) {
            volumes[index].letter = 0;
            continue;
        }
        volumes[index].letter = letter--;
    }
    label_partitions();
}

boot_uint32_t partition_rescan(void) {
    return partition_scan(remembered_boot_serial, remembered_serial_known);
}

/* ---- Writing a partition table ------------------------------------------ */

#define GPT_HEADER_SIZE 92
#define GPT_ENTRY_SIZE 128
#define GPT_ENTRY_COUNT 128
#define GPT_ENTRY_SECTORS ((GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) / SECTOR_SIZE)
#define GPT_FIRST_USABLE (2 + GPT_ENTRY_SECTORS)
#define GPT_ALIGNMENT 2048              /* one mebibyte, in 512-byte sectors */

/* The type GUID for a partition holding an ordinary filesystem, which is what
   Microsoft calls "basic data" and everyone else inherited. */
static const boot_uint8_t BASIC_DATA_TYPE_GUID[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

/* CRC-32 as GPT uses it: the reflected IEEE polynomial, computed a bit at a
   time rather than from a table. A table would be 1 KiB of .bss to save
   microseconds on something that runs twice per disk. */
static boot_uint32_t crc32(const boot_uint8_t* data, boot_uint32_t length) {
    boot_uint32_t remainder = 0xFFFFFFFFU;

    for (boot_uint32_t index = 0; index < length; index++) {
        remainder ^= data[index];
        for (int bit = 0; bit < 8; bit++)
            remainder = (remainder >> 1) ^ (0xEDB88320U & (~(remainder & 1U) + 1U));
    }
    return remainder ^ 0xFFFFFFFFU;
}

static void put32(boot_uint8_t* data, boot_uint32_t value) {
    data[0] = (boot_uint8_t)value;
    data[1] = (boot_uint8_t)(value >> 8);
    data[2] = (boot_uint8_t)(value >> 16);
    data[3] = (boot_uint8_t)(value >> 24);
}

static void put64(boot_uint8_t* data, boot_uint64_t value) {
    put32(data, (boot_uint32_t)value);
    put32(data + 4, (boot_uint32_t)(value >> 32));
}

/* A GUID that will not collide with anything on this machine.
 *
 * Not random: there is no entropy source here worth the name. It is built from
 * the clock and a counter, and stamped with the version and variant bits so
 * that other tools read it as a normal type-4 GUID. Good enough for
 * identifying partitions on one computer, and honest about not being more. */
static void make_guid(boot_uint8_t* guid, boot_uint32_t seed) {
    RTC_TIME now;
    static boot_uint32_t counter;
    boot_uint32_t words[4];

    rtc_read(&now);
    counter++;
    words[0] = ((boot_uint32_t)rtc_fat_date(&now) << 16) | rtc_fat_time(&now);
    words[1] = seed * 2654435761U;              /* Knuth's multiplicative hash */
    words[2] = (counter * 2654435761U) ^ words[0];
    words[3] = (words[1] ^ words[2]) + counter;

    for (int index = 0; index < 4; index++) put32(guid + index * 4, words[index]);
    guid[7] = (boot_uint8_t)((guid[7] & 0x0F) | 0x40);   /* version 4 */
    guid[8] = (boot_uint8_t)((guid[8] & 0x3F) | 0x80);   /* variant 1 */
}

/* An ASCII name into the UTF-16 field a GPT entry carries. */
static void put_name(boot_uint8_t* entry, const char* name) {
    for (int index = 0; index < 36; index++) {
        char character = name && name[index] ? name[index] : 0;
        entry[56 + index * 2] = (boot_uint8_t)character;
        entry[56 + index * 2 + 1] = 0;
        if (!character) break;
    }
}

int partition_write_gpt(BLOCK_DEVICE* device, const GPT_REQUEST* requests,
                        boot_uint32_t count) {
    boot_uint8_t* entries;
    boot_uint8_t* sector;
    boot_uint64_t total;
    boot_uint64_t first_usable;
    boot_uint64_t last_usable;
    boot_uint64_t cursor;
    boot_uint32_t entries_crc;
    boot_uint8_t disk_guid[16];
    int ok = 0;

    if (!device || device->sector_size != SECTOR_SIZE) return 0;
    if (!requests || !count || count > GPT_REQUEST_MAX) return 0;

    total = device->sector_count;
    /* Room for both copies of everything, and a partition after them. */
    if (total < GPT_FIRST_USABLE + GPT_ENTRY_SECTORS + GPT_ALIGNMENT * 2) return 0;

    first_usable = GPT_FIRST_USABLE;
    last_usable = total - GPT_ENTRY_SECTORS - 2;

    entries = (boot_uint8_t*)alloc_pages(
        (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    sector = (boot_uint8_t*)alloc_page();
    if (!entries || !sector) goto done;
    memset(entries, 0, GPT_ENTRY_COUNT * GPT_ENTRY_SIZE);

    /* Lay the partitions out before writing anything, so a request that does
       not fit fails with the disk untouched. */
    cursor = (first_usable + GPT_ALIGNMENT - 1) / GPT_ALIGNMENT * GPT_ALIGNMENT;
    for (boot_uint32_t index = 0; index < count; index++) {
        boot_uint8_t* entry = entries + index * GPT_ENTRY_SIZE;
        boot_uint64_t length = requests[index].sector_count;
        boot_uint64_t last;

        if (!length) length = last_usable - cursor + 1;   /* the rest of it */
        if (!length || cursor + length - 1 > last_usable) goto done;
        last = cursor + length - 1;

        if (requests[index].is_efi_system)
            memcpy(entry, ESP_TYPE_GUID, 16);
        else
            memcpy(entry, BASIC_DATA_TYPE_GUID, 16);
        make_guid(entry + 16, (boot_uint32_t)cursor + index);
        put64(entry + 32, cursor);
        put64(entry + 40, last);
        put64(entry + 48, 0);
        put_name(entry, requests[index].name);

        /* The next one starts at the following alignment boundary. */
        cursor = (last + 1 + GPT_ALIGNMENT - 1) / GPT_ALIGNMENT * GPT_ALIGNMENT;
    }

    entries_crc = crc32(entries, GPT_ENTRY_COUNT * GPT_ENTRY_SIZE);
    make_guid(disk_guid, (boot_uint32_t)total);

    /* The protective MBR first. It exists so that a tool which knows only
       MBRs sees one partition of an unrecognised type covering the whole disk,
       and declines to touch it rather than believing the disk is empty. */
    memset(sector, 0, SECTOR_SIZE);
    sector[MBR_PARTITION_OFFSET + 4] = MBR_TYPE_GPT_PROTECTIVE;
    sector[MBR_PARTITION_OFFSET + 1] = 0;     /* CHS start, meaningless now */
    sector[MBR_PARTITION_OFFSET + 2] = 2;
    sector[MBR_PARTITION_OFFSET + 3] = 0;
    sector[MBR_PARTITION_OFFSET + 5] = 0xFF;  /* CHS end, likewise */
    sector[MBR_PARTITION_OFFSET + 6] = 0xFF;
    sector[MBR_PARTITION_OFFSET + 7] = 0xFF;
    put32(sector + MBR_PARTITION_OFFSET + 8, 1);
    put32(sector + MBR_PARTITION_OFFSET + 12,
          total - 1 > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (boot_uint32_t)(total - 1));
    sector[MBR_SIGNATURE_OFFSET] = 0x55;
    sector[MBR_SIGNATURE_OFFSET + 1] = 0xAA;
    if (!block_write(device, 0, 1, sector)) goto done;

    /* Then both copies of the entry array. */
    for (boot_uint32_t index = 0; index < GPT_ENTRY_SECTORS; index++) {
        if (!block_write(device, 2 + index, 1, entries + index * SECTOR_SIZE))
            goto done;
        if (!block_write(device, total - 1 - GPT_ENTRY_SECTORS + index, 1,
                         entries + index * SECTOR_SIZE)) goto done;
    }

    /* And both headers. They differ only in which LBA each calls itself and
       where each says the other is - and in the checksum that follows. */
    for (int backup = 0; backup < 2; backup++) {
        boot_uint64_t self = backup ? total - 1 : 1;
        boot_uint64_t other = backup ? 1 : total - 1;
        boot_uint64_t array = backup ? total - 1 - GPT_ENTRY_SECTORS : 2;

        memset(sector, 0, SECTOR_SIZE);
        memcpy(sector, "EFI PART", 8);
        put32(sector + 8, 0x00010000U);          /* revision 1.0 */
        put32(sector + 12, GPT_HEADER_SIZE);
        put32(sector + 16, 0);                   /* checksum, filled in below */
        put32(sector + 20, 0);
        put64(sector + 24, self);
        put64(sector + 32, other);
        put64(sector + 40, first_usable);
        put64(sector + 48, last_usable);
        memcpy(sector + 56, disk_guid, 16);
        put64(sector + 72, array);
        put32(sector + 80, GPT_ENTRY_COUNT);
        put32(sector + 84, GPT_ENTRY_SIZE);
        put32(sector + 88, entries_crc);
        /* The header's own checksum covers the header with this field zero,
           which is why it is computed last and written after. */
        put32(sector + 16, crc32(sector, GPT_HEADER_SIZE));

        if (!block_write(device, self, 1, sector)) goto done;
    }

    ok = 1;
done:
    if (entries) free_pages(entries,
        (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (sector) free_page(sector);
    return ok;
}
