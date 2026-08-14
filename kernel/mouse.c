#include "mouse.h"
#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "apic.h"
#include "pic.h"
#include "serial.h"

/* The pointer, from whichever device is providing one.
 *
 * On a laptop that is the touchpad, and the good news about touchpads is that
 * they lie: a Synaptics pad comes up speaking the ordinary PS/2 mouse protocol
 * and only reveals its own if asked in its own language. Three bytes, buttons
 * and two movements - which is all a pointer needs. Gestures and multiple
 * fingers live behind the vendor protocol and are not needed to point at
 * things.
 *
 * The pad sits on the second port of the same 8042 controller the keyboard is
 * on, which is why a laptop reports a PS/2 controller at all in a year when
 * nothing has a PS/2 socket.
 */

#define PS2_DATA 0x60U
#define PS2_STATUS 0x64U
#define PS2_COMMAND 0x64U

#define STATUS_OUTPUT_FULL 0x01U
#define STATUS_INPUT_FULL 0x02U
/* Which port the waiting byte came from. Set means the auxiliary one, which
   is the whole reason a shared controller can carry two devices. */
#define STATUS_FROM_AUX 0x20U

#define COMMAND_READ_CONFIG 0x20U
#define COMMAND_WRITE_CONFIG 0x60U
#define COMMAND_ENABLE_PORT2 0xA8U
#define COMMAND_TEST_PORT2 0xA9U
#define COMMAND_TO_PORT2 0xD4U

#define CONFIG_PORT2_INTERRUPT 0x02U
#define CONFIG_PORT2_CLOCK 0x20U

#define COMMAND_DISABLE_PORT2 0xA7U
#define MOUSE_DISABLE 0xF5U     /* stop reporting, and stop it now */
#define MOUSE_RESET 0xFFU
#define MOUSE_DEFAULTS 0xF6U
#define MOUSE_ENABLE 0xF4U
#define MOUSE_SAMPLE_RATE 0xF3U
#define MOUSE_IDENTIFY 0xF2U
#define MOUSE_ACK 0xFAU

/* What the device says it is, in answer to MOUSE_IDENTIFY. Zero is the
   original three-byte protocol; the others are what the knock below unlocks. */
#define IDENTITY_PLAIN 0x00U
#define IDENTITY_WHEEL 0x03U
#define IDENTITY_WHEEL_5_BUTTON 0x04U

#define MOUSE_IRQ 12

/* Byte 0 of a packet. Bit 3 is always set, which is the only way to tell a
   packet boundary on a stream that has no framing of its own - and the only
   way back into step after a byte is dropped. */
#define PACKET_ALWAYS 0x08U
#define PACKET_LEFT 0x01U
#define PACKET_RIGHT 0x02U
#define PACKET_MIDDLE 0x04U
#define PACKET_SIGN_X 0x10U
#define PACKET_SIGN_Y 0x20U
#define PACKET_OVERFLOW 0xC0U

/* Byte 3, in the five-button protocol. The wheel is the low four bits; the
   extra buttons sit above it, which is why that protocol narrows the wheel
   from eight bits to four. Four is plenty: nobody turns a wheel eight notches
   between two interrupts. */
#define PACKET_Z_MASK 0x0FU
#define PACKET_BUTTON_4 0x10U
#define PACKET_BUTTON_5 0x20U

static int present;
static int pointer_x;
static int pointer_y;
static int limit_x = 639;
static int limit_y = 479;
static boot_uint8_t buttons;

/* Three bytes, or four once the wheel is unlocked. Kept as a variable because
   getting it wrong costs framing rather than a feature: a driver reading three
   bytes from a device sending four is permanently one byte behind. */
static boot_uint8_t packet[4];
static boot_uint32_t packet_bytes = 3;
static boot_uint32_t packet_at;
static boot_uint32_t moves;
static int identity;
static int scroll_units;

