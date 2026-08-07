#include "keyboard.h"
#include "acpi.h"
#include "console.h"
#include "idt.h"
#include "io.h"
#include "pic.h"
#include "xhci.h"
#include "timer.h"

#define PS2_DATA 0x60U
#define PS2_STATUS 0x64U
#define PS2_COMMAND 0x64U

#define STATUS_OUTPUT_FULL 0x01U
#define STATUS_INPUT_FULL 0x02U

#define COMMAND_READ_CONFIG 0x20U
#define COMMAND_WRITE_CONFIG 0x60U
#define COMMAND_DISABLE_PORT1 0xADU
#define COMMAND_ENABLE_PORT1 0xAEU
#define COMMAND_DISABLE_PORT2 0xA7U
#define COMMAND_SELF_TEST 0xAAU
#define SELF_TEST_PASSED 0x55U

/* Device-level commands, sent to the keyboard rather than the controller. */
#define DEVICE_RESET 0xFFU
#define DEVICE_ACK 0xFAU
#define DEVICE_RESEND 0xFEU   /* the keyboard asking for the command again */
#define DEVICE_SELF_TEST_PASSED 0xAAU

#define CONFIG_PORT1_INTERRUPT 0x01U
#define CONFIG_PORT1_TRANSLATION 0x40U

#define KEYBOARD_IRQ 1

#define BUFFER_SIZE 64

/* Scancode set 1: the code the controller sends with translation enabled. */
#define SCANCODE_ESCAPE_PREFIX 0xE0U
#define SCANCODE_RELEASE 0x80U

static int keyboard_present;

static volatile boot_uint16_t buffer[BUFFER_SIZE];
static volatile boot_uint32_t buffer_head;
static volatile boot_uint32_t buffer_tail;

static int shift_held;
static int control_held;
static int alt_held;
static int caps_lock;
static int escape_pending;

/* Scancode set 1, unshifted then shifted. Index is the make code. */
static const char plain_map[0x59] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,    '*', 0,   ' ', 0
};

static const char shift_map[0x59] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,    '*', 0,   ' ', 0
};

static void buffer_push(boot_uint16_t key) {
    boot_uint32_t next = (buffer_head + 1) % BUFFER_SIZE;
    /* Drop the key rather than overwrite unread ones: a full buffer means
       nobody is reading, and the oldest keystrokes are the meaningful ones. */
    if (next == buffer_tail) return;
    buffer[buffer_head] = key;
    buffer_head = next;
}

static void wait_for_input_clear(void) {
    for (int spin = 0; spin < 100000; spin++)
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL)) return;
}

static void controller_command(boot_uint8_t command) {
    wait_for_input_clear();
    outb(PS2_COMMAND, command);
}

static void controller_write_data(boot_uint8_t value) {
    wait_for_input_clear();
    outb(PS2_DATA, value);
}

static int controller_read_data(boot_uint8_t* value) {
    for (int spin = 0; spin < 100000; spin++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) {
            *value = inb(PS2_DATA);
            return 1;
        }
    }
    return 0;
}

static boot_uint16_t translate_escaped(boot_uint8_t code) {
    switch (code) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x49: return KEY_PAGE_UP;
    case 0x51: return KEY_PAGE_DOWN;
    case 0x53: return KEY_DELETE;
    case 0x52: return KEY_INSERT;
    case 0x1C: return '\n';  /* keypad enter */
    case 0x35: return '/';   /* keypad slash */
    default: return 0;
    }
}

