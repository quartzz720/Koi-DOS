#include "e1000.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"

/* The registers this driver touches, and no others. The card has hundreds;
   almost all of them are for features nothing here uses. */
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_ICR 0x00C0
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_TXDCTL 0x3828
/* Transmit arbitration, which decides whether the transmitter gets to use the
   bus at all. Nothing here has ever written it and its reset value is not
   documented to be usable. */
#define REG_TARC0 0x3840
#define REG_TCTL_EXT 0x0404
#define REG_MTA 0x5200          /* 128 entries of multicast filter */
#define REG_RAL0 0x5400
#define REG_RAH0 0x5404

#define CTRL_FULL_DUPLEX 0x00000001U
#define CTRL_AUTO_SPEED 0x00000020U
#define CTRL_SET_LINK_UP 0x00000040U
#define CTRL_SPEED_SHIFT 8
#define CTRL_FORCE_SPEED 0x00000800U
#define CTRL_FORCE_DUPLEX 0x00001000U
#define CTRL_RESET 0x04000000U

#define STATUS_LINK_UP 0x00000002U
/* Bit zero of STATUS is the duplex the physical layer settled on, which is a
   different question from bit zero of CTRL - that one is what the MAC has been
   told to be. They are not kept in step by anything. */
#define STATUS_LINK_FULL_DUPLEX 0x00000001U
/* PHY Reset Asserted, on the chipset parts only - the ones where the physical
   layer lives beside the management engine rather than on the card. The
   hardware sets it at reset and waits to be told the driver has noticed;
   until then it holds parts of its own configuration back. Meaningless and
   harmless on the older parts, which is why it is cleared unconditionally. */
#define STATUS_PHY_RESET_ASSERTED 0x00000400U
#define STATUS_SPEED_SHIFT 6
#define STATUS_SPEED_MASK 0x03

#define RCTL_ENABLE 0x00000002U
#define RCTL_BROADCAST 0x00008000U
#define RCTL_STRIP_CRC 0x04000000U
/* Buffer size 2048 is the default encoding - all four size bits zero - which
   is why there is no constant for it and why that is easy to get wrong. */

#define TCTL_ENABLE 0x00000002U
#define TCTL_PAD_SHORT 0x00000008U
#define TCTL_COLLISION_THRESHOLD (15U << 4)
#define TCTL_COLLISION_DISTANCE (0x40U << 12)

/* Inter-packet gap for copper, from the manual's table: 10, 8 and 6 in the
   three fields. Left at the reset value the card transmits and nothing on the
   wire understands it. */
#define TIPG_COPPER 0x0060200AU

#define RAH_VALID 0x80000000U

#define RX_STATUS_DONE 0x01
#define RX_STATUS_END 0x02

#define TX_CMD_END 0x01
#define TX_CMD_CRC 0x02
#define TX_CMD_REPORT 0x08
#define TX_STATUS_DONE 0x01

/* Descriptor counts. The length register is in bytes and must be a multiple of
   128, so both of these are chosen to land on one. */
#define RX_COUNT 16
#define TX_COUNT 8
#define BUFFER_SIZE 2048
#define FRAME_MAX 1514

typedef struct {
    boot_uint64_t address;
    boot_uint16_t length;
    boot_uint16_t checksum;
    boot_uint8_t status;
    boot_uint8_t errors;
    boot_uint16_t special;
} RX_DESCRIPTOR;

typedef struct {
    boot_uint64_t address;
    boot_uint16_t length;
    boot_uint8_t checksum_offset;
    boot_uint8_t command;
    boot_uint8_t status;
    boot_uint8_t checksum_start;
    boot_uint16_t special;
} TX_DESCRIPTOR;

static volatile boot_uint8_t* registers;
static RX_DESCRIPTOR* rx_ring;
static TX_DESCRIPTOR* tx_ring;
static boot_uint8_t* rx_buffers;
static boot_uint8_t* tx_buffers;
static boot_uint32_t rx_next;
static boot_uint32_t tx_next;
static boot_uint8_t mac[6];
static int ready;

static boot_uint32_t frames_sent;
static boot_uint32_t frames_received;
static boot_uint32_t frames_dropped;

static void log(const char* text) { serial_write(text); }
static void log_hex(boot_uint64_t value) { serial_write_hex(value); }

static boot_uint32_t read32(boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(registers + offset);
}

static void write32(boot_uint32_t offset, boot_uint32_t value) {
    *(volatile boot_uint32_t*)(registers + offset) = value;
}

