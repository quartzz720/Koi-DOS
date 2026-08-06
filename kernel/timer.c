#include "timer.h"
#include "io.h"

#define PIT_FREQUENCY 1193182U
#define PIT_COMMAND_PORT 0x43U
#define PIT_CHANNEL0_PORT 0x40U
#define PIT_CHANNEL0_RATE_GENERATOR 0x34U

static boot_uint16_t pit_divisor;
static boot_uint16_t last_counter;
static boot_uint32_t subticks;
static boot_uint64_t elapsed_ticks;

static boot_uint16_t pit_counter(void) {
    boot_uint8_t low;
    boot_uint8_t high;
    /* Latch channel 0, then read its current down-counter value. */
    outb(PIT_COMMAND_PORT, 0x00);
    low = inb(PIT_CHANNEL0_PORT);
    high = inb(PIT_CHANNEL0_PORT);
    return (boot_uint16_t)(((boot_uint16_t)high << 8) | low);
}

void timer_init(void) {
    pit_divisor = (boot_uint16_t)(PIT_FREQUENCY / TIMER_HZ);
    if (!pit_divisor) pit_divisor = 1;
    outb(PIT_COMMAND_PORT, PIT_CHANNEL0_RATE_GENERATOR);
    outb(PIT_CHANNEL0_PORT, (boot_uint8_t)(pit_divisor & 0xFF));
    outb(PIT_CHANNEL0_PORT, (boot_uint8_t)(pit_divisor >> 8));
    last_counter = pit_counter();
    subticks = 0;
    elapsed_ticks = 0;
}

void timer_poll(void) {
    boot_uint16_t current = pit_counter();
    boot_uint16_t elapsed = (current <= last_counter)
        ? (boot_uint16_t)(last_counter - current)
        : (boot_uint16_t)(last_counter + pit_divisor - current);

    last_counter = current;
    subticks += elapsed;
    while (subticks >= pit_divisor) {
        subticks -= pit_divisor;
        elapsed_ticks++;
    }
}

boot_uint64_t timer_ticks(void) {
    return elapsed_ticks;
}