static void handle_scancode(boot_uint8_t code) {
    int released;
    char character;

    if (code == SCANCODE_ESCAPE_PREFIX) {
        escape_pending = 1;
        return;
    }

    released = (code & SCANCODE_RELEASE) != 0;
    code = (boot_uint8_t)(code & 0x7F);

    if (escape_pending) {
        escape_pending = 0;
        /* The right-hand modifiers arrive escaped; treat them as their
           left-hand twins. */
        if (code == 0x1D) { control_held = !released; return; }
        if (code == 0x38) { alt_held = !released; return; }
        if (!released) {
            boot_uint16_t key = translate_escaped(code);
            if (key) buffer_push(key);
        }
        return;
    }

    switch (code) {
    case 0x2A: case 0x36: shift_held = !released; return;
    case 0x1D: control_held = !released; return;
    case 0x38: alt_held = !released; return;
    case 0x3A: if (!released) caps_lock = !caps_lock; return;
    default: break;
    }
    if (released) return;

    /* F1..F12: 0x3B-0x44 then 0x57, 0x58. */
    if (code >= 0x3B && code <= 0x44) {
        buffer_push((boot_uint16_t)(KEY_F1 + (code - 0x3B)));
        return;
    }
    if (code == 0x57) { buffer_push(KEY_F1 + 10); return; }
    if (code == 0x58) { buffer_push(KEY_F1 + 11); return; }

    if (code >= sizeof(plain_map)) return;
    character = shift_held ? shift_map[code] : plain_map[code];
    if (!character) return;

    /* Caps lock affects letters only, and inverts whatever shift decided. */
    if (caps_lock) {
        if (character >= 'a' && character <= 'z') character = (char)(character - 32);
        else if (character >= 'A' && character <= 'Z') character = (char)(character + 32);
    }
    /* Ctrl+letter produces the control code, as every terminal has since. */
    if (control_held) {
        if (character >= 'a' && character <= 'z') character = (char)(character - 'a' + 1);
        else if (character >= 'A' && character <= 'Z') character = (char)(character - 'A' + 1);
    }
    buffer_push((boot_uint16_t)(unsigned char)character);
}