/* The hardware address, without going near the EEPROM.
 *
 * Reading it properly means talking to the non-volatile memory, and on the
 * newer parts that means sharing a semaphore with the management engine - a
 * second processor on the same chip with its own opinions about who owns the
 * link. All of which is avoidable: the firmware has already read the address
 * out and left it in the receive address register, because that is the
 * register the card filters on. Taking it from there is one read and cannot
 * deadlock against anything. */
static int read_address(void) {
    boot_uint32_t low = read32(REG_RAL0);
    boot_uint32_t high = read32(REG_RAH0);

    if (!(high & RAH_VALID)) return 0;
    mac[0] = (boot_uint8_t)low;
    mac[1] = (boot_uint8_t)(low >> 8);
    mac[2] = (boot_uint8_t)(low >> 16);
    mac[3] = (boot_uint8_t)(low >> 24);
    mac[4] = (boot_uint8_t)high;
    mac[5] = (boot_uint8_t)(high >> 8);

    /* All zeroes passes the valid bit on some parts and is not an address. */
    for (int index = 0; index < 6; index++)
        if (mac[index]) return 1;
    return 0;
}

static int reset_card(void) {
    boot_uint64_t start;

    /* Interrupts off before anything else. Nothing here is wired to handle one
       and the firmware may well have left some enabled. */
    write32(REG_IMC, 0xFFFFFFFFU);
    (void)read32(REG_ICR);

    write32(REG_CTRL, read32(REG_CTRL) | CTRL_RESET);
    /* The manual asks for a delay before the register is even read back: the
       reset is not instantaneous and the bit reads as clear while it is still
       happening. */
    timer_wait(10);

    start = timer_ticks();
    while (read32(REG_CTRL) & CTRL_RESET) {
        timer_poll();
        if (timer_expired(start, 1000)) {
            log("E1000: the card did not come out of reset\n");
            return 0;
        }
    }

    write32(REG_IMC, 0xFFFFFFFFU);
    (void)read32(REG_ICR);

    /* Acknowledge the PHY reset the hardware asserted on its own. Written back
       with the bit clear, which is how this register is acknowledged - it is
       not a status bit that goes away when the condition does. */
    {
        boot_uint32_t status = read32(REG_STATUS);
        if (status & STATUS_PHY_RESET_ASSERTED)
            write32(REG_STATUS, status & ~STATUS_PHY_RESET_ASSERTED);
    }
    return 1;
}

static int build_rings(void) {
    void* descriptors = alloc_page();

    /* Both rings out of one page: sixteen receive descriptors and eight
       transmit ones is 384 bytes, and the alignment the card wants - sixteen
       bytes - a page satisfies many times over. */
    if (!descriptors) return 0;
    memset(descriptors, 0, PAGE_SIZE);
    rx_ring = (RX_DESCRIPTOR*)descriptors;
    tx_ring = (TX_DESCRIPTOR*)((boot_uint8_t*)descriptors + RX_COUNT * 16);

    rx_buffers = (boot_uint8_t*)alloc_pages(RX_COUNT * BUFFER_SIZE / PAGE_SIZE);
    tx_buffers = (boot_uint8_t*)alloc_pages(TX_COUNT * BUFFER_SIZE / PAGE_SIZE);
    if (!rx_buffers || !tx_buffers) return 0;

    for (boot_uint32_t index = 0; index < RX_COUNT; index++) {
        rx_ring[index].address = (boot_uint64_t)(unsigned long long)
                                 (rx_buffers + index * BUFFER_SIZE);
        rx_ring[index].status = 0;
    }
    for (boot_uint32_t index = 0; index < TX_COUNT; index++) {
        tx_ring[index].address = (boot_uint64_t)(unsigned long long)
                                 (tx_buffers + index * BUFFER_SIZE);
        /* Done, so the first pass round the ring does not wait for a transfer
           that never happened. */
        tx_ring[index].status = TX_STATUS_DONE;
    }

    write32(REG_RDBAL, (boot_uint32_t)(unsigned long long)rx_ring);
    write32(REG_RDBAH, (boot_uint32_t)((boot_uint64_t)(unsigned long long)rx_ring >> 32));
    write32(REG_RDLEN, RX_COUNT * 16);
    write32(REG_RDH, 0);
    /* The tail is the last descriptor the card may write into, so it points at
       the end of the ring rather than the start: head equal to tail would mean
       an empty ring and the card would take nothing at all. */
    write32(REG_RDT, RX_COUNT - 1);
    rx_next = 0;

    write32(REG_TDBAL, (boot_uint32_t)(unsigned long long)tx_ring);
    write32(REG_TDBAH, (boot_uint32_t)((boot_uint64_t)(unsigned long long)tx_ring >> 32));
    write32(REG_TDLEN, TX_COUNT * 16);
    write32(REG_TDH, 0);
    write32(REG_TDT, 0);
    tx_next = 0;

    /* Ours, broadcast, and the card strips the frame check sequence so what
       arrives is the frame and nothing else. Promiscuous mode is deliberately
       not set: a DOS has no business reading somebody else's traffic. */
    write32(REG_RCTL, RCTL_ENABLE | RCTL_BROADCAST | RCTL_STRIP_CRC);
    write32(REG_TIPG, TIPG_COPPER);
    return 1;
}