/* How many times each button has gone down, ever.
 *
 * Because asking "is it down now" loses clicks, and loses them in a way that
 * looks like a broken button rather than a missed sample. A click is a
 * hundred milliseconds if a person is being careless and rather less if they
 * are not; anything that polls at thirty frames a second and reads the level
 * will sooner or later look between the press and the release and see nothing
 * at all. A count cannot be missed - the reader compares it with what it last
 * saw, and the presses in between are all still there. */
static boot_uint32_t press_count[3];

static void wait_input_clear(void) {
    for (int spin = 0; spin < 100000; spin++)
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL)) return;
}

static void command(boot_uint8_t value) {
    wait_input_clear();
    outb(PS2_COMMAND, value);
}

static void write_data(boot_uint8_t value) {
    wait_input_clear();
    outb(PS2_DATA, value);
}

/* Wait for the device on the second port to answer, and take whatever comes.
 *
 * Not "whatever comes with the second-port bit set", which is what this asked
 * for briefly and should not have. The bit is reliable for a byte the
 * controller delivers by itself, and it is not reliable for the reply to a
 * command sent through it: some controllers set it, some do not, and on the
 * one this was tried on the mouse's own acknowledgements arrived without it.
 * Every handshake then failed, the driver decided there was no pointer, and
 * the cursor sat in the corner where a pointer that does not exist is drawn.
 *
 * So: the old behaviour, which works everywhere, with its known and small
 * cost - a key pressed in the exact moment of the handshake can be mistaken
 * for an acknowledgement. The window is a few hundred microseconds once, at
 * startup. The routing that matters is in the interrupt handlers, where the
 * bit does what it says. */
static boot_uint8_t read_data(void) {
    for (int spin = 0; spin < 200000; spin++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) return inb(PS2_DATA);
    return 0xFF;
}

/* Say something to the device on the second port rather than to the
   controller itself, which is what the prefix is for. Returns whether it
   acknowledged. */
/* Empty the output buffer. Whatever is in it belongs to a conversation that
   is over - or to a device that was talking before anybody asked it to. */
static void drain(void) {
    for (int spin = 0; spin < 4096; spin++) {
        if (!(inb(PS2_STATUS) & STATUS_OUTPUT_FULL)) return;
        (void)inb(PS2_DATA);
    }
}

static int tell_mouse(boot_uint8_t value) {
    command(COMMAND_TO_PORT2);
    write_data(value);
    return read_data() == MOUSE_ACK;
}

/* Set the reporting rate. Two bytes: the command, acknowledged, then the value,
   acknowledged - and it is the sequence of *values* rather than the rates
   themselves that matters below. */
static int set_rate(boot_uint8_t rate) {
    if (!tell_mouse(MOUSE_SAMPLE_RATE)) return 0;
    return tell_mouse(rate);
}

/* Ask the device what it is. */
static int identify(void) {
    if (!tell_mouse(MOUSE_IDENTIFY)) return -1;
    return (int)read_data();
}

/* The knock.
 *
 * A wheel mouse pretends to be a plain one until it is asked in a way a plain
 * one cannot notice. Microsoft's IntelliMouse chose a sequence of sample rates
 * - 200, 100, 80 - because setting the rate is something every mouse already
 * understands, so an old device sees three ordinary commands and a new one sees
 * a password. Ask its identity afterwards and it answers 3 instead of 0, and
 * from then on it sends four bytes rather than three. A second knock, 200, 200,
 * 80, gets 4: the wheel and two more buttons.
 *
 * This is also, and mainly, how a touchpad scrolls.
 *
 * A Synaptics pad has its own protocol carrying finger positions, pressure and
 * how many fingers are down - and none of that is needed here, because the pad
 * itself turns two fingers moving together into wheel notches when it is in
 * this mode. The gesture is recognised in the pad's firmware. So the whole of
 * "two-finger scrolling" is these six bytes, and a driver that implements the
 * vendor protocol to get it has done a great deal of work for the same result.
 *
 * Returns the identity the device settled on. */
