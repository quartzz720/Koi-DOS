#include "serial.h"
#include "io.h"

#define COM1_BASE 0x3F8U

/* 16550 UART register offsets from the port base. */
#define UART_DATA 0U            /* RBR/THR when DLAB=0, divisor low when DLAB=1 */
#define UART_INTERRUPT_ENABLE 1U /* IER, divisor high when DLAB=1 */
#define UART_FIFO_CONTROL 2U    /* FCR (write) */
#define UART_LINE_CONTROL 3U    /* LCR */
#define UART_MODEM_CONTROL 4U   /* MCR */
#define UART_LINE_STATUS 5U     /* LSR */

#define LCR_DLAB 0x80U          /* divisor latch access */
#define LCR_8N1 0x03U           /* 8 data bits, no parity, 1 stop bit */
#define LSR_TRANSMIT_EMPTY 0x20U

/* 115200 baud: the UART clock is 115200 Hz, so the divisor is 1. */
#define BAUD_DIVISOR 1U

static int serial_present;

void serial_init(void) {
    outb(COM1_BASE + UART_INTERRUPT_ENABLE, 0x00);      /* no interrupts, we poll */
    outb(COM1_BASE + UART_LINE_CONTROL, LCR_DLAB);
    outb(COM1_BASE + UART_DATA, (boot_uint8_t)BAUD_DIVISOR);
    outb(COM1_BASE + UART_INTERRUPT_ENABLE, (boot_uint8_t)(BAUD_DIVISOR >> 8));
    outb(COM1_BASE + UART_LINE_CONTROL, LCR_8N1);       /* clears DLAB */
    outb(COM1_BASE + UART_FIFO_CONTROL, 0xC7);          /* enable + clear FIFOs */
    outb(COM1_BASE + UART_MODEM_CONTROL, 0x0B);         /* DTR, RTS, OUT2 */

    /* Loopback test: without it, a machine with no COM1 would make every
       serial_putchar() spin forever waiting for a transmit-empty bit that
       never arrives. Modern laptops have no physical port. */
    outb(COM1_BASE + UART_MODEM_CONTROL, 0x1E);         /* loopback mode */
    outb(COM1_BASE + UART_DATA, 0xAE);
    serial_present = (inb(COM1_BASE + UART_DATA) == 0xAE);
    outb(COM1_BASE + UART_MODEM_CONTROL, 0x0B);         /* back to normal */
}

/* Kept whether or not anything is listening on the port itself - which is the
   entire point, since the machines where the drivers behave unexpectedly are
   the ones with no serial port. One bounds check per character. */
static char captured[BOOT_LOG_SIZE];
static boot_uint32_t captured_length;
static int captured_truncated;

static void capture(char character) {
    if (captured_length >= BOOT_LOG_SIZE - 1) {
        captured_truncated = 1;
        return;
    }
    captured[captured_length++] = character;
}

const char* boot_log(void) {
    captured[captured_length] = 0;
    return captured;
}

boot_uint32_t boot_log_length(void) { return captured_length; }
int boot_log_truncated(void) { return captured_truncated; }

static void transmit(char character) {
    while (!(inb(COM1_BASE + UART_LINE_STATUS) & LSR_TRANSMIT_EMPTY));
    outb(COM1_BASE + UART_DATA, (boot_uint8_t)character);
}

void serial_putchar(char character) {
    /* Captured before the port is consulted, so a machine with no port still
       keeps the log. The carriage return the port needs is not captured: it is
       a property of the wire, not of what was said. */
    capture(character);
    if (!serial_present) return;
    if (character == '\n') transmit('\r');
    transmit(character);
}

void serial_write(const char* text) {
    while (*text) serial_putchar(*text++);
}

void serial_write_hex(boot_uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        serial_putchar(digits[(value >> shift) & 0xFU]);
}

void serial_write_dec(boot_uint64_t value) {
    char buffer[21];
    int index = 20;

    buffer[index] = 0;
    do {
        buffer[--index] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value);
    serial_write(&buffer[index]);
}
