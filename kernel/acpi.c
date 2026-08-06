#include "acpi.h"
#include "string.h"
#include "io.h"

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

/* ---- The MADT: where the interrupt hardware lives ----------------------- */

#define MADT_LOCAL_APIC_ADDRESS_OFFSET 36
#define MADT_ENTRIES_OFFSET 44

#define MADT_TYPE_IO_APIC 1
#define MADT_TYPE_INTERRUPT_OVERRIDE 2
#define MADT_TYPE_LOCAL_APIC_OVERRIDE 5

/* Read little-endian fields out of a packed table without assuming the table
   itself is aligned - ACPI makes no such promise. */
static boot_uint32_t load32(const boot_uint8_t* data) {
    return (boot_uint32_t)data[0] | ((boot_uint32_t)data[1] << 8) |
           ((boot_uint32_t)data[2] << 16) | ((boot_uint32_t)data[3] << 24);
}

static boot_uint64_t load64(const boot_uint8_t* data) {
    return (boot_uint64_t)load32(data) | ((boot_uint64_t)load32(data + 4) << 32);
}

/* Walk the MADT's variable-length entry list, calling nothing and returning
   the first entry of the wanted type - or, for overrides, the one matching a
   source IRQ. `match` is ignored for types that have no source. */
static const boot_uint8_t* madt_entry(boot_uint8_t type, int match) {
    const ACPI_TABLE_HEADER* madt =
        (const ACPI_TABLE_HEADER*)acpi_find_table("APIC");
    const boot_uint8_t* bytes;
    boot_uint32_t offset;

    if (!madt || madt->length <= MADT_ENTRIES_OFFSET) return (const boot_uint8_t*)0;
    bytes = (const boot_uint8_t*)madt;

    for (offset = MADT_ENTRIES_OFFSET; offset + 2 <= madt->length; ) {
        boot_uint8_t entry_type = bytes[offset];
        boot_uint8_t entry_length = bytes[offset + 1];

        /* A zero length would spin here forever on a malformed table. */
        if (entry_length < 2 || offset + entry_length > madt->length) break;
        if (entry_type == type &&
            (match < 0 || (entry_length >= 4 && bytes[offset + 3] == match)))
            return bytes + offset;
        offset += entry_length;
    }
    return (const boot_uint8_t*)0;
}

int acpi_madt_present(void) {
    return acpi_find_table("APIC") != (const void*)0;
}

boot_uint64_t acpi_local_apic_address(void) {
    const ACPI_TABLE_HEADER* madt =
        (const ACPI_TABLE_HEADER*)acpi_find_table("APIC");
    const boot_uint8_t* override;

    if (!madt || madt->length < MADT_ENTRIES_OFFSET) return 0;

    /* A 64-bit override entry wins over the 32-bit field in the header, which
       is the whole reason the override exists. */
    override = madt_entry(MADT_TYPE_LOCAL_APIC_OVERRIDE, -1);
    if (override) return load64(override + 4);

    return load32((const boot_uint8_t*)madt + MADT_LOCAL_APIC_ADDRESS_OFFSET);
}

boot_uint64_t acpi_io_apic_address(void) {
    const boot_uint8_t* entry = madt_entry(MADT_TYPE_IO_APIC, -1);
    return entry ? load32(entry + 4) : 0;
}

boot_uint32_t acpi_io_apic_base(void) {
    const boot_uint8_t* entry = madt_entry(MADT_TYPE_IO_APIC, -1);
    return entry ? load32(entry + 8) : 0;
}

boot_uint32_t acpi_interrupt_override(boot_uint8_t irq, int* active_low,
                                      int* level_triggered) {
    const boot_uint8_t* entry = madt_entry(MADT_TYPE_INTERRUPT_OVERRIDE, irq);
    boot_uint16_t flags;

    /* No override means the ISA default: the IRQ arrives on the global
       interrupt of the same number, active high and edge triggered. */
    if (active_low) *active_low = 0;
    if (level_triggered) *level_triggered = 0;
    if (!entry) return irq;

    flags = (boot_uint16_t)(entry[8] | ((boot_uint16_t)entry[9] << 8));
    /* Polarity in bits 0-1, trigger mode in bits 2-3; zero in either means
       "whatever the bus normally does", which for ISA is high and edge. */
    if (active_low) *active_low = (flags & 0x3) == 0x3;
    if (level_triggered) *level_triggered = ((flags >> 2) & 0x3) == 0x3;
    return load32(entry + 4);
}

