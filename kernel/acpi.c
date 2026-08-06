#include "acpi.h"
#include "string.h"

typedef struct __attribute__((packed)) {
    char signature[8];
    boot_uint8_t checksum;
    char oem_id[6];
    boot_uint8_t revision;
    boot_uint32_t rsdt_address;
    /* Revision 2 and later continue here. */
    boot_uint32_t length;
    boot_uint64_t xsdt_address;
    boot_uint8_t extended_checksum;
    boot_uint8_t reserved[3];
} ACPI_RSDP;

typedef struct __attribute__((packed)) {
    char signature[4];
    boot_uint32_t length;
    boot_uint8_t revision;
    boot_uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    boot_uint32_t oem_revision;
    boot_uint32_t creator_id;
    boot_uint32_t creator_revision;
} ACPI_TABLE_HEADER;

/* Offset of IAPC_BOOT_ARCH within the FADT, and the bit that says an 8042 is
   present. The field only exists from FADT revision 3 onwards. */
#define FADT_BOOT_ARCH_OFFSET 109
#define FADT_MINIMUM_LENGTH 116
#define IAPC_BOOT_ARCH_8042 0x0002

static const ACPI_TABLE_HEADER* root_table;
static int root_is_xsdt;
static boot_uint32_t root_entries;

static int checksum_ok(const void* table, boot_uint32_t length) {
    const boot_uint8_t* bytes = (const boot_uint8_t*)table;
    boot_uint8_t sum = 0;
    for (boot_uint32_t i = 0; i < length; i++) sum = (boot_uint8_t)(sum + bytes[i]);
    return sum == 0;
}

void acpi_init(boot_uint64_t rsdp_address) {
    const ACPI_RSDP* rsdp = (const ACPI_RSDP*)(unsigned long long)rsdp_address;
    const ACPI_TABLE_HEADER* root;

    root_table = (const ACPI_TABLE_HEADER*)0;
    if (!rsdp_address) return;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) return;
    /* The first 20 bytes always checksum on their own, in every revision. */
    if (!checksum_ok(rsdp, 20)) return;

    /* Prefer the XSDT: its entries are 64-bit, so it can describe tables the
       32-bit RSDT cannot reach. */
    if (rsdp->revision >= 2 && checksum_ok(rsdp, rsdp->length) &&
        rsdp->xsdt_address) {
        root = (const ACPI_TABLE_HEADER*)(unsigned long long)rsdp->xsdt_address;
        root_is_xsdt = 1;
    } else {
        root = (const ACPI_TABLE_HEADER*)(unsigned long long)rsdp->rsdt_address;
        root_is_xsdt = 0;
    }
    if (!root || !checksum_ok(root, root->length)) return;

    root_table = root;
    root_entries = (root->length - (boot_uint32_t)sizeof(ACPI_TABLE_HEADER)) /
                   (root_is_xsdt ? 8U : 4U);
}

const void* acpi_find_table(const char signature[4]) {
    const boot_uint8_t* entries;

    if (!root_table) return (const void*)0;
    entries = (const boot_uint8_t*)root_table + sizeof(ACPI_TABLE_HEADER);

    for (boot_uint32_t index = 0; index < root_entries; index++) {
        boot_uint64_t address;
        const ACPI_TABLE_HEADER* table;
        if (root_is_xsdt) {
            /* The XSDT is only 4-byte aligned, so a 64-bit load off it can be
               misaligned. Assemble it from bytes instead. */
            const boot_uint8_t* slot = entries + index * 8;
            address = 0;
            for (int byte = 7; byte >= 0; byte--)
                address = (address << 8) | slot[byte];
        } else {
            const boot_uint8_t* slot = entries + index * 4;
            address = (boot_uint64_t)slot[0] | ((boot_uint64_t)slot[1] << 8) |
                      ((boot_uint64_t)slot[2] << 16) | ((boot_uint64_t)slot[3] << 24);
        }
        if (!address) continue;
        table = (const ACPI_TABLE_HEADER*)(unsigned long long)address;
        if (memcmp(table->signature, signature, 4) == 0) return table;
    }
    return (const void*)0;
}

int acpi_has_8042(void) {
    const ACPI_TABLE_HEADER* fadt = (const ACPI_TABLE_HEADER*)acpi_find_table("FACP");
    boot_uint16_t boot_architecture;

    /* No ACPI, no FADT, or a FADT too old to carry the field: assume the
       controller is there. Machines that predate the flag all have one. */
    if (!fadt || fadt->revision < 3 || fadt->length < FADT_MINIMUM_LENGTH) return 1;

    boot_architecture = (boot_uint16_t)(
        ((const boot_uint8_t*)fadt)[FADT_BOOT_ARCH_OFFSET] |
        ((boot_uint16_t)((const boot_uint8_t*)fadt)[FADT_BOOT_ARCH_OFFSET + 1] << 8));
    return (boot_architecture & IAPC_BOOT_ARCH_8042) != 0;
}
