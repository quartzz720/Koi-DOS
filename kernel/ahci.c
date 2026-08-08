#include "ahci.h"
#include "memory.h"
#include "timer.h"
#include "string.h"
#include "block.h"

#define AHCI_GHC_AE 0x80000000U
#define AHCI_CAP_S64A 0x80000000U /* CAP bit 31: supports 64-bit addressing */
#define AHCI_PXCMD_ST 0x00000001U
#define AHCI_PXCMD_FRE 0x00000010U
#define AHCI_PXCMD_FR 0x00004000U
#define AHCI_PXCMD_CR 0x00008000U
#define AHCI_PXTFD_BSY 0x80U
#define AHCI_PXTFD_DRQ 0x08U
#define AHCI_PXIS_TFES 0x40000000U
#define SATA_SIGNATURE_ATA 0x00000101U
#define FIS_TYPE_REG_H2D 0x27U
#define ATA_CMD_READ_DMA_EXT 0x25U
#define ATA_CMD_WRITE_DMA_EXT 0x35U
#define ATA_CMD_IDENTIFY 0xECU

typedef struct {
    volatile boot_uint32_t clb, clbu, fb, fbu, is, ie, cmd, rsv0;
    volatile boot_uint32_t tfd, sig, ssts, sctl, serr, sact, ci, sntf;
    volatile boot_uint32_t fbs, devslp;
    boot_uint32_t rsv1[10];
    boot_uint32_t vendor[4];
} HBA_PORT;

typedef struct {
    boot_uint8_t reserved[0x100];
    HBA_PORT ports[32];
} HBA_MEMORY;

typedef struct {
    boot_uint16_t flags;
    boot_uint16_t prdt_length;
    volatile boot_uint32_t prdbc;
    boot_uint32_t ctba;
    boot_uint32_t ctbau;
    boot_uint32_t reserved[4];
} HBA_COMMAND_HEADER;

typedef struct {
    boot_uint32_t dba;
    boot_uint32_t dbau;
    boot_uint32_t reserved;
    boot_uint32_t dbc;
} HBA_PRDT_ENTRY;

typedef struct {
    boot_uint8_t cfis[64];
    boot_uint8_t acmd[16];
    boot_uint8_t reserved[48];
    HBA_PRDT_ENTRY prdt[1];
} HBA_COMMAND_TABLE;

/* One disk on one port of one controller.
 *
 * All of this used to be file-scope singletons, which said quietly that a
 * machine has one SATA controller with one disk on it. Machines do not: a
 * desktop board has six ports and people fill them, and the driver stopped at
 * the first one it found and never mentioned the rest. The controller pointer
 * moved into here with the rest of it, because two controllers can differ in
 * whether they can address memory above four gigabytes and picking the wrong
 * answer is a DMA write to a truncated address.
 *
 * Sixteen because that is more disks than anything this will run on, and the
 * table costs nothing. */
#define AHCI_MAX_DISKS 16

typedef struct {
    HBA_PORT* port;
    HBA_COMMAND_HEADER* command_list;
    HBA_COMMAND_TABLE* command_table;
    boot_uint64_t sectors;
    /* 64-bit addressing is optional in AHCI: bit 31 of the HBA capability
       register. When the controller does not support it, every DMA address
       must fit in 32 bits, including the data buffer. */
    int supports_64bit;
    int used;
} AHCI_DISK;

static AHCI_DISK disks[AHCI_MAX_DISKS];
static boot_uint32_t disk_count;

static int wait_until_clear(volatile boot_uint32_t* value, boot_uint32_t mask,
                            boot_uint64_t timeout_ms) {
    boot_uint64_t start = timer_ticks();
    while (*value & mask) {
        timer_poll();
        if (timer_ticks() - start >= timeout_ms) return 0;
    }
    return 1;
}

static void stop_command_engine(HBA_PORT* port) {
    port->cmd &= ~(AHCI_PXCMD_ST | AHCI_PXCMD_FRE);
    (void)wait_until_clear(&port->cmd, AHCI_PXCMD_CR | AHCI_PXCMD_FR, 500);
}

static void start_command_engine(HBA_PORT* port) {
    (void)wait_until_clear(&port->cmd, AHCI_PXCMD_CR, 500);
    port->cmd |= AHCI_PXCMD_FRE | AHCI_PXCMD_ST;
}

static int port_has_ata_disk(HBA_PORT* port) {
    boot_uint32_t ssts = port->ssts;
    return (ssts & 0x0FU) == 3U && ((ssts >> 8) & 0x0FU) == 1U &&
           port->sig == SATA_SIGNATURE_ATA;
}

