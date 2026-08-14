#include "keyboard.h"
#include "mouse.h"
#include "layout.h"
#include "serial.h"
#include "acpi.h"
#include "cpu.h"
#include "console.h"
#include "idt.h"
#include "io.h"
#include "pic.h"
#include "xhci.h"
#include "net.h"
#include "timer.h"
#include "program.h"

#define PS2_DATA 0x60U
#define PS2_STATUS 0x64U
#define PS2_COMMAND 0x64U

#define STATUS_OUTPUT_FULL 0x01U
#define STATUS_INPUT_FULL 0x02U
/* Set when the byte waiting came from the second port - the mouse. The two
   devices share one data port and one output buffer, so every read has to ask
   whose byte this is. */
#define STATUS_FROM_AUX 0x20U

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

/* The event queue, alongside the character one and independent of it.
 *
 * Two queues rather than one stream with a flag, because the two answer
 * different questions and mixing them breaks both: the shell reading a line
 * would find release events where it expected characters, and a game reading
 * events would find shifted characters that never match what it saw go down. */
static volatile boot_uint16_t events[BUFFER_SIZE];
static volatile boot_uint32_t event_head;
static volatile boot_uint32_t event_tail;

void keyboard_submit_event(int key, int released) {
    boot_uint32_t next;

    if (key <= 0 || key > 0x7FFF) return;
    next = (event_head + 1) % BUFFER_SIZE;
    /* Drop rather than overwrite, as with characters. A full queue means
       nobody is draining it, and the oldest events are the meaningful ones. */
    if (next == event_tail) return;
    events[event_head] =
        (boot_uint16_t)(released ? ((boot_uint16_t)key | KOI_KEY_RELEASED)
                                 : (boot_uint16_t)key);
    event_head = next;
}

/* USB is polled whenever there is a controller, not only when a keyboard is on
 * one.
 *
 * This asked `xhci_has_keyboard()` because collecting keystrokes was once the
 * only thing the poll did. It is not: it drains every controller's event ring,
 * and it is where a device plugged in after boot gets noticed. On a machine
 * whose keyboard is USB the two questions have the same answer and the
 * difference never shows. On a laptop with a PS/2 keyboard it is the whole
 * difference between plugging in a stick and nothing happening at all - which
 * is exactly what a laptop did: the stick was found when it was present at
 * boot, and never afterwards, because nothing was ever looking. */
static int usb_present(void) {
    return xhci_controller_count() != 0;
}

int keyboard_next_event(void) {
    boot_uint16_t event;

    if (usb_present()) xhci_poll();
    if (event_tail == event_head) return 0;
    event = events[event_tail];
    event_tail = (event_tail + 1) % BUFFER_SIZE;
    return (int)event;
}

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

/* Wait for a byte from the keyboard's side of the controller.
 *
 * A byte from the mouse is read and given to the mouse driver rather than
 * returned as an answer. A touchpad that the firmware has already set
 * streaming - which is what touching one before the system starts does - fills
 * this buffer with movement, and reading the first byte that appears as the
 * keyboard's reply means the reply is a coordinate. Every handshake below then
 * fails, and the machine starts with no keyboard on a laptop whose keyboard is
 * fine. */
