#include "hpet.h"
#include "acpi.h"
#include "paging.h"
#include "serial.h"

/* Registers, at the base ACPI gives us. All of them are 64 bits wide and must
   be addressed as such or in aligned halves. */
#define HPET_CAPABILITIES 0x000
#define HPET_CONFIGURATION 0x010
#define HPET_MAIN_COUNTER 0x0F0

#define CONFIGURATION_ENABLE 0x1

/* One page is more than the whole register block, which is 1 KiB. */
#define HPET_WINDOW_SIZE 0x1000

/* A femtosecond is 10^-15 seconds; the capability register counts the tick
   period in them, which is how a 14 MHz clock is described exactly rather than
   as a rounded frequency. */
#define FEMTOSECONDS_PER_SECOND 1000000000000000ULL

static volatile boot_uint8_t* registers;
static boot_uint32_t frequency;

static boot_uint32_t read32(boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(registers + offset);
}

static void write32(boot_uint32_t offset, boot_uint32_t value) {
    *(volatile boot_uint32_t*)(registers + offset) = value;
}

int hpet_init(void) {
    boot_uint64_t base = acpi_hpet_address();
    boot_uint32_t period;

    registers = 0;
    frequency = 0;

    if (!base) {
        serial_write("HPET: not described by ACPI\n");
        return 0;
    }
    if (!paging_map_device(base, HPET_WINDOW_SIZE)) {
        serial_write("HPET: could not map its registers\n");
        return 0;
    }
    registers = (volatile boot_uint8_t*)(unsigned long long)base;

    /* The period lives in the upper half of the capability register. */
    period = read32(HPET_CAPABILITIES + 4);
    /* A period of zero, or one longer than 100 nanoseconds, is outside what
       the specification permits and means we are reading the wrong thing. */
    if (!period || period > 100000000U) {
        serial_write("HPET: implausible tick period, ignoring it\n");
        registers = 0;
        return 0;
    }
    frequency = (boot_uint32_t)(FEMTOSECONDS_PER_SECOND / period);

    /* Start the counter. Firmware often leaves it stopped. */
    write32(HPET_CONFIGURATION, read32(HPET_CONFIGURATION) | CONFIGURATION_ENABLE);

    /* And prove it is moving before promising anyone it works: a counter that
       reads the same twice is worse than no counter, because every delay
       written against it would hang. */
    {
        boot_uint32_t first = read32(HPET_MAIN_COUNTER);
        boot_uint32_t second;
        int moved = 0;

        for (int spin = 0; spin < 1000000; spin++) {
            second = read32(HPET_MAIN_COUNTER);
            if (second != first) { moved = 1; break; }
        }
        if (!moved) {
            serial_write("HPET: counter is not advancing, ignoring it\n");
            registers = 0;
            frequency = 0;
            return 0;
        }
    }

    serial_write("HPET: ");
    serial_write_dec(frequency / 1000U);
    serial_write(" kHz at ");
    serial_write_hex(base);
    serial_write("\n");
    return 1;
}

int hpet_available(void) {
    return registers != 0;
}

boot_uint32_t hpet_frequency(void) {
    return frequency;
}

boot_uint32_t hpet_counter(void) {
    return registers ? read32(HPET_MAIN_COUNTER) : 0;
}

void hpet_delay_us(boot_uint32_t microseconds) {
    boot_uint32_t start;
    boot_uint32_t needed;

    if (!registers || !microseconds) return;
    /* Computed in 64 bits and stored in 32: a second at any legal HPET
       frequency is comfortably inside 32 bits, and longer waits belong to the
       millisecond timer rather than here. */
    if (microseconds > 1000000U) microseconds = 1000000U;
    needed = (boot_uint32_t)(((boot_uint64_t)microseconds *
                              (boot_uint64_t)frequency) / 1000000ULL);

    start = read32(HPET_MAIN_COUNTER);
    while ((boot_uint32_t)(read32(HPET_MAIN_COUNTER) - start) < needed) { }
}