static int ata_command(AHCI_DISK* disk, boot_uint8_t command, boot_uint64_t lba,
                       boot_uint16_t count, boot_uint32_t bytes,
                       const void* buffer, int write, int lba_addressing);

/* Issue IDENTIFY DEVICE and pull the addressable sector count out of it.
   Knowing the real size lets the block layer reject reads past the end of the
   disk instead of letting the controller return whatever it feels like. */
static void identify_device(AHCI_DISK* disk) {
    boot_uint16_t* data = (boot_uint16_t*)alloc_page_low();

    disk->sectors = 0;
    if (!data) return;
    memset(data, 0, PAGE_SIZE);
    if (ata_command(disk, ATA_CMD_IDENTIFY, 0, 0, 512, data, 0, 0)) {
        /* Words 100-103 hold the 48-bit LBA count. Words 60-61 are the older
           28-bit count, still the only valid one on small or ancient disks. */
        boot_uint64_t large = (boot_uint64_t)data[100] |
                              ((boot_uint64_t)data[101] << 16) |
                              ((boot_uint64_t)data[102] << 32) |
                              ((boot_uint64_t)data[103] << 48);
        boot_uint64_t small = (boot_uint64_t)data[60] |
                              ((boot_uint64_t)data[61] << 16);
        disk->sectors = large ? large : small;
    }
    free_page(data);
}

static int transfer(AHCI_DISK* disk, boot_uint64_t lba, boot_uint16_t count,
                    const void* buffer, int write);

static int ahci_block_read(BLOCK_DEVICE* device, boot_uint64_t lba,
                           boot_uint32_t count, void* buffer) {
    return transfer((AHCI_DISK*)device->driver_data, lba,
                    (boot_uint16_t)count, buffer, 0);
}

static int ahci_block_write(BLOCK_DEVICE* device, boot_uint64_t lba,
                            boot_uint32_t count, const void* buffer) {
    return transfer((AHCI_DISK*)device->driver_data, lba,
                    (boot_uint16_t)count, buffer, 1);
}

/* Named by the order they were found in, which is the order the block layer
   and the drive letters will see them. */
static int register_block_device(AHCI_DISK* disk, boot_uint32_t number) {
    BLOCK_DEVICE device;

    memset(&device, 0, sizeof(device));
    device.name[0] = 'a'; device.name[1] = 'h'; device.name[2] = 'c';
    device.name[3] = 'i';
    device.name[4] = (char)('0' + (number % 10));
    device.name[5] = 0;
    device.sector_size = 512;
    device.sector_count = disk->sectors;
    device.driver_data = disk;
    device.read = ahci_block_read;
    device.write = ahci_block_write;
    return block_register(&device) >= 0;
}

int ahci_init(const PCI_DEVICE* controller) {
    HBA_MEMORY* hba;
    boot_uint32_t implemented;
    int supports_64bit;
    int found = 0;

    pci_enable_bus_mastering(controller);
    if (controller->bar[5] & 1U) return 0;
    hba = (HBA_MEMORY*)(unsigned long long)(controller->bar[5] & ~0x0FU);
    ((volatile boot_uint32_t*)hba)[1] |= AHCI_GHC_AE;
    supports_64bit = (((volatile boot_uint32_t*)hba)[0] & AHCI_CAP_S64A) != 0;
    implemented = ((volatile boot_uint32_t*)hba)[3]; /* PI at offset 0x0C. */

    /* Every implemented port, not the first one with a disk on it. Stopping at
       the first is how a machine with a system disk and a data disk shows one
       of them, and which one depends on the order the board wired its
       sockets. */
    for (boot_uint8_t index = 0; index < 32; index++) {
        HBA_PORT* port;
        AHCI_DISK* disk;
        void* list_page;
        void* fis_page;
        void* table_page;

        if (!(implemented & (1U << index))) continue;
        port = &hba->ports[index];
        if (!port_has_ata_disk(port)) continue;
        if (disk_count >= AHCI_MAX_DISKS) break;

        stop_command_engine(port);
        /* Command list, FIS area and command table are addressed by paired
           32-bit registers. Taking them from low memory keeps the upper
           halves zero and correct on controllers without 64-bit support. */
        list_page = alloc_page_low();
        fis_page = alloc_page_low();
        table_page = alloc_page_low();
        /* One port that cannot be set up is not a reason to abandon the ones
           after it. */
        if (!list_page || !fis_page || !table_page) continue;
        memset(list_page, 0, PAGE_SIZE);
        memset(fis_page, 0, PAGE_SIZE);
        memset(table_page, 0, PAGE_SIZE);
        port->clb = (boot_uint32_t)(unsigned long long)list_page;
        port->clbu = 0;
        port->fb = (boot_uint32_t)(unsigned long long)fis_page;
        port->fbu = 0;

        disk = &disks[disk_count];
        memset(disk, 0, sizeof(*disk));
        disk->port = port;
        disk->command_list = (HBA_COMMAND_HEADER*)list_page;
        disk->command_table = (HBA_COMMAND_TABLE*)table_page;
        disk->supports_64bit = supports_64bit;

        start_command_engine(port);
        identify_device(disk);
        if (!register_block_device(disk, disk_count)) continue;
        disk->used = 1;
        disk_count++;
        found++;
    }
    return found;
}