/* Start the transmitter, and not before the link is up.
 *
 * This was done with the rest of the setup, which is the order the older parts
 * do not mind and the order this card was found not to accept: everything read
 * back correctly - ring base, length, the enable bit itself - and the head
 * pointer never moved off zero while six descriptors waited. The collision
 * distance is the reason it is ordered this way. It differs between half and
 * full duplex, the duplex is not known until the link has negotiated, and the
 * transmitter takes its copy of the configuration when it is enabled. Enabled
 * first, it takes the answer to a question nothing had asked yet. */
static void start_transmitter(void) {
    boot_uint32_t status = read32(REG_STATUS);
    boot_uint32_t full_duplex = status & STATUS_LINK_FULL_DUPLEX;
    boot_uint32_t control = TCTL_ENABLE | TCTL_PAD_SHORT |
                            TCTL_COLLISION_THRESHOLD;

    /* Tell the MAC what the link turned out to be.
     *
     * Somebody has to, and on these parts it is not the card. Automatic speed
     * detection - which on the older ones watches the physical layer and fills
     * the fields in unasked - does not exist here: the datasheet has those
     * bits reserved, and so is the "set link up" bit next to them, which is
     * read-only and always reads back set.
     *
     * What the hardware does instead is sample the duplex from the physical
     * layer "on the asserting edge of the PHY LINK signal" - on the rising
     * edge, once, and never again. And the reset a few lines above happens
     * while the link is already up, because the firmware brought it up long
     * before this driver existed. That edge is in the past. The MAC keeps
     * whatever it had, which turned out to be half duplex.
     *
     * Nothing complains about that. A half duplex transmitter simply waits
     * for the medium to go quiet before it may send, and on a full duplex
     * link, by its rules, it never does. Every register reads back correctly,
     * the link is up, frames arrive - and the transmit head pointer never
     * moves off zero, because the transmitter is obediently waiting for a
     * silence that is not coming.
     *
     * So both are forced, from what the physical layer actually negotiated.
     * There is nothing left to detect once it has already happened. */
    {
        boot_uint32_t speed = (status >> STATUS_SPEED_SHIFT) & STATUS_SPEED_MASK;
        boot_uint32_t setting = read32(REG_CTRL);

        setting &= ~(3U << CTRL_SPEED_SHIFT);
        setting |= speed << CTRL_SPEED_SHIFT;
        setting |= CTRL_FORCE_SPEED | CTRL_FORCE_DUPLEX;
        if (full_duplex) setting |= CTRL_FULL_DUPLEX;
        else setting &= ~CTRL_FULL_DUPLEX;
        write32(REG_CTRL, setting);
        (void)read32(REG_CTRL);
    }

    /* Sixty-four byte times on a full duplex link, five hundred and twelve on
       a half duplex one, which is a distance rather than a preference: it is
       how far a collision can be before it stops being this card's problem. */
    control |= full_duplex ? TCTL_COLLISION_DISTANCE : (0x200U << 12);
    /* Only our own fields, and nothing else.
     *
     * This register was being assembled from scratch and written whole, which
     * quietly zeroed two things that are not ours to zero. One is reserved
     * with a default of one - the datasheet says read-only, and it read back
     * as zero anyway. The other is the read request threshold, which decides
     * when the transmitter asks the packet buffer for data; its default is 01b
     * and it was being set to 00b by omission.
     *
     * Writing a control register as a fresh constant is a habit worth losing
     * on hardware that documents defaults for bits it does not explain. */
    {
        boot_uint32_t existing = read32(REG_TCTL);

        existing &= ~(TCTL_ENABLE | TCTL_PAD_SHORT |
                      (0xFFU << 4) | (0x3FFU << 12));
        write32(REG_TCTL, existing | control);
    }
    /* Read back, which on this register is not decoration: the write is
       latched by hardware that may not be ready to take it. */
    (void)read32(REG_TCTL);
}