static int unlock_wheel(void) {
    int found;

    if (!set_rate(200) || !set_rate(100) || !set_rate(80)) return IDENTITY_PLAIN;
    found = identify();
    if (found != IDENTITY_WHEEL) return IDENTITY_PLAIN;

    /* It has a wheel. Ask whether it also has the extra buttons - and if that
       second knock fails for any reason we still have the first one's answer,
       which is why the identity is re-read rather than assumed. */
    if (set_rate(200) && set_rate(200) && set_rate(80)) {
        int more = identify();
        if (more == IDENTITY_WHEEL_5_BUTTON) return IDENTITY_WHEEL_5_BUTTON;
    }
    return IDENTITY_WHEEL;
}

/* The wheel, out of whichever protocol is in use.
 *
 * The wire counts positive when the wheel turns towards the user, which every
 * program then has to negate before it means anything. Negated once, here: a
 * positive result is scrolling up, and up is the direction the content moves. */
static int scroll_from(boot_uint8_t byte) {
    if (identity == IDENTITY_WHEEL_5_BUTTON) {
        int z = byte & PACKET_Z_MASK;
        if (z & 0x08) z -= 16;      /* four bits, signed */
        return -z;
    }
    {
        int z = (int)byte;
        if (z & 0x80) z -= 256;     /* eight bits, signed */
        return -z;
    }
}

/* One complete packet, turned into a position.
 *
 * The movements are nine-bit signed values with the sign kept in the first
 * byte, which is an encoding nobody would choose today and everybody still
 * has to implement. Y counts upwards, and screens count downwards, so it is
 * subtracted rather than added - which is the sort of thing that produces a
 * pointer that works perfectly and moves the wrong way. */
static void handle_packet(void) {
    int dx = packet[1];
    int dy = packet[2];

    {
        boot_uint8_t now = (boot_uint8_t)(packet[0] & (PACKET_LEFT |
                                                       PACKET_RIGHT |
                                                       PACKET_MIDDLE));
        static const boot_uint8_t which[3] = { PACKET_LEFT, PACKET_RIGHT,
                                               PACKET_MIDDLE };

        for (int index = 0; index < 3; index++)
            if ((now & which[index]) && !(buttons & which[index]))
                press_count[index]++;
        buttons = now;
    }

    if (packet_bytes == 4) {
        /* The wheel first: it is reported in the same packet as movement, and
           a packet whose X or Y overflowed is thrown away below. Two fingers
           dragged quickly are exactly the case that overflows, so reading the
           scroll after that test would lose the notches that matter most. */
        scroll_units += scroll_from(packet[3]);
        if (identity == IDENTITY_WHEEL_5_BUTTON)
            buttons |= (boot_uint8_t)(packet[3] & (PACKET_BUTTON_4 |
                                                   PACKET_BUTTON_5));
    }

    if (packet[0] & PACKET_OVERFLOW) return;   /* moved further than it can say */
    if (packet[0] & PACKET_SIGN_X) dx -= 256;
    if (packet[0] & PACKET_SIGN_Y) dy -= 256;

    pointer_x += dx;
    pointer_y -= dy;

    if (pointer_x < 0) pointer_x = 0;
    if (pointer_y < 0) pointer_y = 0;
    if (pointer_x > limit_x) pointer_x = limit_x;
    if (pointer_y > limit_y) pointer_y = limit_y;
    moves++;
}

/* One byte from the second port, wherever it was read.
 *
 * Both interrupts share one data port, and either handler can find the other
 * device's byte waiting when it looks. The keyboard's handler hands anything
 * marked as coming from the mouse to this, rather than reading it as a
 * scancode - which is what it used to do, and is why a touchpad that was
 * already streaming when the system started left the machine with no pointer
 * and a keyboard full of modifiers nobody had pressed. */