static void keyboard_interrupt(INTERRUPT_FRAME* frame) {
    (void)frame;
    /* Drain: one interrupt can cover several bytes, and a byte left in the
       output buffer stops the controller raising IRQ1 again. */
    while (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
        handle_scancode(inb(PS2_DATA));
}

/* Read bytes until one of them is `wanted`, or patience runs out.
 *
 * The 8042 has one byte of output buffer and no notion of whose byte it is.
 * Anything the keyboard sent before we asked it a question is still sitting
 * there, and a person pressing a key while the system boots is not an unusual
 * event - it is what someone checking whether the keyboard has come alive does
 * every time. Judging a reply by the first byte read therefore mistakes a
 * scancode for an answer. */
static int read_until(boot_uint8_t wanted, int attempts) {
    boot_uint8_t reply;

    for (int attempt = 0; attempt < attempts; attempt++)
        if (controller_read_data(&reply) && reply == wanted) return 1;
    return 0;
}

/* Reset the device on port 1 and see whether anything answers.
 *
 * A keyboard replies 0xFA to acknowledge and then 0xAA when its own power-on
 * test finishes; an empty socket replies with nothing. Without this the driver
 * reports success on any board whose chipset has an 8042, which on a desktop
 * with only USB sockets means promising a keyboard that cannot exist.
 *
 * Run before IRQ1 is unmasked, so the replies arrive here rather than at the
 * interrupt handler. */
static int keyboard_present_on_port(void) {
    /* Immediately before the reset, not several milliseconds earlier during
       the self-test: the window in between is enough for a keystroke. */
    while (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) (void)inb(PS2_DATA);

    for (int attempt = 0; attempt < 2; attempt++) {
        boot_uint8_t reply;
        int resend = 0;

        controller_write_data(DEVICE_RESET);
        for (int byte = 0; byte < 4; byte++) {
            if (!controller_read_data(&reply)) break;
            if (reply == DEVICE_ACK) {
                /* The power-on test takes hundreds of milliseconds on real
                   hardware, far longer than the ordinary read timeout, so it
                   gets its own patience. */
                return read_until(DEVICE_SELF_TEST_PASSED, 20);
            }
            /* Not an answer to us - a scancode from a key pressed while the
               system was loading. Step over it. */
            if (reply == DEVICE_RESEND) { resend = 1; break; }
        }
        if (!resend) return 0;
    }
    return 0;
}

int keyboard_init(void) {
    boot_uint8_t config;
    boot_uint8_t result;

    /* Ask ACPI before touching the ports. On a machine with no 8042 these
       reads return 0xFF and every check below would "pass" on garbage. */
    if (!acpi_has_8042()) return KEYBOARD_ABSENT;

    controller_command(COMMAND_DISABLE_PORT1);
    controller_command(COMMAND_DISABLE_PORT2);

    /* Discard anything the firmware left in the output buffer. */
    while (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) (void)inb(PS2_DATA);

    controller_command(COMMAND_SELF_TEST);
    if (!controller_read_data(&result) || result != SELF_TEST_PASSED)
        return KEYBOARD_ABSENT;

    controller_command(COMMAND_READ_CONFIG);
    if (!controller_read_data(&config)) return KEYBOARD_ABSENT;
    /* Interrupts on, and keep translation to scancode set 1 - the map above
       assumes it, and it is what firmware normally leaves enabled. */
    config = (boot_uint8_t)((config | CONFIG_PORT1_INTERRUPT |
                             CONFIG_PORT1_TRANSLATION));
    controller_command(COMMAND_WRITE_CONFIG);
    controller_write_data(config);

    controller_command(COMMAND_ENABLE_PORT1);

    if (!keyboard_present_on_port()) return KEYBOARD_NO_DEVICE;

    buffer_head = 0;
    buffer_tail = 0;
    irq_register(KEYBOARD_IRQ, keyboard_interrupt);
    pic_unmask_irq(KEYBOARD_IRQ);
    keyboard_present = 1;
    return KEYBOARD_READY;
}

void keyboard_submit(int key) {
    if (key) buffer_push((boot_uint16_t)key);
}

int keyboard_available(void) {
    return keyboard_present || xhci_has_keyboard();
}

int keyboard_present_ps2(void) {
    return keyboard_present;
}

int keyboard_poll(void) {
    boot_uint16_t key;
    if (buffer_tail == buffer_head) return 0;
    key = buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    return (int)key;
}

int keyboard_pending(void) {
    if (xhci_has_keyboard()) xhci_poll();
    return buffer_tail != buffer_head;
}

int keyboard_getchar(void) {
    int key;
    int usb = xhci_has_keyboard();

    if (!keyboard_present && !usb) return 0;
    console_show_cursor(1);
    for (;;) {
        if ((key = keyboard_poll())) break;
        if (usb) {
            /* The controller's interrupt is still not routed anywhere, so USB
               keystrokes have to be collected rather than waited for.
               What changed is that something else now wakes us: with the timer
               interrupt running, `hlt` returns a thousand times a second,
               which is far more often than anyone types and enormously kinder
               than spinning. Without it there is nothing to wake on, and the
               loop falls back to `pause` - which at least tells the processor
               this is a spin loop. */
            xhci_poll();
            if (timer_is_interrupt_driven()) __asm__ volatile ("hlt");
            else __asm__ volatile ("pause");
            continue;
        }
        /* PS/2 raises IRQ1, so sleeping until the next interrupt is both
           correct and stops an idle guest burning a host core. */
        __asm__ volatile ("hlt");
    }
    console_show_cursor(0);
    return key;
}

boot_uint64_t keyboard_read_line(char* buffer_out, boot_uint64_t capacity) {
    boot_uint64_t length = 0;

    if (!capacity) return 0;
    for (;;) {
        int key = keyboard_getchar();
        if (!key) break;
        if (key == '\n') {
            console_putchar('\n');
            break;
        }
        if (key == '\b') {
            if (length) {
                length--;
                console_putchar('\b');
            }
            continue;
        }
        if (key > 0xFF || key < ' ') continue;
        if (length + 1 >= capacity) continue;
        buffer_out[length++] = (char)key;
        console_putchar((char)key);
    }
    buffer_out[length] = 0;
    return length;
}
