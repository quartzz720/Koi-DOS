#include "mouse.h"
#include "io.h"
#include "idt.h"
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

#define MOUSE_RESET 0xFFU
#define MOUSE_DEFAULTS 0xF6U
#define MOUSE_ENABLE 0xF4U
#define MOUSE_ACK 0xFAU

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

static int present;
static int pointer_x;
static int pointer_y;
static int limit_x = 639;
static int limit_y = 479;
static boot_uint8_t buttons;

static boot_uint8_t packet[3];
static boot_uint32_t packet_at;
static boot_uint32_t moves;

static void wait_input_clear(void) {
    for (int spin = 0; spin < 100000; spin++)
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL)) return;
}

static void wait_output_full(void) {
    for (int spin = 0; spin < 100000; spin++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL) return;
}

static void command(boot_uint8_t value) {
    wait_input_clear();
    outb(PS2_COMMAND, value);
}

static void write_data(boot_uint8_t value) {
    wait_input_clear();
    outb(PS2_DATA, value);
}

static boot_uint8_t read_data(void) {
    wait_output_full();
    return inb(PS2_DATA);
}

/* Say something to the device on the second port rather than to the
   controller itself, which is what the prefix is for. Returns whether it
   acknowledged. */
static int tell_mouse(boot_uint8_t value) {
    command(COMMAND_TO_PORT2);
    write_data(value);
    return read_data() == MOUSE_ACK;
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

    buttons = (boot_uint8_t)(packet[0] & (PACKET_LEFT | PACKET_RIGHT |
                                          PACKET_MIDDLE));

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

static void mouse_interrupt(INTERRUPT_FRAME* frame) {
    boot_uint8_t status = inb(PS2_STATUS);

    (void)frame;
    /* Only bytes from the second port. The keyboard shares this controller
       and its interrupt is somebody else's; taking its bytes here would be
       both a lost keystroke and a corrupted packet. */
    if (!(status & STATUS_OUTPUT_FULL) || !(status & STATUS_FROM_AUX)) return;

    {
        boot_uint8_t value = inb(PS2_DATA);

        /* Back into step. A stream with no framing needs one bit that is
           always true of a first byte, and this protocol has exactly one. */
        if (packet_at == 0 && !(value & PACKET_ALWAYS)) return;

        packet[packet_at++] = value;
        if (packet_at == 3) {
            packet_at = 0;
            handle_packet();
        }
    }
}

int mouse_init(boot_uint32_t width, boot_uint32_t height) {
    boot_uint8_t config;

    /* Where it may go, and where it starts: the middle, because a pointer
       that begins in a corner looks like one that is not working. */
    limit_x = (int)width - 1;
    limit_y = (int)height - 1;
    pointer_x = limit_x / 2;
    pointer_y = limit_y / 2;

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

    if (!tell_mouse(MOUSE_RESET)) {
        serial_write("MOUSE: nothing on the second port\n");
        return 0;
    }
    /* A reset is answered with an acknowledgement, then a self-test result,
       then the device identity. Read and discarded: what matters is that
       something is there and talking. */
    (void)read_data();
    (void)read_data();

    if (!tell_mouse(MOUSE_DEFAULTS)) return 0;
    if (!tell_mouse(MOUSE_ENABLE)) {
        serial_write("MOUSE: it would not start reporting\n");
        return 0;
    }

    packet_at = 0;
    irq_register(MOUSE_IRQ, mouse_interrupt);
    present = 1;
    serial_write("MOUSE: PS/2 pointer ready\n");
    return 1;
}

int mouse_present(void) { return present; }
int mouse_x(void) { return pointer_x; }
int mouse_y(void) { return pointer_y; }
int mouse_buttons(void) { return buttons; }
boot_uint32_t mouse_movements(void) { return moves; }

void mouse_place(int x, int y) {
    pointer_x = x < 0 ? 0 : (x > limit_x ? limit_x : x);
    pointer_y = y < 0 ? 0 : (y > limit_y ? limit_y : y);
}
