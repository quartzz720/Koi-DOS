#include "timer.h"
#include "keyboard.h"
#include "io.h"

#define PIT_FREQUENCY 1193182U
#define PIT_COMMAND_PORT 0x43U
#define PIT_CHANNEL0_PORT 0x40U
#define PIT_CHANNEL0_RATE_GENERATOR 0x34U

static boot_uint16_t pit_divisor;
static boot_uint16_t last_counter;
static boot_uint32_t subticks;
static volatile boot_uint64_t elapsed_ticks;
static int interrupt_driven;

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

void timer_use_interrupt(void) {
    interrupt_driven = 1;
}

int timer_is_interrupt_driven(void) {
    return interrupt_driven;
}

void timer_tick(void) {
    elapsed_ticks++;

    /* And a look at the keyboard controller, every tick.
     *
     * The 8042 hands over one byte and then waits to be asked again: until
     * that byte is read it delivers nothing and raises no interrupt. So one
     * interrupt that never arrives - or arrives while the buffer already
     * holds the other device's byte - stops both the keyboard and the
     * pointer, permanently, and nothing else in the machine will notice.
     *
     * A hundred times a second, whatever is stuck is taken and given to
     * whoever it belongs to. This is not how input is meant to arrive and it
     * is not what makes it fast; it is what makes a lost interrupt cost ten
     * milliseconds instead of the machine. It runs during the long parts of
     * startup as well, which is where the trouble actually starts: nothing
     * polls the keyboard while disks and USB are being enumerated, and a
     * touchpad that the firmware left streaming is filling the buffer the
     * whole time. */
    ps2_drain();
}

void timer_poll(void) {
    boot_uint16_t current;

    /* Once a timer interrupt is keeping the count, polling is not merely
       unnecessary - reading the PIT here would add ticks the interrupt has
       already counted. */
    if (interrupt_driven) return;

    current = pit_counter();
    /* Only movement within one reload period is visible here: the counter
       wraps every millisecond, so a gap longer than that is lost rather than
       merely imprecise. That is the defect the timer interrupt removes. */
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

int timer_expired(boot_uint64_t start, boot_uint64_t milliseconds) {
    /* Polling here as well as returning the answer: a caller's loop is
       typically `while (!ready && !timer_expired(...))`, and on a polled
       source nothing else would advance the clock. */
    timer_poll();
    return timer_ticks() - start >= milliseconds;
}

void timer_wait(boot_uint64_t milliseconds) {
    boot_uint64_t start = timer_ticks();
    while (!timer_expired(start, milliseconds)) { }
}