/* Issue one ATA command through the port's single command slot.
 *
 * `lba_addressing` distinguishes the two shapes of command we send: the DMA
 * transfers address a sector and set the LBA bit in the device register, while
 * IDENTIFY addresses nothing and must leave that register clear. */
static int ata_command(AHCI_DISK* disk, boot_uint8_t command, boot_uint64_t lba,
                       boot_uint16_t count, boot_uint32_t bytes,
                       const void* buffer, int write, int lba_addressing) {
    boot_uint64_t data = (boot_uint64_t)(unsigned long long)buffer;
    boot_uint64_t table;
    HBA_PORT* sata_port;
    HBA_COMMAND_HEADER* header;
    boot_uint8_t* fis;
    boot_uint64_t start;

    if (!disk || !disk->port || !buffer || !bytes || bytes > 0x400000U) return 0;
    sata_port = disk->port;
    table = (boot_uint64_t)(unsigned long long)disk->command_table;
    /* The PRD base must be word aligned, and on a controller without 64-bit
       support it must also fit in 32 bits. Failing here is far better than
       letting the controller DMA to a truncated address. */
    if (data & 1U) return 0;
    if (!disk->supports_64bit && (data >> 32) != 0) return 0;
    if (!wait_until_clear(&sata_port->tfd, AHCI_PXTFD_BSY | AHCI_PXTFD_DRQ, 1000)) return 0;

    header = &disk->command_list[0];
    memset(header, 0, sizeof(*header));
    memset(disk->command_table, 0, PAGE_SIZE);
    /* Low five bits: command FIS length in dwords. Bit 6: write direction. */
    header->flags = (boot_uint16_t)(5U | (write ? 0x40U : 0));
    header->prdt_length = 1;
    header->ctba = (boot_uint32_t)table;
    header->ctbau = (boot_uint32_t)(table >> 32);
    disk->command_table->prdt[0].dba = (boot_uint32_t)data;
    disk->command_table->prdt[0].dbau = (boot_uint32_t)(data >> 32);
    disk->command_table->prdt[0].dbc = (bytes - 1U) | 0x80000000U;

    fis = disk->command_table->cfis;
    fis[0] = FIS_TYPE_REG_H2D;
    fis[1] = 0x80U;              /* bit 7: this FIS carries a command */
    fis[2] = command;
    fis[4] = (boot_uint8_t)lba;
    fis[5] = (boot_uint8_t)(lba >> 8);
    fis[6] = (boot_uint8_t)(lba >> 16);
    fis[7] = lba_addressing ? 0x40U : 0x00U;
    fis[8] = (boot_uint8_t)(lba >> 24);
    fis[9] = (boot_uint8_t)(lba >> 32);
    fis[10] = (boot_uint8_t)(lba >> 40);
    fis[12] = (boot_uint8_t)count;
    fis[13] = (boot_uint8_t)(count >> 8);

    sata_port->is = 0xFFFFFFFFU;
    sata_port->ci = 1U;
    start = timer_ticks();
    while (sata_port->ci & 1U) {
        timer_poll();
        if (timer_ticks() - start >= 5000U) return 0;
    }
    return (sata_port->is & AHCI_PXIS_TFES) == 0;
}

static int transfer(AHCI_DISK* disk, boot_uint64_t lba, boot_uint16_t count,
                    const void* buffer, int write) {
    if (!count) return 0;
    return ata_command(disk,
                       write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                       lba, count, (boot_uint32_t)count * 512U, buffer, write, 1);
}

boot_uint32_t ahci_disk_count(void) {
    return disk_count;
}

boot_uint64_t ahci_sector_count(boot_uint32_t index) {
    return index < disk_count ? disks[index].sectors : 0;
}