int e1000_init(const PCI_DEVICE* device) {
    boot_uint64_t base;
    boot_uint32_t status;

    if (ready) return 0;                 /* one card is enough for now */
    if (device->bar[0] & 1U) return 0;   /* an I/O port BAR: not this family */
    base = (boot_uint64_t)(device->bar[0] & ~0x0FU);
    if (!base) return 0;

    pci_enable_bus_mastering(device);
    registers = (volatile boot_uint8_t*)(unsigned long long)base;

    log("E1000: ");
    log_hex(device->vendor_id);
    log(":");
    log_hex(device->device_id);
    log(" at ");
    log_hex(base);
    log("\n");

    /* Reset the card, unless the firmware has already brought the link up.
     *
     * The datasheet is explicit that the MAC samples the physical layer "on
     * the asserting edge of the PHY LINK signal" - once, when the link comes
     * up. The firmware brings the link up long before this driver exists, so
     * that edge is already in the past, and a reset here throws away
     * everything the MAC sampled from it with no way to ask for it again. The
     * duplex is the part of that which is visible in a register; there is no
     * reason to believe it is the only part.
     *
     * So a card that is already up is left alone and configured in place.
     * Interrupts are still silenced - that is the one thing that must not be
     * inherited from whoever was here before. */
    if (read32(REG_STATUS) & STATUS_LINK_UP) {
        log("E1000: the link is already up; configuring without a reset\n");
        write32(REG_IMC, 0xFFFFFFFFU);
        (void)read32(REG_ICR);
    } else if (!reset_card()) {
        return 0;
    }

    /* Bring the link up and let the card work out speed and duplex with
       whatever is on the other end. */
    write32(REG_CTRL, read32(REG_CTRL) | CTRL_SET_LINK_UP | CTRL_AUTO_SPEED);

    /* An empty multicast filter. After a reset it is not guaranteed to be
       anything, and a full one accepts every multicast frame on the wire. */
    for (boot_uint32_t index = 0; index < 128; index++)
        write32(REG_MTA + index * 4, 0);

    if (!read_address()) {
        log("E1000: the card will not say what its address is\n");
        return 0;
    }
    log("E1000: hardware address ");
    for (int index = 0; index < 6; index++) {
        static const char digits[] = "0123456789ABCDEF";
        char text[4];
        text[0] = index ? ':' : ' ';
        text[1] = digits[(mac[index] >> 4) & 0xF];
        text[2] = digits[mac[index] & 0xF];
        text[3] = 0;
        log(index ? text : text + 1);
    }
    log("\n");

    if (!build_rings()) {
        log("E1000: out of memory for the descriptor rings\n");
        return 0;
    }

    /* Auto-negotiation takes a moment, and a cable that is not plugged in
       takes forever. Waited for briefly and then reported either way: a card
       with no cable is not a failure, it is a card with no cable. */
    {
        boot_uint64_t start = timer_ticks();
        while (!(read32(REG_STATUS) & STATUS_LINK_UP)) {
            timer_poll();
            if (timer_expired(start, 3000)) break;
        }
    }

    start_transmitter();

    status = read32(REG_STATUS);
    log("E1000: ");
    if (status & STATUS_LINK_UP) {
        static const char* speeds[] = { "10", "100", "1000", "1000" };
        log("link up at ");
        log(speeds[(status >> STATUS_SPEED_SHIFT) & STATUS_SPEED_MASK]);
        log(" Mb/s, ");
        log((status & CTRL_FULL_DUPLEX) ? "full duplex\n" : "half duplex\n");
    } else {
        log("no cable\n");
    }

    ready = 1;
    return 1;
}

int e1000_ready(void) { return ready; }

int e1000_link_down(void) {
    return ready && !(read32(REG_STATUS) & STATUS_LINK_UP);
}

const boot_uint8_t* e1000_address(void) { return mac; }

