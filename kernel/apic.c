#include "apic.h"
#include "acpi.h"
#include "hpet.h"
#include "timer.h"
#include "paging.h"
#include "serial.h"

/* Local APIC registers, at the base ACPI gives us. All are 32 bits wide and
   must be accessed as such. */
#define LAPIC_ID 0x020
#define LAPIC_VERSION 0x030
#define LAPIC_TASK_PRIORITY 0x080
#define LAPIC_EOI 0x0B0
#define LAPIC_SPURIOUS 0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE 0x3E0

#define SPURIOUS_ENABLE 0x100        /* software enable, bit 8 */
#define SPURIOUS_VECTOR 0xFF

#define LVT_MASKED 0x10000           /* bit 16 */
#define LVT_PERIODIC 0x20000         /* bits 17-18: 1 means periodic */

/* Divide configuration is encoded across bits 0, 1 and 3, which is why the
   value for "divide by 16" is 3 and the value for "by 1" is 11. */
#define DIVIDE_BY_16 0x3
#define DIVIDE_SHIFT 4               /* what DIVIDE_BY_16 actually divides by */

/* The model-specific register that holds the Local APIC's base address and
   its global enable bit. */
#define MSR_APIC_BASE 0x1B
#define APIC_BASE_GLOBAL_ENABLE 0x800

/* I/O APIC: two registers, one selecting and one carrying the value. */
#define IOAPIC_SELECT 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_REG_VERSION 0x01
#define IOAPIC_REG_REDIRECTION 0x10

#define REDIRECTION_MASKED 0x10000       /* bit 16 */
#define REDIRECTION_LEVEL 0x8000         /* bit 15 */
#define REDIRECTION_ACTIVE_LOW 0x2000    /* bit 13 */

#define APIC_WINDOW_SIZE 0x1000

/* How long to measure the timer over. Long enough that the count is large
   compared with the cost of reading the clocks, short enough not to be felt
   during boot. */
#define CALIBRATION_MS 50

static volatile boot_uint8_t* lapic;
static volatile boot_uint8_t* ioapic;
static boot_uint32_t ioapic_base_interrupt;
static boot_uint32_t timer_frequency;

static boot_uint32_t lapic_read(boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(lapic + offset);
}

static void lapic_write(boot_uint32_t offset, boot_uint32_t value) {
    *(volatile boot_uint32_t*)(lapic + offset) = value;
}

static boot_uint64_t read_msr(boot_uint32_t index) {
    boot_uint32_t low;
    boot_uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return (boot_uint64_t)low | ((boot_uint64_t)high << 32);
}

static void write_msr(boot_uint32_t index, boot_uint64_t value) {
    __asm__ volatile ("wrmsr"
                      : : "c"(index), "a"((boot_uint32_t)value),
                          "d"((boot_uint32_t)(value >> 32)));
}

static boot_uint32_t ioapic_read(boot_uint32_t index) {
    *(volatile boot_uint32_t*)(ioapic + IOAPIC_SELECT) = index;
    return *(volatile boot_uint32_t*)(ioapic + IOAPIC_WINDOW);
}

static void ioapic_write(boot_uint32_t index, boot_uint32_t value) {
    *(volatile boot_uint32_t*)(ioapic + IOAPIC_SELECT) = index;
    *(volatile boot_uint32_t*)(ioapic + IOAPIC_WINDOW) = value;
}

/* Count how far the timer gets in a known interval.
 *
 * The HPET is the reference when there is one, and the PIT when there is not.
 * The PIT is a worse reference - it is what the whole exercise is trying to
 * stop relying on - but it is a great deal better than guessing, and
 * `timer_wait` polls it continuously so it does not lose time the way an
 * unattended PIT does. */
static boot_uint32_t calibrate(void) {
    boot_uint32_t remaining;
    boot_uint64_t counted;

    lapic_write(LAPIC_TIMER_DIVIDE, DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFFU);

    if (hpet_available()) hpet_delay_us(CALIBRATION_MS * 1000U);
    else timer_wait(CALIBRATION_MS);

    remaining = lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0);        /* stop it again */

    counted = (boot_uint64_t)(0xFFFFFFFFU - remaining);
    /* A timer that did not move, or one that ran out entirely, has not been
       measured - it has only produced a number. */
    if (!counted || !remaining) return 0;
    return (boot_uint32_t)((counted * 1000ULL) / CALIBRATION_MS);
}