/* ---- The HPET ------------------------------------------------------------ */

#define HPET_ADDRESS_OFFSET 44

boot_uint64_t acpi_hpet_address(void) {
    const ACPI_TABLE_HEADER* hpet =
        (const ACPI_TABLE_HEADER*)acpi_find_table("HPET");

    if (!hpet || hpet->length < HPET_ADDRESS_OFFSET + 8) return 0;
    /* The address sits inside a generic address structure. Only the memory
       space case is worth supporting; an HPET in I/O space does not exist. */
    if (((const boot_uint8_t*)hpet)[40] != 0) return 0;
    return load64((const boot_uint8_t*)hpet + HPET_ADDRESS_OFFSET);
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

/* ---- Turning the machine off -------------------------------------------- */

#define FADT_SMI_COMMAND 48
#define FADT_ACPI_ENABLE 52
#define FADT_PM1A_CONTROL 64
#define FADT_PM1B_CONTROL 68
#define FADT_DSDT 40
#define FADT_FLAGS 112
#define FADT_RESET_REGISTER 116      /* a generic address structure */
#define FADT_RESET_VALUE 128
#define FADT_X_DSDT 140
#define FADT_X_PM1A_CONTROL 172
#define FADT_X_PM1B_CONTROL 184

#define FADT_FLAG_RESET_SUPPORTED 0x400

#define SLEEP_ENABLE 0x2000          /* bit 13 of the PM1 control register */
#define SCI_ENABLED 0x1              /* bit 0: ACPI mode rather than legacy */

#define ADDRESS_SPACE_MEMORY 0
#define ADDRESS_SPACE_IO 1

/* An I/O port out of either the old 32-bit field or the newer generic address
   structure beside it. The extended one wins when it is filled in and names
   I/O space, which is what every x86 machine uses for these. */
static boot_uint32_t fadt_port(const boot_uint8_t* fadt, boot_uint32_t length,
                               boot_uint32_t legacy_offset,
                               boot_uint32_t extended_offset) {
    if (length >= extended_offset + 12) {
        const boot_uint8_t* gas = fadt + extended_offset;
        boot_uint64_t address = load64(gas + 4);
        if (address && gas[0] == ADDRESS_SPACE_IO)
            return (boot_uint32_t)address;
    }
    if (length >= legacy_offset + 4) return load32(fadt + legacy_offset);
    return 0;
}

/* The sleep type values for soft-off, dug out of the DSDT's bytecode.
 *
 * `_S5_` is followed by a package: the package opcode, a length whose top two
 * bits say how many bytes the length itself takes, an element count, and then
 * the values - each either a bare zero/one opcode or a byte prefix and a byte.
 * Two numbers is all we need, and this is the shape they come in. */
static int find_sleep_values(boot_uint32_t* type_a, boot_uint32_t* type_b) {
    const ACPI_TABLE_HEADER* fadt =
        (const ACPI_TABLE_HEADER*)acpi_find_table("FACP");
    const boot_uint8_t* dsdt;
    const boot_uint8_t* cursor;
    boot_uint32_t dsdt_length;
    boot_uint64_t address = 0;

    if (!fadt) return 0;
    if (fadt->length >= FADT_X_DSDT + 8)
        address = load64((const boot_uint8_t*)fadt + FADT_X_DSDT);
    if (!address && fadt->length >= FADT_DSDT + 4)
        address = load32((const boot_uint8_t*)fadt + FADT_DSDT);
    if (!address) return 0;

    dsdt = (const boot_uint8_t*)(unsigned long long)address;
    if (memcmp(dsdt, "DSDT", 4) != 0) return 0;
    dsdt_length = load32(dsdt + 4);
    if (dsdt_length < 36 || dsdt_length > 0x400000U) return 0;

    for (boot_uint32_t offset = 36; offset + 8 < dsdt_length; offset++) {
        if (memcmp(dsdt + offset, "_S5_", 4) != 0) continue;

        cursor = dsdt + offset + 4;
        if (*cursor != 0x12) continue;            /* not a package after all */
        cursor++;
        /* The package length's own size lives in its top two bits; skip it and
           the element count that follows. */
        cursor += ((*cursor & 0xC0) >> 6) + 2;

        if (*cursor == 0x0A) cursor++;            /* a byte follows */
        *type_a = (boot_uint32_t)*cursor << 10;
        cursor++;
        if (*cursor == 0x0A) cursor++;
        *type_b = (boot_uint32_t)*cursor << 10;
        return 1;
    }
    return 0;
}

int acpi_power_off(void) {
    const ACPI_TABLE_HEADER* fadt =
        (const ACPI_TABLE_HEADER*)acpi_find_table("FACP");
    boot_uint32_t pm1a;
    boot_uint32_t pm1b;
    boot_uint32_t type_a = 0;
    boot_uint32_t type_b = 0;

    if (!fadt) return 0;
    if (!find_sleep_values(&type_a, &type_b)) return 0;

    pm1a = fadt_port((const boot_uint8_t*)fadt, fadt->length,
                     FADT_PM1A_CONTROL, FADT_X_PM1A_CONTROL);
    pm1b = fadt_port((const boot_uint8_t*)fadt, fadt->length,
                     FADT_PM1B_CONTROL, FADT_X_PM1B_CONTROL);
    if (!pm1a) return 0;

    /* Ask the firmware to hand ACPI over, if it has not already. UEFI systems
       arrive with it enabled and the command port set to zero; older ones do
       not, and writing to the control register before the switch does
       nothing at all. */
    if (!(inw((boot_uint16_t)pm1a) & SCI_ENABLED)) {
        boot_uint32_t command = fadt->length >= FADT_SMI_COMMAND + 4
            ? load32((const boot_uint8_t*)fadt + FADT_SMI_COMMAND) : 0;
        boot_uint8_t enable = fadt->length >= FADT_ACPI_ENABLE + 1
            ? ((const boot_uint8_t*)fadt)[FADT_ACPI_ENABLE] : 0;

        if (!command || !enable) return 0;
        outb((boot_uint16_t)command, enable);
        for (int spin = 0; spin < 300000; spin++)
            if (inw((boot_uint16_t)pm1a) & SCI_ENABLED) break;
        if (!(inw((boot_uint16_t)pm1a) & SCI_ENABLED)) return 0;
    }

    outw((boot_uint16_t)pm1a, (boot_uint16_t)(type_a | SLEEP_ENABLE));
    if (pm1b) outw((boot_uint16_t)pm1b, (boot_uint16_t)(type_b | SLEEP_ENABLE));

    /* The machine should be gone by now. Reaching here means it refused. */
    for (int spin = 0; spin < 1000000; spin++) __asm__ volatile ("pause");
    return 0;
}

int acpi_reset(void) {
    const ACPI_TABLE_HEADER* fadt =
        (const ACPI_TABLE_HEADER*)acpi_find_table("FACP");
    const boot_uint8_t* gas;
    boot_uint64_t address;
    boot_uint8_t value;

    if (!fadt || fadt->length < FADT_RESET_VALUE + 1) return 0;
    if (!(load32((const boot_uint8_t*)fadt + FADT_FLAGS) &
          FADT_FLAG_RESET_SUPPORTED)) return 0;

    gas = (const boot_uint8_t*)fadt + FADT_RESET_REGISTER;
    address = load64(gas + 4);
    value = ((const boot_uint8_t*)fadt)[FADT_RESET_VALUE];
    if (!address) return 0;

    if (gas[0] == ADDRESS_SPACE_IO) {
        outb((boot_uint16_t)address, value);
    } else if (gas[0] == ADDRESS_SPACE_MEMORY) {
        *(volatile boot_uint8_t*)(unsigned long long)address = value;
    } else {
        return 0;
    }

    for (int spin = 0; spin < 1000000; spin++) __asm__ volatile ("pause");
    return 0;
}