int e1000_send(const void* frame, boot_uint32_t length) {
    TX_DESCRIPTOR* descriptor;
    boot_uint64_t start;

    if (!ready || !length || length > FRAME_MAX) return 0;

    descriptor = &tx_ring[tx_next];
    /* The ring is eight deep and nothing here sends faster than it drains, so
       finding this descriptor still busy means the card has stopped rather
       than that we are ahead of it. */
    start = timer_ticks();
    while (!(descriptor->status & TX_STATUS_DONE)) {
        timer_poll();
        if (timer_expired(start, 1000)) {
            log("E1000: the transmit ring is not draining\n");
            return 0;
        }
    }

    memcpy(tx_buffers + tx_next * BUFFER_SIZE, frame, length);
    descriptor->length = (boot_uint16_t)length;
    descriptor->checksum_offset = 0;
    descriptor->checksum_start = 0;
    descriptor->special = 0;
    descriptor->status = 0;
    /* End of packet, add the frame check sequence, and report back when it has
       gone - that last one is what makes the done bit above mean anything. */
    descriptor->command = TX_CMD_END | TX_CMD_CRC | TX_CMD_REPORT;

    tx_next = (tx_next + 1) % TX_COUNT;
    write32(REG_TDT, tx_next);

    /* Waited for rather than left in flight. A DOS has one thing happening at
       a time, and knowing the frame is on the wire before returning makes
       every failure above here mean what it says. */
    start = timer_ticks();
    while (!(descriptor->status & TX_STATUS_DONE)) {
        timer_poll();
        if (timer_expired(start, 1000)) {
            /* The head pointer, sampled rather than read once.
             *
             * Everything known so far is its value seconds afterwards, which
             * cannot tell a card that never stirred from one that fetched the
             * descriptor, choked, and rolled back. Eight readings across the
             * first moments say which: all zero is a transmitter that is not
             * running, and anything else is one that is and is failing. */
            log("E1000: a frame was never sent. TDH");
            for (int sample = 0; sample < 8; sample++) {
                log(sample ? "," : " ");
                serial_write_dec(read32(REG_TDH));
                timer_wait(1);
            }
            log(" TDT ");
            serial_write_dec(read32(REG_TDT));
            log(" status ");
            serial_write_hex(descriptor->status);
            log("\n");
            return 0;
        }
    }
    frames_sent++;
    return 1;
}

boot_uint32_t e1000_receive(void* frame, boot_uint32_t size) {
    RX_DESCRIPTOR* descriptor = &rx_ring[rx_next];
    boot_uint32_t length;

    if (!ready) return 0;
    if (!(descriptor->status & RX_STATUS_DONE)) return 0;

    length = descriptor->length;
    if (!(descriptor->status & RX_STATUS_END) || descriptor->errors) {
        /* A frame split across descriptors, or one the card marked bad. The
           buffers are larger than any legal frame, so the first of those means
           something on the wire is wrong rather than that we are short of
           room. */
        frames_dropped++;
        length = 0;
    }
    if (length > size) length = size;
    if (length) {
        memcpy(frame, rx_buffers + rx_next * BUFFER_SIZE, length);
        frames_received++;
    }

    descriptor->status = 0;
    /* The tail is the last descriptor the card may use, so handing this one
       back means pointing at it - and the card resumes from the one after. */
    write32(REG_RDT, rx_next);
    rx_next = (rx_next + 1) % RX_COUNT;
    return length;
}

/* The card's own view of its two rings.
 *
 * "Nothing was sent" is what the driver believes, and the driver only knows
 * that a descriptor never came back marked done. The head pointers say
 * something it cannot: whether the card ever fetched the descriptor at all.
 *
 *   head == tail   the card has caught up - it took the descriptor and is
 *                  finished, so a missing done bit is a write-back problem
 *   head < tail    the descriptor is sitting there untouched, and the
 *                  transmitter is not running whatever the enable bit says
 */
void e1000_diagnose(void (*out)(const char*), void (*number)(boot_uint64_t)) {
    static const struct { const char* name; boot_uint32_t offset; } fields[] = {
        { "CTRL", REG_CTRL }, { "STATUS", REG_STATUS },
        { "TCTL", REG_TCTL }, { "RCTL", REG_RCTL },
        { "TDH", REG_TDH }, { "TDT", REG_TDT },
        { "RDH", REG_RDH }, { "RDT", REG_RDT },
        /* Read back rather than remembered: a base address the card did not
           keep is a different failure from a card that kept it and did
           nothing, and they look identical from here. */
        { "TDBAL", REG_TDBAL }, { "TDLEN", REG_TDLEN },
        { "TXDCTL", REG_TXDCTL }, { "TARC0", REG_TARC0 },
        { "TCTLEXT", REG_TCTL_EXT }
    };

    if (!ready) { out("no card"); return; }
    for (boot_uint32_t index = 0;
         index < sizeof(fields) / sizeof(fields[0]); index++) {
        if (index) out(" ");
        out(fields[index].name);
        out("=");
        number(read32(fields[index].offset));
    }
}

void e1000_counters(boot_uint32_t* sent, boot_uint32_t* received,
                    boot_uint32_t* dropped) {
    *sent = frames_sent;
    *received = frames_received;
    *dropped = frames_dropped;
}