static int controller_read_data(boot_uint8_t* value) {
    for (int spin = 0; spin < 100000; spin++) {
        boot_uint8_t status = inb(PS2_STATUS);

        if (!(status & STATUS_OUTPUT_FULL)) continue;
        if (status & STATUS_FROM_AUX) {
            mouse_from_controller(inb(PS2_DATA));
            continue;
        }
        *value = inb(PS2_DATA);
        return 1;
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

/* Which key a make code is, ignoring what it would type.
 *
 * The event queue reports this rather than the character, so that a key reads
 * the same going down as coming up. Reporting the shifted character would mean
 * a key pressed with shift and released without it looked like two different
 * keys, and anything tracking what is held would never see it stop. */
static int scancode_identity(boot_uint8_t code, int escaped) {
    if (escaped) {
        if (code == 0x1D) return KOI_KEY_CONTROL;
        if (code == 0x38) return KOI_KEY_ALT;
        return (int)translate_escaped(code);
    }
    switch (code) {
    case 0x2A: case 0x36: return KOI_KEY_SHIFT;
    case 0x1D: return KOI_KEY_CONTROL;
    case 0x38: return KOI_KEY_ALT;
    default: break;
    }
    if (code >= 0x3B && code <= 0x44) return KEY_F1 + (code - 0x3B);
    if (code == 0x57) return KEY_F1 + 10;
    if (code == 0x58) return KEY_F1 + 11;
    if (code >= sizeof(plain_map)) return 0;
    return (int)(unsigned char)plain_map[code];
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
        keyboard_submit_event(scancode_identity(code, 1), released);
        if (!released)
            keyboard_attention(control_held, alt_held,
                               (int)translate_escaped(code));
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

    keyboard_submit_event(scancode_identity(code, 0), released);

    switch (code) {
    /* Alt+Shift changes layout, whichever of the two is pressed second. The
       gesture is the one every other system uses, and a gesture somebody
       already has in their fingers is worth more than a better one they would
       have to learn. */
    case 0x2A: case 0x36:
        shift_held = !released;
        layout_gesture(shift_held, alt_held);
        return;
    case 0x1D: control_held = !released; return;
    case 0x38:
        alt_held = !released;
        layout_gesture(shift_held, alt_held);
        return;
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
    /* Ctrl+Escape asks for the shell's menu, and has to be told apart from a
       plain Escape before the control-code rule below turns it into one. */
    if (control_held && character == 27) {
        buffer_push(KOI_KEY_MENU);
        return;
    }
    /* Ctrl+letter produces the control code, as every terminal has since. */
    if (control_held) {
        if (character >= 'a' && character <= 'z') character = (char)(character - 'a' + 1);
        else if (character >= 'A' && character <= 'Z') character = (char)(character - 'A' + 1);
    }
    keyboard_submit((int)(unsigned char)character);
}

/* A byte the controller says came from the first port, read by somebody else.
   The pointer's interrupt hands keystrokes over this way, for the same reason
   this one hands it movement: one buffer, two devices, and whoever reads a
   byte owes it to its owner. */
void keyboard_from_controller(boot_uint8_t value) {
    handle_scancode(value);
}

/* Take whatever the controller is holding, whoever it belongs to.
 *
 * The 8042 has one byte of output buffer and will not deliver another - or
 * raise another interrupt - until that byte is read. So a handler that finds
 * the other device's byte and declines it does not merely skip a keystroke:
 * it stops the controller. Both devices then go quiet, having each delivered
 * a handful of events first, which is exactly what a touchpad and a keyboard
 * both dying a moment after startup looks like.
 *
 * Called from both interrupts and from the keyboard's own polling, so a byte
 * that arrives when no interrupt does - which is what a lost interrupt is -
 * costs a few milliseconds rather than the machine's input. */
void ps2_drain(void) {
    /* Nothing before the controller has been set up: reading port 0x60 on a
       machine that has no 8042 gives 0xFF for ever, and feeding that to the
       scancode decoder would invent keystrokes on exactly the machines that
       have no keyboard to press. */
    if (!keyboard_present) return;

    for (int spin = 0; spin < 64; spin++) {
        boot_uint8_t status = inb(PS2_STATUS);

        if (!(status & STATUS_OUTPUT_FULL)) return;
        if (status & STATUS_FROM_AUX) mouse_from_controller(inb(PS2_DATA));
        else handle_scancode(inb(PS2_DATA));
    }
}

static void keyboard_interrupt(INTERRUPT_FRAME* frame) {
    (void)frame;
    /* Drain: one interrupt can cover several bytes, and a byte left in the
       output buffer stops the controller raising IRQ1 again.
     *
     * Whose byte it is has to be asked every time. This loop used to take
     * whatever was there and read it as a scancode, and with a touchpad
     * streaming that meant the mouse's packets arriving here instead: the
     * pointer stopped moving, because its bytes were being eaten, and the
     * keyboard filled with keys nobody pressed - a movement byte with bit 7
     * set reads as a release, and one that happens to be 0x1D reads as
     * Control going down and staying there. Both devices dead, from one
     * missing test. */
    ps2_drain();
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

    /* Discard anything the firmware left in the output buffer - from either
       device, since both ports are disabled above and nothing more is coming
       from either until they are enabled again. */
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

/* One place where a key becomes what the buffer carries, so both keyboards get
 * layouts and neither driver has to know about alphabets.
 *
 * A letter outside ASCII is pushed as its UTF-8 bytes rather than as a code
 * point, because everything downstream already reads UTF-8: the console holds
 * a start byte until its continuations arrive, and the editors step through
 * text by character. Nothing had to change to receive Cyrillic - it only had
 * to be sent. */
/* Somebody has asked for the running program to stop.
 *
 * Noticed here rather than in either driver, because this is where both kinds
 * of keyboard already meet: a PS/2 keyboard and a USB one must not need to
 * agree about anything for Ctrl+C to work on both.
 *
 * Recorded rather than acted on. This runs inside an interrupt handler, and
 * the program it is about is somewhere in the middle of its own work with
 * kernel state half-changed around it. Stopping it here would mean unwinding
 * a stack from an interrupt, which is a way of turning a stuck program into a
 * stuck machine. So the flag waits for a moment the kernel chooses. */
static int break_requested;

/* Whether Ctrl+C stops the running program. On by default and put back on by
   the kernel when a program exits - see command.c, which does the same for
   the keyboard layout and for the screen. */
static int break_enabled = 1;

void keyboard_break_enable(int enabled) { break_enabled = enabled != 0; }

int keyboard_break_taken(void) {
    int asked = break_requested;
    break_requested = 0;
    return asked;
}

void keyboard_break_clear(void) { break_requested = 0; }

void keyboard_attention(int control_down, int alt_down, int key) {
    if (!control_down || !alt_down || key != KEY_DELETE) return;

    /* Said out loud before it happens. A machine that restarts in silence
       looks like a machine that crashed, and somebody who pressed this by
       accident deserves to know which of the two it was. */
    serial_write("KEYBOARD: Ctrl+Alt+Del, restarting\n");
    console_write("\nCtrl+Alt+Del - restarting.\n");

    /* ACPI first, because it is the machine's own answer, then the fallback
       that needs nothing: an empty interrupt table and one interrupt. */
    (void)acpi_reset();
    cpu_reset();
}

void keyboard_submit(int key) {
    boot_uint32_t code;

    if (!key) return;

    /* ETX, which is what Ctrl+C has produced since teletypes. It is not put
       into the buffer as well: a program that is being stopped has no use for
       the character, and one that is not being stopped never sees a Ctrl+C
       that meant anything else.
     *
     * Unless the program has said it would rather not be stopped. A desktop
     * is a program by every measure this kernel uses, so Ctrl+C killed Mizu -
     * and losing your session to a keystroke that means "stop this command" is
     * not a thing anybody expects from a desktop. A program that asks keeps
     * the keystroke as an ordinary character and decides for itself; the
     * kernel puts the rule back the moment it exits, so asking is not a way
     * to leave the machine unstoppable. */
    if (key == 3 && break_enabled) {
        break_requested = 1;
        return;
    }

    if (key < 0x20 || key > 0x7E) {   /* control codes, arrows, F-keys */
        buffer_push((boot_uint16_t)key);
        return;
    }

    code = layout_map(key);
    if (code < 0x80) {
        buffer_push((boot_uint16_t)code);
    } else if (code < 0x800) {
        buffer_push((boot_uint16_t)(0xC0 | (code >> 6)));
        buffer_push((boot_uint16_t)(0x80 | (code & 0x3F)));
    } else {
        buffer_push((boot_uint16_t)(0xE0 | (code >> 12)));
        buffer_push((boot_uint16_t)(0x80 | ((code >> 6) & 0x3F)));
        buffer_push((boot_uint16_t)(0x80 | (code & 0x3F)));
    }
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
    if (usb_present()) xhci_poll();
    /* And anything the 8042 is holding that no interrupt arrived for. On a
       machine where the two devices interleave heavily this is the difference
       between input that stops for good and input that stutters for a
       millisecond. */
    if (keyboard_present) ps2_drain();
    return buffer_tail != buffer_head;
}

int keyboard_getchar(void) {
    int key;
    int usb = xhci_has_keyboard();
    int controllers = usb_present();

    if (!keyboard_present && !usb) return 0;
    console_show_cursor(1);
    for (;;) {
        if ((key = keyboard_poll())) break;
        /* Waiting for a key is where a program spends most of a hang, and it
         * is a wait no keystroke can end - Ctrl+C is not put into the buffer,
         * so without this the one thing somebody presses to get out is the one
         * thing that cannot arrive.
         *
         * The flag is left set for the kernel to act on, except at the prompt,
         * where nobody else will take it and a Ctrl+C aimed at nothing must
         * not go on to land on the next program that starts. */
        if (break_requested) {
            if (!program_depth()) break_requested = 0;
            console_show_cursor(0);
            return 3;
        }
        /* Before the sleep, whichever keyboard we are waiting on: this is the
           only place a device plugged in while somebody sits at the prompt can
           be noticed. */
        if (controllers) xhci_poll();
        /* And the network, for exactly the same reason and with exactly the
           same history. Answering an ARP request or a ping is not something
           this system decides to do - it is something it does when somebody
           asks, and nobody was listening between commands. A machine that
           replies only while it is running `ping` itself is a machine no other
           machine can find. */
        if (net_link_ready()) net_poll();
        if (usb || controllers) {
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
           correct and stops an idle guest burning a host core. Only reached
           when there is no USB controller at all; otherwise the branch above
           has already slept, and it wakes often enough to keep looking. */
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
        /* Ctrl+C abandons the line. At the prompt that is all it does, and it
           says so the way DOS did; inside a program the kernel is about to
           stop it and will print its own. */
        if (key == 3) {
            if (!program_depth()) console_write("^C\n");
            length = 0;
            break;
        }
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