int apic_init(void) {
    boot_uint64_t base = acpi_local_apic_address();
    boot_uint64_t io_base = acpi_io_apic_address();

    lapic = 0;
    ioapic = 0;
    timer_frequency = 0;

    if (!base) {
        serial_write("APIC: no local APIC address\n");
        return 0;
    }
    if (!paging_map_device(base, APIC_WINDOW_SIZE)) {
        serial_write("APIC: could not map the local APIC\n");
        return 0;
    }
    lapic = (volatile boot_uint8_t*)(unsigned long long)base;

    /* Two separate enables, and both are needed: the hardware one in the MSR,
       which firmware normally leaves set, and the software one in the spurious
       vector register, which it normally does not. */
    write_msr(MSR_APIC_BASE, read_msr(MSR_APIC_BASE) | APIC_BASE_GLOBAL_ENABLE);
    lapic_write(LAPIC_SPURIOUS, SPURIOUS_ENABLE | SPURIOUS_VECTOR);
    /* Accept every priority. Left as the firmware had it, a raised task
       priority silently blocks interrupts that are otherwise set up correctly. */
    lapic_write(LAPIC_TASK_PRIORITY, 0);

    timer_frequency = calibrate();
    if (!timer_frequency) {
        serial_write("APIC: the timer could not be calibrated\n");
        lapic = 0;
        return 0;
    }

    serial_write("APIC: local APIC ");
    serial_write_dec(lapic_read(LAPIC_ID) >> 24);
    serial_write(", timer ");
    serial_write_dec(timer_frequency / 1000U);
    serial_write(" kHz measured against ");
    serial_write(hpet_available() ? "the HPET\n" : "the PIT\n");

    if (io_base && paging_map_device(io_base, APIC_WINDOW_SIZE)) {
        ioapic = (volatile boot_uint8_t*)(unsigned long long)io_base;
        ioapic_base_interrupt = acpi_io_apic_base();
        serial_write("APIC: I/O APIC with ");
        serial_write_dec(((ioapic_read(IOAPIC_REG_VERSION) >> 16) & 0xFF) + 1);
        serial_write(" inputs from interrupt ");
        serial_write_dec(ioapic_base_interrupt);
        serial_write("\n");
    } else {
        serial_write("APIC: no I/O APIC - IRQs stay on the 8259\n");
    }
    return 1;
}

int apic_available(void) {
    return lapic != 0;
}

boot_uint32_t apic_timer_frequency(void) {
    return timer_frequency;
}

int apic_start_timer(boot_uint32_t hz, boot_uint8_t vector) {
    boot_uint32_t count;

    if (!lapic || !timer_frequency || !hz) return 0;
    count = timer_frequency / hz;
    if (!count) {
        serial_write("APIC: the timer cannot run that fast\n");
        return 0;
    }

    lapic_write(LAPIC_TIMER_DIVIDE, DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | vector);
    lapic_write(LAPIC_TIMER_INITIAL, count);
    return 1;
}

void apic_end_of_interrupt(void) {
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

int apic_route_irq(boot_uint8_t irq, boot_uint8_t vector) {
    int active_low = 0;
    int level_triggered = 0;
    boot_uint32_t global;
    boot_uint32_t entry;
    boot_uint32_t low;

    if (!ioapic) return 0;

    /* Where this IRQ actually arrives. Firmware routinely moves the legacy
       lines, and programming the input that shares the IRQ's number would
       enable an input nothing is wired to. */
    global = acpi_interrupt_override(irq, &active_low, &level_triggered);
    if (global < ioapic_base_interrupt) return 0;
    entry = global - ioapic_base_interrupt;
    if (entry > ((ioapic_read(IOAPIC_REG_VERSION) >> 16) & 0xFF)) return 0;

    low = vector;                       /* fixed delivery, physical, to APIC 0 */
    if (active_low) low |= REDIRECTION_ACTIVE_LOW;
    if (level_triggered) low |= REDIRECTION_LEVEL;

    /* The destination first, while the entry is still masked, then the half
       that unmasks it - so the route is never briefly pointing nowhere. */
    ioapic_write(IOAPIC_REG_REDIRECTION + entry * 2 + 1,
                 (lapic_read(LAPIC_ID) >> 24) << 24);
    ioapic_write(IOAPIC_REG_REDIRECTION + entry * 2, low | REDIRECTION_MASKED);
    ioapic_write(IOAPIC_REG_REDIRECTION + entry * 2, low);

    serial_write("APIC: IRQ ");
    serial_write_dec(irq);
    serial_write(" routed from interrupt ");
    serial_write_dec(global);
    serial_write(" to vector ");
    serial_write_dec(vector);
    serial_write("\n");
    return 1;
}