void mouse_from_controller(boot_uint8_t value) {
    /* Nothing before the driver is up. Bytes arriving during the handshake
       are answers to it, and the packet decoder must not eat them - which is
       the other half of the regression above: the keyboard's handler was
       feeding the mouse's acknowledgements into a state machine that was not
       running yet, and they never reached the code waiting for them. */
    if (!present) return;

    /* Back into step. A stream with no framing needs one bit that is always
       true of a first byte, and this protocol has exactly one. */
    if (packet_at == 0 && !(value & PACKET_ALWAYS)) return;

    packet[packet_at++] = value;
    if (packet_at == packet_bytes) {
        packet_at = 0;
        handle_packet();
    }
}

static void mouse_interrupt(INTERRUPT_FRAME* frame) {
    (void)frame;
    /* Everything waiting, not only what is ours.
     *
     * This used to read one byte and only if it was marked as the mouse's,
     * leaving a keystroke sitting in the buffer for somebody else - and the
     * 8042 delivers nothing further, and raises no further interrupt, until
     * that byte is read. Declining it stopped the controller: the pointer
     * managed eight reports and the keyboard nine keystrokes, and then both
     * went silent. The drain routes each byte to whoever it belongs to. */
    ps2_drain();
}

/* What the screen was, so that a second attempt does not need to be told
   again. */
static boot_uint32_t known_width;
static boot_uint32_t known_height;

int mouse_init(boot_uint32_t width, boot_uint32_t height) {
    boot_uint8_t config;

    known_width = width;
    known_height = height;

    /* Where it may go, and where it starts: the middle, because a pointer
       that begins in a corner looks like one that is not working. */
    limit_x = (int)width - 1;
    limit_y = (int)height - 1;
    pointer_x = limit_x / 2;
    pointer_y = limit_y / 2;

    /* Silence first, and this is the whole of the touchpad bug.
     *
     * A finger on the touchpad before the system starts leaves the firmware's
     * pointer already reporting, and a reporting device fills the output
     * buffer with movement. Every read below then returns a coordinate: the
     * port test read one and decided there was no second port, said so, and
     * gave up - on a laptop whose touchpad works perfectly. Which is exactly
     * the shape of the report: touch it during the firmware and there is no
     * pointer afterwards; do not touch it and everything is fine.
     *
     * Disabling the port stops its clock, so nothing more can arrive while
     * the buffer is emptied; the device itself is told to stop reporting once
     * it can be talked to, so that the handshakes below are answers rather
     * than movement. */
    command(COMMAND_DISABLE_PORT2);
    drain();

    /* Does the controller have a second port at all? A machine with no
       touchpad and no PS/2 mouse fails this and says so rather than waiting
       out four timeouts. */
    command(COMMAND_TEST_PORT2);
    if (read_data() != 0x00) {
        serial_write("MOUSE: the controller has no second port\n");
        return 0;
    }

    command(COMMAND_ENABLE_PORT2);

    command(COMMAND_READ_CONFIG);
    config = read_data();
    /* Interrupts from the second port on, and its clock line enabled - the
       firmware often leaves the clock disabled, and a device whose clock is
       off answers nothing while looking exactly like a device that is absent. */
    config = (boot_uint8_t)((config | CONFIG_PORT2_INTERRUPT) &
                            ~CONFIG_PORT2_CLOCK);
    command(COMMAND_WRITE_CONFIG);
    write_data(config);

    /* Told to be quiet before it is asked anything. A device that was left
       streaming answers the first question with a packet, and the answer to
       the second question is then the rest of the first packet. Not every
       device acknowledges this while it is in that state, so the reply is not
       insisted on - what matters is that the line is empty afterwards. */
    (void)tell_mouse(MOUSE_DISABLE);
    drain();

    if (!tell_mouse(MOUSE_RESET)) {
        /* Once more, in case that acknowledgement was the tail of a packet
           that was already on its way. */
        drain();
        if (!tell_mouse(MOUSE_RESET)) {
            serial_write("MOUSE: nothing on the second port\n");
            return 0;
        }
    }
    /* A reset is answered with an acknowledgement, then a self-test result,
       then the device identity. Read and discarded: what matters is that
       something is there and talking. */
    (void)read_data();
    (void)read_data();

    if (!tell_mouse(MOUSE_DEFAULTS)) return 0;

    /* While reporting is off, which is the only time the knock is safe: it
       reads answers back, and a device that is streaming movement would have
       filled the line with packets to read them out of. */
    identity = unlock_wheel();
    packet_bytes = (identity == IDENTITY_PLAIN) ? 3 : 4;

    /* The knock left the rate at 80 samples a second, which is slow enough to
       see as a stuttering pointer. Back to the usual 100. */
    (void)set_rate(100);

    if (!tell_mouse(MOUSE_ENABLE)) {
        serial_write("MOUSE: it would not start reporting\n");
        return 0;
    }

    packet_at = 0;
    irq_register(MOUSE_IRQ, mouse_interrupt);

    /* And make sure the line can actually arrive.
     *
     * This used to be done once, at startup, by the code that moves
     * everything from the 8259 to the IO APIC - and only for a pointer that
     * already existed at that moment. A pointer found later got a handler, a
     * `present` flag and no interrupt at all: `mouse` said the pointer was
     * working, and it never moved, because nothing was ever delivered. That
     * is a worse answer than saying there is none.
     *
     * Asked for here instead, by the driver that needs it, whenever it starts
     * - which is the same reason the routing lives with the device rather
     * than with the boot sequence. */
    if (apic_available()) {
        if (!apic_route_irq(MOUSE_IRQ, IRQ_BASE + MOUSE_IRQ))
            serial_write("MOUSE: the IO APIC would not take its line\n");
    } else {
        pic_unmask_irq(MOUSE_IRQ);
    }
    present = 1;
    if (identity == IDENTITY_PLAIN)
        serial_write("MOUSE: PS/2 pointer ready, no wheel\n");
    else if (identity == IDENTITY_WHEEL)
        serial_write("MOUSE: PS/2 pointer ready with a wheel\n");
    else
        serial_write("MOUSE: PS/2 pointer ready with a wheel and 5 buttons\n");
    return 1;
}

int mouse_present(void) { return present; }

/* Try again, for a pointer that was not there when the machine started.
 *
 * Asked for by the shell (`mouse`) and by the moment a program takes the
 * screen: a desktop with no pointer is the place where somebody notices, and
 * one more handshake costs a few milliseconds. It is also the honest answer to
 * "can we not just start it later" - the initialisation is not expensive and
 * nothing says it may happen only once. */
int mouse_restart(void) {
    if (present) return 1;
    if (!known_width || !known_height) return 0;
    return mouse_init(known_width, known_height);
}
int mouse_x(void) { return pointer_x; }
int mouse_y(void) { return pointer_y; }
int mouse_buttons(void) { return buttons; }
boot_uint32_t mouse_movements(void) { return moves; }
int mouse_has_wheel(void) { return identity != IDENTITY_PLAIN; }

/* Running total rather than "what has arrived since you last asked".
 *
 * Taking would mean whoever asks first gets it and everybody else sees a still
 * wheel, and that is exactly the shape of bug that only appears once something
 * else starts asking too. A total can be read by any number of callers, each
 * remembering what it last saw. */
int mouse_scroll(void) { return scroll_units; }

boot_uint32_t mouse_presses(int button) {
    if (button < 0 || button > 2) return 0;
    return press_count[button];
}

void mouse_place(int x, int y) {
    pointer_x = x < 0 ? 0 : (x > limit_x ? limit_x : x);
    pointer_y = y < 0 ? 0 : (y > limit_y ? limit_y : y);
}
