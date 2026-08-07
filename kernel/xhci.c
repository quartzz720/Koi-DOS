#include "xhci.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "block.h"

/* The USB 3 host controllers, and the two class drivers that sit on them.
 *
 * Three levels of state, each with its own lifetime, and keeping them apart is
 * the shape of this file. A machine has several controllers; a controller has
 * several devices; a device has several endpoints. Every one of those was a
 * file-scope variable at some point in this driver's life, and every one of
 * them turned out to be wrong on real hardware:
 *
 *   - One device at a time failed the moment a keyboard and a stick were
 *     plugged in together.
 *   - One controller at a time failed on the first real machine, which has two
 *     - a chipset one and a CPU one - with the keyboard on one and the stick
 *     on the other.
 *
 * xHCI fails quietly. A controller set up incorrectly does not complain; it
 * simply never posts an event, and every later step waits for something that
 * will not come. So each stage reports what it found rather than only whether
 * it succeeded, and the serial log is the record of how far enumeration got.
 */

/* Capability registers, at the start of the BAR. */
#define CAP_CAPLENGTH 0x00      /* byte: distance to the operational registers */
#define CAP_HCIVERSION 0x02     /* word */
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14          /* doorbell array offset */
#define CAP_RTSOFF 0x18         /* runtime register offset */

/* Operational registers, at CAPLENGTH from the base. */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(port) (0x400 + (port) * 0x10)

#define USBCMD_RUN 0x0001
#define USBCMD_RESET 0x0002
#define USBCMD_INTERRUPTER_ENABLE 0x0004

#define USBSTS_HALTED 0x0001
#define USBSTS_CONTROLLER_NOT_READY 0x0800

#define PORTSC_CONNECTED 0x00000001
#define PORTSC_ENABLED 0x00000002
#define PORTSC_RESET 0x00000010
#define PORTSC_POWER 0x00000200
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK 0x0F

/* PORTSC is a minefield of write-1-to-clear bits. Writing the value straight
   back would clear every change flag and - worse - bit 1 disables the port
   rather than reporting it. Any read-modify-write has to mask these out. */
#define PORTSC_CONNECT_CHANGE 0x00020000    /* something came or went */
#define PORTSC_CHANGE_BITS 0x00FE0000
#define PORTSC_WRITE_MASK (PORTSC_CHANGE_BITS | PORTSC_ENABLED)

#define SPEED_FULL 1
#define SPEED_LOW 2
#define SPEED_HIGH 3
#define SPEED_SUPER 4

/* Commands. */
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_RESET_ENDPOINT 14
#define TRB_SET_TR_DEQUEUE 16

/* The three stages a control transfer is built from, plus the plain data TRB
   that bulk and interrupt endpoints use. */
#define TRB_NORMAL 1
#define TRB_SETUP_STAGE 2
#define TRB_DATA_STAGE 3
#define TRB_STATUS_STAGE 4

#define TRB_IOC 0x20            /* interrupt on completion */
#define TRB_IMMEDIATE_DATA 0x40
#define TRB_DIRECTION_IN 0x10000

#define TRANSFER_TYPE_NO_DATA (0u << 16)
#define TRANSFER_TYPE_IN (3u << 16)

/* USB standard requests. */
#define USB_GET_DESCRIPTOR 6
#define USB_SET_CONFIGURATION 9
#define USB_CLEAR_FEATURE 1
#define USB_FEATURE_ENDPOINT_HALT 0
#define USB_DESCRIPTOR_DEVICE 1
#define USB_DESCRIPTOR_CONFIGURATION 2
#define USB_DESCRIPTOR_INTERFACE 4
#define USB_DESCRIPTOR_ENDPOINT 5

/* HID class, boot subclass, keyboard protocol - the combination every USB
   keyboard supports so that a BIOS can drive it without a report parser. */
#define USB_CLASS_HID 3
#define HID_SUBCLASS_BOOT 1
#define HID_PROTOCOL_KEYBOARD 1
#define HID_SET_PROTOCOL 0x0B
#define HID_PROTOCOL_BOOT 0

/* Mass storage: SCSI transparent command set over bulk-only transport. Every
   USB stick made in the last two decades speaks exactly this. */
#define USB_CLASS_MASS_STORAGE 8
#define MSC_SUBCLASS_SCSI 6
#define MSC_PROTOCOL_BULK_ONLY 0x50

#define ENDPOINT_TYPE_BULK_OUT 2
#define ENDPOINT_TYPE_BULK_IN 6
#define ENDPOINT_TYPE_INTERRUPT_IN 7

#define ENDPOINT_TRANSFER_BULK 2
#define ENDPOINT_TRANSFER_INTERRUPT 3

/* Runtime registers, at RTSOFF from the base. Interrupter 0 begins at 0x20. */
#define RT_INTERRUPTER0 0x20
#define IR_IMAN 0x00
#define IR_IMOD 0x04
#define IR_ERSTSZ 0x08
#define IR_ERSTBA 0x10
#define IR_ERDP 0x18

#define IMAN_INTERRUPT_PENDING 0x1
#define IMAN_INTERRUPT_ENABLE 0x2
#define ERDP_HANDLER_BUSY 0x8

/* TRB types we produce or consume. */
#define TRB_LINK 6
#define TRB_NO_OP_COMMAND 23
#define TRB_TRANSFER_EVENT 32
#define TRB_COMMAND_COMPLETION 33
#define TRB_PORT_STATUS_CHANGE 34

#define TRB_TYPE_SHIFT 10
#define TRB_CYCLE 0x1
#define TRB_TOGGLE_CYCLE 0x2

#define COMPLETION_SUCCESS 1
#define COMPLETION_STALL 6
#define COMPLETION_SHORT_PACKET 13

/* One 4 KiB page holds 256 TRBs, which is more depth than a system issuing
   one transfer at a time will ever need. */
#define RING_TRBS 256

/* Enough for the capability, operational, runtime and doorbell blocks of any
   controller worth supporting. The real size is in the BAR's mask bits, which
   would mean sizing the BAR; this is simpler and costs one 2 MiB page. */
#define XHCI_WINDOW_SIZE 0x10000

/* Extended capabilities, walked from HCCPARAMS1. */
#define ECAP_ID_LEGACY 1
#define LEGACY_BIOS_OWNED 0x00010000
#define LEGACY_OS_OWNED 0x01000000

/* How many plugged-in devices we drive per controller, and how many
   controllers in total. The first real machine had two - a chipset one with
   fourteen root ports and a CPU one - so one is definitively not enough. */
#define USB_MAX_DEVICES 8

/* Root-hub ports a controller may have. The field in HCSPARAMS1 is eight bits
   wide; nothing real has more than a couple of dozen, and the table this sizes
   is one word per port. */
#define XHCI_MAX_PORTS 64
#define XHCI_MAX_CONTROLLERS 4

/* A Transfer Request Block: the unit every xHCI ring is made of. */
typedef struct {
    boot_uint64_t parameter;
    boot_uint32_t status;
    boot_uint32_t control;
} TRB;

/* A ring, and where we are in it. The cycle bit is half the state: it is what
   distinguishes a TRB we have written from one the controller has already
   consumed, and it flips every time the ring wraps. */
typedef struct {
    TRB* trbs;
    boot_uint32_t index;
    boot_uint32_t cycle;
} RING;

struct XHCI_CONTROLLER;

/* One enumerated device: which controller it is on, its slot, where it is
   plugged in, and the ring its control endpoint runs on. Class drivers hang
   their own endpoints off it. */
typedef struct {
    struct XHCI_CONTROLLER* controller;
    int used;
    boot_uint32_t slot;
    boot_uint32_t port;
    boot_uint32_t speed;
    boot_uint32_t packet_size;   /* of endpoint zero */
    boot_uint8_t* context;       /* the output device context */
    RING control;
} USB_DEVICE;

/* One host controller. Everything here was file-scope until a machine with two
   of them proved that wrong. */
typedef struct XHCI_CONTROLLER {
    volatile boot_uint8_t* registers;   /* capability registers, the BAR base */
    volatile boot_uint8_t* operational;
    volatile boot_uint8_t* runtime;
    volatile boot_uint32_t* doorbells;

    boot_uint64_t* device_contexts;     /* DCBAA */
    RING command_ring;
    TRB* event_ring;
    boot_uint32_t event_index;
    boot_uint32_t event_cycle;          /* what the controller stamps for us */

    /* What each root-hub port had last time anyone looked. Hot-plug is the
       difference between this and what it has now: the change bit says
       something happened, and the connect bit says which way. */
    boot_uint32_t connected[XHCI_MAX_PORTS];

    boot_uint32_t port_count;
    boot_uint32_t slot_count;
    boot_uint32_t connected_ports;
    boot_uint32_t context_size;         /* 32 or 64 bytes, per HCCPARAMS1 */
    int running;

    USB_DEVICE devices[USB_MAX_DEVICES];
    boot_uint32_t device_count;
} XHCI_CONTROLLER;

static XHCI_CONTROLLER controllers[XHCI_MAX_CONTROLLERS];
static boot_uint32_t controller_count;

static boot_uint32_t read32(const XHCI_CONTROLLER* self, boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(self->registers + offset);
}

static void write32(const XHCI_CONTROLLER* self, boot_uint32_t offset,
                    boot_uint32_t value) {
    *(volatile boot_uint32_t*)(self->registers + offset) = value;
}

static boot_uint32_t op_read32(const XHCI_CONTROLLER* self,
                               boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(self->operational + offset);
}

static void op_write32(const XHCI_CONTROLLER* self, boot_uint32_t offset,
                       boot_uint32_t value) {
    *(volatile boot_uint32_t*)(self->operational + offset) = value;
}

static void log(const char* text) {
    serial_write(text);
}

static void log_hex(boot_uint64_t value) {
    serial_write_hex(value);
}

static void log_dec(boot_uint64_t value) {
    serial_write_dec(value);
}

/* Every log line says which controller it came from, because on a machine with
   two of them the same message from each is otherwise indistinguishable. */
static void log_controller(const XHCI_CONTROLLER* self) {
    log("XHCI");
    log_dec((boot_uint64_t)(self - controllers));
    log(": ");
}

/* Spin until a bit clears, giving up after `timeout_ms`.
   Returns 1 if it cleared, 0 on timeout. */
static int wait_clear(const XHCI_CONTROLLER* self, boot_uint32_t offset,
                      boot_uint32_t mask, boot_uint64_t timeout_ms) {
    boot_uint64_t start = timer_ticks();
    while (op_read32(self, offset) & mask) {
        timer_poll();
        if (timer_ticks() - start >= timeout_ms) return 0;
    }
    return 1;
}

/* Take the controller away from the firmware.
 *
 * A BIOS that provided legacy USB keyboard emulation still owns the controller
 * when we arrive, and will fight us for it through SMM. The handoff capability
 * is the defined way to ask for it: set the OS-owned bit, wait for the
 * BIOS-owned bit to clear. Skipping this is a classic way to end up with a
 * controller that behaves erratically for reasons invisible from the OS. */
static void claim_from_firmware(XHCI_CONTROLLER* self) {
    boot_uint32_t hccparams1 = read32(self, CAP_HCCPARAMS1);
    boot_uint32_t offset = (hccparams1 >> 16) & 0xFFFF;

    if (!offset) return;
    offset *= 4;   /* the field counts dwords from the base */

    for (int guard = 0; guard < 64 && offset; guard++) {
        boot_uint32_t capability = read32(self, offset);
        boot_uint32_t next = (capability >> 8) & 0xFF;

        if ((capability & 0xFF) == ECAP_ID_LEGACY) {
            if (capability & LEGACY_BIOS_OWNED) {
                boot_uint64_t start;
                log_controller(self);
                log("firmware owns the controller, asking for it\n");
                write32(self, offset, capability | LEGACY_OS_OWNED);
                start = timer_ticks();
                while (read32(self, offset) & LEGACY_BIOS_OWNED) {
                    timer_poll();
                    if (timer_ticks() - start >= 1000) {
                        /* Some firmware never lets go. Taking it anyway is
                           what every other OS does, and works. */
                        log_controller(self);
                        log("firmware did not release it, taking it\n");
                        break;
                    }
                }
            }
            /* Disable SMI generation so the firmware stops being notified
               about a controller it no longer drives. */
            write32(self, offset + 4, 0);
            return;
        }
        if (!next) return;
        offset += next * 4;
    }
}

static int reset_controller(XHCI_CONTROLLER* self) {
    /* Halt first. Resetting a running controller is undefined, and the
       firmware may well have left it running to service a legacy keyboard. */
    op_write32(self, OP_USBCMD, op_read32(self, OP_USBCMD) & ~USBCMD_RUN);
    {
        boot_uint64_t start = timer_ticks();
        while (!(op_read32(self, OP_USBSTS) & USBSTS_HALTED)) {
            timer_poll();
            if (timer_ticks() - start >= 1000) {
                log_controller(self);
                log("controller would not halt\n");
                return 0;
            }
        }
    }

    op_write32(self, OP_USBCMD, op_read32(self, OP_USBCMD) | USBCMD_RESET);
    /* The reset bit clears itself when the controller is done. */
    if (!wait_clear(self, OP_USBCMD, USBCMD_RESET, 1000)) {
        log_controller(self);
        log("reset did not complete\n");
        return 0;
    }
    /* And then it needs a moment more before its registers mean anything. */
    if (!wait_clear(self, OP_USBSTS, USBSTS_CONTROLLER_NOT_READY, 1000)) {
        log_controller(self);
        log("controller stayed not-ready after reset\n");
        return 0;
    }
    return 1;
}

/* Ring the doorbell for a slot. Slot 0 is the command ring; a device slot's
   doorbell carries the endpoint in its low byte. */
static void ring_doorbell(const XHCI_CONTROLLER* self, boot_uint32_t slot,
                          boot_uint32_t target) {
    self->doorbells[slot] = target;
    /* Read something back to push the write out ahead of anything that
       follows it. */
    (void)op_read32(self, OP_USBSTS);
}

/* ---- Rings -------------------------------------------------------------- */

/* Lay out a fresh ring in a page and close it into a loop.
 *
 * The last entry is a Link TRB pointing back at the start, with the toggle bit
 * set so that crossing it flips the cycle. That is what lets a ring be endless
 * without a separate head pointer: the boundary between our TRBs and stale
 * ones is wherever the cycle bit stops matching. */
static void ring_init(RING* ring, TRB* trbs) {
    TRB* link = &trbs[RING_TRBS - 1];

    memset(trbs, 0, PAGE_SIZE);
    ring->trbs = trbs;
    ring->index = 0;
    ring->cycle = 1;

    link->parameter = (boot_uint64_t)(unsigned long long)trbs;
    link->status = 0;
    link->control = (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE;
}

/* Put one TRB on a ring. Does not ring the doorbell: a control transfer builds
   three of these and only then tells the controller. */
static void ring_push(RING* ring, boot_uint64_t parameter, boot_uint32_t status,
                      boot_uint32_t control) {
    TRB* slot = &ring->trbs[ring->index];

    slot->parameter = parameter;
    slot->status = status;
    slot->control = (control & ~TRB_CYCLE) | ring->cycle;

    if (++ring->index == RING_TRBS - 1) {
        TRB* link = &ring->trbs[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | ring->cycle;
        ring->index = 0;
        ring->cycle ^= 1;
    }
}

/* Where the controller should resume from, in the form Set TR Dequeue Pointer
   wants: the address of the next TRB with the ring's cycle state in bit 0. */
static boot_uint64_t ring_position(const RING* ring) {
    return (boot_uint64_t)(unsigned long long)&ring->trbs[ring->index] |
           ring->cycle;
}

static void enqueue_command(XHCI_CONTROLLER* self, boot_uint64_t parameter,
                            boot_uint32_t status, boot_uint32_t control) {
    ring_push(&self->command_ring, parameter, status, control);
    ring_doorbell(self, 0, 0);
}

/* ---- Events ------------------------------------------------------------- */

/* Take the next event this controller has posted. Returns 0 on timeout; a
   timeout of zero makes this a non-blocking peek. */
static int next_event(XHCI_CONTROLLER* self, TRB* out,
                      boot_uint64_t timeout_ms) {
    boot_uint64_t start = timer_ticks();

    for (;;) {
        TRB* candidate = &self->event_ring[self->event_index];

        if ((candidate->control & TRB_CYCLE) == self->event_cycle) {
            *out = *candidate;
            if (++self->event_index == RING_TRBS) {
                self->event_index = 0;
                self->event_cycle ^= 1;
            }
            /* Tell the controller how far we have read, and clear the
               handler-busy bit while we are there. */
            {
                boot_uint64_t dequeue = (boot_uint64_t)(unsigned long long)
                    &self->event_ring[self->event_index];
                volatile boot_uint32_t* erdp = (volatile boot_uint32_t*)
                    (self->runtime + RT_INTERRUPTER0 + IR_ERDP);
                erdp[0] = (boot_uint32_t)dequeue | ERDP_HANDLER_BUSY;
                erdp[1] = (boot_uint32_t)(dequeue >> 32);
            }
            return 1;
        }
        timer_poll();
        if (timer_ticks() - start >= timeout_ms) return 0;
    }
}

static boot_uint32_t event_type(const TRB* event) {
    return (event->control >> TRB_TYPE_SHIFT) & 0x3F;
}

static boot_uint32_t event_slot(const TRB* event) {
    return (event->control >> 24) & 0xFF;
}

static boot_uint32_t event_endpoint(const TRB* event) {
    return (event->control >> 16) & 0x1F;
}

static int keyboard_event(const XHCI_CONTROLLER* self, const TRB* event);

/* Deal with an event that is not the one being waited for.
 *
 * The event ring is shared by everything a controller has to say, and it says
 * plenty. A port reset posts a Port Status Change; the keyboard posts a report
 * every time a key moves. Both can land in the middle of someone else's wait,
 * and a driver that took the first event to arrive would read a keypress as
 * the result of a disk command - the completion-code field sits in the same
 * bits for every event type, so it would even look like a success.
 *
 * Returns 1 when the event has been handled and the wait should carry on. */
static int service_event(const XHCI_CONTROLLER* self, const TRB* event) {
    switch (event_type(event)) {
    case TRB_PORT_STATUS_CHANGE:
        return 1;                    /* expected during enumeration */
    case TRB_TRANSFER_EVENT:
        return keyboard_event(self, event);
    default:
        return 0;
    }
}

static void log_unexpected(const XHCI_CONTROLLER* self, const TRB* event) {
    log_controller(self);
    log("ignoring event type ");
    log_dec(event_type(event));
    log(" from slot ");
    log_dec(event_slot(event));
    log("\n");
}

/* Wait specifically for a command to complete. */
static int wait_for_command(XHCI_CONTROLLER* self, TRB* out,
                            boot_uint64_t timeout_ms) {
    for (int guard = 0; guard < 64; guard++) {
        if (!next_event(self, out, timeout_ms)) return 0;
        if (event_type(out) == TRB_COMMAND_COMPLETION) return 1;
        if (!service_event(self, out)) log_unexpected(self, out);
    }
    return 0;
}

/* Wait for a transfer to finish on one particular endpoint of one particular
   device, servicing everything else that turns up meanwhile. */
static int wait_for_transfer(XHCI_CONTROLLER* self, boot_uint32_t slot,
                             boot_uint32_t dci, TRB* out,
                             boot_uint64_t timeout_ms) {
    for (int guard = 0; guard < 64; guard++) {
        if (!next_event(self, out, timeout_ms)) return 0;
        if (event_type(out) == TRB_TRANSFER_EVENT &&
            event_slot(out) == slot && event_endpoint(out) == dci)
            return 1;
        if (!service_event(self, out)) log_unexpected(self, out);
    }
    return 0;
}

static const char* completion_name(boot_uint32_t code) {
    switch (code) {
    case 1: return "success";
    case 4: return "USB transaction error";
    case 5: return "TRB error";
    case 6: return "stall";
    case 9: return "no slots available";
    case 11: return "slot not enabled";
    case 13: return "short packet";
    case 17: return "parameter error";
    case 19: return "context state error";
    default: return "other";
    }
}

/* Issue a command and report whether it succeeded, with the reason if not. */
static int run_command(XHCI_CONTROLLER* self, const char* what,
                       boot_uint64_t parameter, boot_uint32_t control) {
    TRB event;

    enqueue_command(self, parameter, 0, control);
    if (!wait_for_command(self, &event, 1000)) {
        log_controller(self);
        log(what);
        log(" produced no event\n");
        return 0;
    }
    if ((event.status >> 24) != COMPLETION_SUCCESS) {
        log_controller(self);
        log(what);
        log(" failed: ");
        log(completion_name(event.status >> 24));
        log("\n");
        return 0;
    }
    return 1;
}

/* ---- Controller bring-up ------------------------------------------------ */

/* Build the data structures the controller reads: the device context array,
   the command ring, and the event ring with its segment table. */
static int build_rings(XHCI_CONTROLLER* self) {
    boot_uint32_t hcsparams2 = read32(self, CAP_HCSPARAMS2);
    boot_uint32_t scratchpads = ((hcsparams2 >> 21) & 0x1F) << 5;
    boot_uint64_t address;
    TRB* command_trbs;

    scratchpads |= (hcsparams2 >> 27) & 0x1F;

    self->device_contexts = (boot_uint64_t*)alloc_page();
    command_trbs = (TRB*)alloc_page();
    self->event_ring = (TRB*)alloc_page();
    if (!self->device_contexts || !command_trbs || !self->event_ring) {
        log_controller(self);
        log("out of memory building rings\n");
        return 0;
    }
    memset(self->device_contexts, 0, PAGE_SIZE);
    memset(self->event_ring, 0, PAGE_SIZE);
    ring_init(&self->command_ring, command_trbs);

    /* Some controllers want scratch pages of their own to work in. Refusing to
       provide them is not an option - the controller simply will not run. */
    if (scratchpads) {
        boot_uint64_t* array = (boot_uint64_t*)alloc_page();
        if (!array) return 0;
        memset(array, 0, PAGE_SIZE);
        for (boot_uint32_t index = 0; index < scratchpads && index < 512; index++) {
            void* page = alloc_page();
            if (!page) return 0;
            memset(page, 0, PAGE_SIZE);
            array[index] = (boot_uint64_t)(unsigned long long)page;
        }
        self->device_contexts[0] = (boot_uint64_t)(unsigned long long)array;
        log_controller(self);
        log_dec(scratchpads);
        log(" scratchpad pages\n");
    }

    self->event_index = 0;
    self->event_cycle = 1;

    address = (boot_uint64_t)(unsigned long long)self->device_contexts;
    op_write32(self, OP_DCBAAP, (boot_uint32_t)address);
    op_write32(self, OP_DCBAAP + 4, (boot_uint32_t)(address >> 32));

    address = (boot_uint64_t)(unsigned long long)self->command_ring.trbs;
    /* Bit 0 is the ring cycle state the controller should start with. */
    op_write32(self, OP_CRCR, (boot_uint32_t)address | 1);
    op_write32(self, OP_CRCR + 4, (boot_uint32_t)(address >> 32));

    /* The event ring is described by a segment table rather than a link TRB,
       because the controller owns the wrap. One segment is plenty. */
    {
        boot_uint64_t* erst = (boot_uint64_t*)alloc_page();
        volatile boot_uint32_t* interrupter =
            (volatile boot_uint32_t*)(self->runtime + RT_INTERRUPTER0);
        boot_uint64_t erst_address;

        if (!erst) return 0;
        memset(erst, 0, PAGE_SIZE);
        erst[0] = (boot_uint64_t)(unsigned long long)self->event_ring;
        erst[1] = RING_TRBS;      /* segment size, in TRBs */

        erst_address = (boot_uint64_t)(unsigned long long)erst;
        address = (boot_uint64_t)(unsigned long long)self->event_ring;

        interrupter[IR_ERSTSZ / 4] = 1;
        interrupter[IR_ERDP / 4] = (boot_uint32_t)address | ERDP_HANDLER_BUSY;
        interrupter[IR_ERDP / 4 + 1] = (boot_uint32_t)(address >> 32);
        interrupter[IR_ERSTBA / 4] = (boot_uint32_t)erst_address;
        interrupter[IR_ERSTBA / 4 + 1] = (boot_uint32_t)(erst_address >> 32);
    }

    /* Announce how many device slots we intend to use. */
    op_write32(self, OP_CONFIG, self->slot_count);
    return 1;
}

static int start_controller(XHCI_CONTROLLER* self) {
    op_write32(self, OP_USBCMD, op_read32(self, OP_USBCMD) | USBCMD_RUN);
    {
        boot_uint64_t start = timer_ticks();
        while (op_read32(self, OP_USBSTS) & USBSTS_HALTED) {
            timer_poll();
            if (timer_ticks() - start >= 1000) {
                log_controller(self);
                log("controller would not start\n");
                return 0;
            }
        }
    }
    return 1;
}

/* The checkpoint that matters: send a command that does nothing, and see the
   controller acknowledge it. If this works, the ring layout, the doorbell, the
   cycle bits and the event ring are all correct - and every later failure is
   about USB rather than about the controller. */
static int noop_round_trip(XHCI_CONTROLLER* self) {
    TRB event;

    enqueue_command(self, 0, 0, TRB_NO_OP_COMMAND << TRB_TYPE_SHIFT);
    if (!wait_for_command(self, &event, 1000)) {
        log_controller(self);
        log("no-op produced no event - rings are not working\n");
        return 0;
    }
    {
        boot_uint32_t code = event.status >> 24;
        log_controller(self);
        log("no-op returned completion ");
        log(completion_name(code));
        log("\n");
        return code == COMPLETION_SUCCESS;
    }
}

/* ---- Enumeration -------------------------------------------------------- */

/* Reset a root-hub port and wait for it to come up enabled.
 *
 * A USB 2 device arrives connected but not enabled - the port sits in Polling
 * until it is reset, and only then does the device answer anything. USB 3
 * ports train themselves and arrive already enabled, so a reset there is
 * unnecessary but harmless. */
static int reset_port(XHCI_CONTROLLER* self, boot_uint32_t port) {
    boot_uint32_t status = op_read32(self, OP_PORTSC(port));
    boot_uint64_t start;

    if (status & PORTSC_ENABLED) return 1;   /* already trained */

    op_write32(self, OP_PORTSC(port),
               (status & ~PORTSC_WRITE_MASK) | PORTSC_RESET);

    start = timer_ticks();
    for (;;) {
        status = op_read32(self, OP_PORTSC(port));
        if (status & PORTSC_ENABLED) break;
        timer_poll();
        if (timer_ticks() - start >= 500) {
            log_controller(self);
            log("port reset timed out\n");
            return 0;
        }
    }
    /* Acknowledge the change flags the reset raised, or the controller keeps
       reporting them. */
    op_write32(self, OP_PORTSC(port),
               (status & ~PORTSC_WRITE_MASK) | (status & PORTSC_CHANGE_BITS));
    return 1;
}

/* Ask the controller for a device slot. Returns the slot number, or 0. */
static boot_uint32_t enable_slot(XHCI_CONTROLLER* self) {
    TRB event;

    enqueue_command(self, 0, 0, TRB_ENABLE_SLOT << TRB_TYPE_SHIFT);
    if (!wait_for_command(self, &event, 1000)) {
        log_controller(self);
        log("enable slot produced no event\n");
        return 0;
    }
    if ((event.status >> 24) != COMPLETION_SUCCESS) {
        log_controller(self);
        log("enable slot failed: ");
        log(completion_name(event.status >> 24));
        log("\n");
        return 0;
    }
    {
        boot_uint32_t slot = event_slot(&event);
        if (!slot) {
            log_controller(self);
            log("enable slot returned slot zero\n");
        }
        return slot;
    }
}

/* Give a slot back. A device that has been unplugged still owns one until the
   controller is told, and slots are a small fixed number. */
static void disable_slot(XHCI_CONTROLLER* self, boot_uint32_t slot) {
    TRB event;

    if (!slot) return;
    enqueue_command(self, 0, 0,
                    (TRB_DISABLE_SLOT << TRB_TYPE_SHIFT) | (slot << 24));
    if (!wait_for_command(self, &event, 1000)) {
        log_controller(self);
        log("disable slot produced no event\n");
        return;
    }
    self->device_contexts[slot] = 0;
}

/* The maximum packet size a control endpoint starts with, which depends only
   on the link speed until the device descriptor says otherwise. */
static boot_uint32_t default_packet_size(boot_uint32_t speed) {
    switch (speed) {
    case SPEED_LOW: return 8;
    case SPEED_FULL: return 8;    /* may turn out to be 16, 32 or 64 */
    case SPEED_HIGH: return 64;
    case SPEED_SUPER: return 512;
    default: return 8;
    }
}

/* The two context blocks the controller reads, addressed by device context
   index: endpoint zero is 1, and every other endpoint is number * 2 plus one
   for the IN direction. The input context has a control block in front of the
   slot context; the output one starts with the slot context itself. */
static boot_uint32_t* input_slot_context(const XHCI_CONTROLLER* self,
                                         boot_uint8_t* input) {
    return (boot_uint32_t*)(input + self->context_size);
}

static boot_uint32_t* input_endpoint_context(const XHCI_CONTROLLER* self,
                                             boot_uint8_t* input,
                                             boot_uint32_t dci) {
    return (boot_uint32_t*)(input + self->context_size * (dci + 1));
}

/* Fill in one endpoint context. `interval` is already in the controller's
   units - an exponent of 125 microsecond periods - and is ignored for bulk. */
static void describe_endpoint(const XHCI_CONTROLLER* self, boot_uint8_t* input,
                              boot_uint32_t dci, boot_uint32_t type,
                              boot_uint32_t packet, boot_uint32_t interval,
                              const RING* ring) {
    boot_uint32_t* context = input_endpoint_context(self, input, dci);
    boot_uint64_t address = (boot_uint64_t)(unsigned long long)ring->trbs;

    context[0] = interval << 16;
    /* Endpoint type, three retries before giving up, and the packet size. */
    context[1] = (type << 3) | (3u << 1) | (packet << 16);
    /* The dequeue pointer carries the ring's initial cycle state in bit 0. */
    context[2] = (boot_uint32_t)address | 1;
    context[3] = (boot_uint32_t)(address >> 32);
    /* Average TRB length, advisory. A periodic endpoint also declares how much
       it can move per service interval; a bulk one has no interval to speak
       of and leaves that half zero. */
    context[4] = packet;
    if (type == ENDPOINT_TYPE_INTERRUPT_IN) context[4] |= packet << 16;
}

/* Give the device on `port` an address, and leave its control endpoint ready
   for transfers. */
static int address_device(USB_DEVICE* device) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint8_t* input;
    boot_uint8_t* output;
    TRB* transfer_trbs;

    input = (boot_uint8_t*)alloc_page();
    output = (boot_uint8_t*)alloc_page();
    transfer_trbs = (TRB*)alloc_page();
    if (!input || !output || !transfer_trbs) {
        log_controller(self);
        log("out of memory addressing the device\n");
        return 0;
    }
    memset(input, 0, PAGE_SIZE);
    memset(output, 0, PAGE_SIZE);
    ring_init(&device->control, transfer_trbs);

    /* The input context tells the controller which parts of the device
       context we are supplying: bit 0 the slot, bit 1 endpoint zero. */
    ((boot_uint32_t*)input)[1] = 0x3;

    /* Route string zero (directly on a root port), one context entry, the
       link speed, and which root port it is on. */
    input_slot_context(self, input)[0] = (1u << 27) | (device->speed << 20);
    input_slot_context(self, input)[1] = (device->port + 1) << 16;

    device->packet_size = default_packet_size(device->speed);
    describe_endpoint(self, input, 1, 4 /* control */, device->packet_size, 0,
                      &device->control);

    device->context = output;
    self->device_contexts[device->slot] =
        (boot_uint64_t)(unsigned long long)output;

    if (!run_command(self, "address device",
                     (boot_uint64_t)(unsigned long long)input,
                     (TRB_ADDRESS_DEVICE << TRB_TYPE_SHIFT) |
                     (device->slot << 24)))
        return 0;

    /* The controller writes the assigned address into the output slot
       context; reading it back proves the device really answered. */
    log_controller(self);
    log("slot ");
    log_dec(device->slot);
    log(" addressed, device address ");
    log_dec(((boot_uint32_t*)output)[3] & 0xFF);
    log(", state ");
    log_dec(((boot_uint32_t*)output)[3] >> 27);
    log("\n");
    return 1;
}

/* Tell the controller that endpoint zero is bigger than we guessed.
 *
 * Only full-speed devices need this: the others have one legal packet size per
 * speed, but a full-speed device may use 8, 16, 32 or 64 and only says which
 * in the descriptor we had to read through the endpoint to get. Reading eight
 * bytes first is safe at any size, which is why enumeration does that. */
static int evaluate_packet_size(USB_DEVICE* device, boot_uint32_t packet) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint8_t* input = (boot_uint8_t*)alloc_page();
    boot_uint64_t position;
    int ok;

    if (!input) return 0;
    memset(input, 0, PAGE_SIZE);
    ((boot_uint32_t*)input)[1] = 0x2;      /* endpoint zero only */
    describe_endpoint(self, input, 1, 4 /* control */, packet, 0,
                      &device->control);
    /* Evaluate Context ignores the dequeue pointer, but leaving a stale one in
       a structure the controller reads is asking for trouble later. */
    position = ring_position(&device->control);
    input_endpoint_context(self, input, 1)[2] = (boot_uint32_t)position;
    input_endpoint_context(self, input, 1)[3] = (boot_uint32_t)(position >> 32);

    ok = run_command(self, "evaluate context",
                     (boot_uint64_t)(unsigned long long)input,
                     (TRB_EVALUATE_CONTEXT << TRB_TYPE_SHIFT) |
                     (device->slot << 24));
    free_page(input);
    if (ok) device->packet_size = packet;
    return ok;
}

/* A control transfer: ask the device for something and read the answer.
 *
 * Three TRBs rather than one, because USB control transfers have three phases
 * on the wire and the controller wants each spelled out. The setup packet
 * itself travels inside the first TRB rather than in a buffer - that is what
 * the immediate-data flag means - which saves an allocation for eight bytes.
 *
 * Returns the number of bytes received, or -1. */
static int control_in(USB_DEVICE* device, boot_uint8_t request_type,
                      boot_uint8_t request, boot_uint16_t value,
                      boot_uint16_t index, void* buffer, boot_uint16_t length) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint64_t setup;
    TRB event;
    boot_uint32_t code;

    /* The eight bytes of the setup packet, in the order USB puts them on the
       wire and therefore in the order the controller expects them packed. */
    setup = (boot_uint64_t)request_type |
            ((boot_uint64_t)request << 8) |
            ((boot_uint64_t)value << 16) |
            ((boot_uint64_t)index << 32) |
            ((boot_uint64_t)length << 48);

    ring_push(&device->control, setup, 8,
              (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IMMEDIATE_DATA |
              (length ? TRANSFER_TYPE_IN : TRANSFER_TYPE_NO_DATA));

    if (length) {
        ring_push(&device->control, (boot_uint64_t)(unsigned long long)buffer,
                  length, (TRB_DATA_STAGE << TRB_TYPE_SHIFT) | TRB_DIRECTION_IN);
    }

    /* The status stage runs the opposite way to the data, so an IN transfer
       ends with an OUT acknowledgement. Only this one asks for an event. */
    ring_push(&device->control, 0, 0,
              (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC |
              (length ? 0 : TRB_DIRECTION_IN));

    /* Endpoint zero is device context index 1. */
    ring_doorbell(self, device->slot, 1);

    if (!wait_for_transfer(self, device->slot, 1, &event, 1000)) {
        log_controller(self);
        log("control transfer produced no event\n");
        return -1;
    }
    code = event.status >> 24;
    if (code != COMPLETION_SUCCESS && code != COMPLETION_SHORT_PACKET) {
        log_controller(self);
        log("control transfer failed: ");
        log(completion_name(code));
        log("\n");
        return -1;
    }
    /* The status field carries how much was NOT transferred. */
    return (int)(length - (event.status & 0xFFFFFF));
}

/* Read the device descriptor and say what turned up. This is the first thing
   the device itself answers, as opposed to the controller answering for it. */
static int identify_device(USB_DEVICE* device) {
    boot_uint8_t* descriptor = (boot_uint8_t*)alloc_page();
    int got;

    if (!descriptor) return 0;
    memset(descriptor, 0, PAGE_SIZE);

    /* Eight bytes first. The eighth is the control endpoint's packet size, and
       until it is known a longer read could be split into packets the device
       does not use. */
    got = control_in(device, 0x80, USB_GET_DESCRIPTOR,
                     (USB_DESCRIPTOR_DEVICE << 8), 0, descriptor, 8);
    if (got < 8) {
        log_controller(device->controller);
        log("could not read the start of the device descriptor\n");
        free_page(descriptor);
        return 0;
    }
    if (descriptor[7] && descriptor[7] != device->packet_size &&
        device->speed != SPEED_SUPER) {
        log_controller(device->controller);
        log("endpoint zero is ");
        log_dec(descriptor[7]);
        log(" bytes, not ");
        log_dec(device->packet_size);
        log("\n");
        (void)evaluate_packet_size(device, descriptor[7]);
    }

    got = control_in(device, 0x80, USB_GET_DESCRIPTOR,
                     (USB_DESCRIPTOR_DEVICE << 8), 0, descriptor, 18);
    if (got < 18) {
        log_controller(device->controller);
        log("could not read the device descriptor\n");
        free_page(descriptor);
        return 0;
    }

    log_controller(device->controller);
    log("device ");
    log_hex((boot_uint32_t)descriptor[8] | ((boot_uint32_t)descriptor[9] << 8));
    log(":");
    log_hex((boot_uint32_t)descriptor[10] | ((boot_uint32_t)descriptor[11] << 8));
    log(" class ");
    log_dec(descriptor[4]);
    /* What we actually configured, not the descriptor byte: at SuperSpeed that
       byte is a power of two, and printing it raw reads as a 9-byte endpoint. */
    log(", packet size ");
    log_dec(device->packet_size);
    log(", ");
    log_dec(descriptor[17]);
    log(" configuration(s)\n");

    free_page(descriptor);
    return 1;
}

/* Read the whole configuration descriptor into `buffer`, which must be a page.
   Returns its length, or 0. */
static boot_uint32_t read_configuration(USB_DEVICE* device,
                                        boot_uint8_t* buffer) {
    boot_uint16_t total;
    int got;

    /* The first nine bytes say how long the whole thing is. */
    got = control_in(device, 0x80, USB_GET_DESCRIPTOR,
                     (USB_DESCRIPTOR_CONFIGURATION << 8), 0, buffer, 9);
    if (got < 9) return 0;
    total = (boot_uint16_t)(buffer[2] | (buffer[3] << 8));
    if (total > 1024) total = 1024;

    got = control_in(device, 0x80, USB_GET_DESCRIPTOR,
                     (USB_DESCRIPTOR_CONFIGURATION << 8), 0, buffer, total);
    if (got < total) return 0;
    return total;
}

/* Recover an endpoint the device has halted.
 *
 * A stall is the device saying no to one endpoint - an unsupported command, a
 * read past the end of the medium. The endpoint stays halted until both sides
 * agree it is clear, and both have to be told: the controller through Reset
 * Endpoint, then the device through CLEAR_FEATURE. The controller also has to
 * be pointed back at the ring, because the halted transfer left its dequeue
 * pointer parked on a TRB that will never complete. */
static int clear_stall(USB_DEVICE* device, boot_uint32_t dci, RING* ring,
                       boot_uint8_t endpoint_address) {
    XHCI_CONTROLLER* self = device->controller;

    if (!run_command(self, "reset endpoint", 0,
                     (TRB_RESET_ENDPOINT << TRB_TYPE_SHIFT) |
                     (dci << 16) | (device->slot << 24)))
        return 0;
    if (!run_command(self, "set dequeue pointer", ring_position(ring),
                     (TRB_SET_TR_DEQUEUE << TRB_TYPE_SHIFT) |
                     (dci << 16) | (device->slot << 24)))
        return 0;
    /* Endpoint recipient, feature zero: halt. */
    return control_in(device, 0x02, USB_CLEAR_FEATURE,
                      USB_FEATURE_ENDPOINT_HALT, endpoint_address, 0, 0) >= 0;
}

/* ---- HID boot keyboard --------------------------------------------------- */

static USB_DEVICE* keyboard_device;
static RING keyboard_ring;
static boot_uint32_t keyboard_dci;
static boot_uint8_t* report_buffer;
static boot_uint8_t previous_report[8];
static int keyboard_ready;

/* HID usage IDs to characters, unshifted then shifted. The boot protocol
   reports which physical key was pressed, not what it means, so this is the
   same job the PS/2 scancode table does - only the numbering differs. */
static const char hid_plain[0x40] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`',
    ',', '.', '/', 0, 0, 0, 0, 0, 0, 0
};

static const char hid_shifted[0x40] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~',
    '<', '>', '?', 0, 0, 0, 0, 0, 0, 0
};

static int hid_to_key(boot_uint8_t usage, boot_uint8_t modifiers) {
    int shift = (modifiers & 0x22) != 0;    /* either shift key */
    int control = (modifiers & 0x11) != 0;  /* either control key */
    char character;

    /* Arrows and the navigation block sit above the printable range. */
    switch (usage) {
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    case 0x4A: return KEY_HOME;
    case 0x4D: return KEY_END;
    case 0x4B: return KEY_PAGE_UP;
    case 0x4E: return KEY_PAGE_DOWN;
    case 0x4C: return KEY_DELETE;
    case 0x49: return KEY_INSERT;
    default: break;
    }
    if (usage >= 0x3A && usage <= 0x45) return KEY_F1 + (usage - 0x3A);
    if (usage >= sizeof(hid_plain)) return 0;

    character = shift ? hid_shifted[usage] : hid_plain[usage];
    if (!character) return 0;
    if (control && character >= 'a' && character <= 'z')
        character = (char)(character - 'a' + 1);
    else if (control && character >= 'A' && character <= 'Z')
        character = (char)(character - 'A' + 1);
    return (int)(unsigned char)character;
}

/* Queue a request for the next 8-byte report. The endpoint only speaks when
   asked, so one of these has to be outstanding at all times. */
static void queue_report_request(void) {
    ring_push(&keyboard_ring, (boot_uint64_t)(unsigned long long)report_buffer,
              8, (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
    ring_doorbell(keyboard_device->controller, keyboard_device->slot,
                  keyboard_dci);
}

/* Which key a usage is, ignoring what it would type. The event queue reports
   this so a key reads the same going down as coming up - see the note on
   scancode_identity in keyboard.c, which faces the same problem. */
static int hid_identity(boot_uint8_t usage) {
    switch (usage) {
    case 0x4F: return KOI_KEY_RIGHT;
    case 0x50: return KOI_KEY_LEFT;
    case 0x51: return KOI_KEY_DOWN;
    case 0x52: return KOI_KEY_UP;
    case 0x4A: return KOI_KEY_HOME;
    case 0x4D: return KOI_KEY_END;
    case 0x4B: return KOI_KEY_PAGE_UP;
    case 0x4E: return KOI_KEY_PAGE_DOWN;
    case 0x4C: return KOI_KEY_DELETE;
    case 0x49: return KOI_KEY_INSERT;
    default: break;
    }
    if (usage >= 0x3A && usage <= 0x45) return KOI_KEY_F1 + (usage - 0x3A);
    if (usage >= sizeof(hid_plain)) return 0;
    return (int)(unsigned char)hid_plain[usage];
}

/* Is a usage listed in a report? */
static int report_holds(const boot_uint8_t* report, boot_uint8_t usage) {
    for (int index = 2; index < 8; index++)
        if (report[index] == usage) return 1;
    return 0;
}

/* Turn one report into keystrokes and key events.
 *
 * A boot report is a set, not an event: it lists which keys are down right
 * now. A key counts as newly pressed when it is in this report and was not in
 * the last one, which is also how key repeat is avoided without any timing.
 * Comparing the other way round gives the releases, which the same comparison
 * has always been able to see and which used to be thrown away. */
static void handle_report(const boot_uint8_t* report) {
    boot_uint8_t modifiers = report[0];
    boot_uint8_t was = previous_report[0];
    static const struct { boot_uint8_t mask; int key; } modifier_keys[] = {
        { 0x22, KOI_KEY_SHIFT },     /* either shift */
        { 0x11, KOI_KEY_CONTROL },   /* either control */
        { 0x44, KOI_KEY_ALT }        /* either alt */
    };

    for (int index = 0; index < 3; index++) {
        int now_down = (modifiers & modifier_keys[index].mask) != 0;
        int was_down = (was & modifier_keys[index].mask) != 0;
        if (now_down != was_down)
            keyboard_submit_event(modifier_keys[index].key, !now_down);
    }

    for (int index = 2; index < 8; index++) {
        boot_uint8_t usage = report[index];

        if (!usage || usage == 1) continue;   /* 1 means too many keys at once */
        if (report_holds(previous_report, usage)) continue;

        keyboard_submit_event(hid_identity(usage), 0);
        keyboard_submit(hid_to_key(usage, modifiers));
    }

    for (int index = 2; index < 8; index++) {
        boot_uint8_t usage = previous_report[index];

        if (!usage || usage == 1) continue;
        if (report_holds(report, usage)) continue;
        keyboard_submit_event(hid_identity(usage), 1);
    }
    memcpy(previous_report, report, 8);
}

/* Is this transfer event the keyboard's? Called for every transfer event that
   somebody else was not waiting for, which is how keystrokes keep arriving
   while a disk transfer is in flight. The controller has to match too: slot
   numbers are per-controller, so slot 1 on one is a different device from
   slot 1 on the other. */
static int keyboard_event(const XHCI_CONTROLLER* self, const TRB* event) {
    if (!keyboard_ready) return 0;
    if (keyboard_device->controller != self) return 0;
    if (event_slot(event) != keyboard_device->slot) return 0;
    if (event_endpoint(event) != keyboard_dci) return 0;
    handle_report(report_buffer);
    queue_report_request();
    return 1;
}

void xhci_poll(void) {
    TRB event;

    /* Not conditional on a keyboard any more. The event rings have to be
       drained whatever is attached - an event nobody collects stalls the ring
       it is on - and this is also where a device that has just been plugged in
       gets noticed. */
    xhci_service();
    /* Every controller, not just the keyboard's: an event left unread stalls
       its own ring, and a transfer event nobody collects is a device that
       never speaks again. */
    for (boot_uint32_t index = 0; index < controller_count; index++) {
        XHCI_CONTROLLER* self = &controllers[index];
        if (!self->running) continue;
        while (next_event(self, &event, 0))
            if (!service_event(self, &event)) log_unexpected(self, &event);
    }
}

int xhci_has_keyboard(void) {
    return keyboard_ready;
}

/* Walk a configuration descriptor looking for a boot-protocol keyboard, and
   note where its interrupt endpoint is. */
static int find_keyboard(const boot_uint8_t* configuration, boot_uint32_t length,
                         boot_uint8_t* out_interface, boot_uint8_t* out_endpoint,
                         boot_uint16_t* out_packet, boot_uint8_t* out_interval) {
    boot_uint32_t offset = 0;
    int in_keyboard = 0;

    while (offset + 2 <= length) {
        boot_uint8_t size = configuration[offset];
        boot_uint8_t type = configuration[offset + 1];

        if (!size || offset + size > length) break;

        if (type == USB_DESCRIPTOR_INTERFACE && size >= 9) {
            in_keyboard = configuration[offset + 5] == USB_CLASS_HID &&
                          configuration[offset + 6] == HID_SUBCLASS_BOOT &&
                          configuration[offset + 7] == HID_PROTOCOL_KEYBOARD;
            if (in_keyboard) *out_interface = configuration[offset + 2];
        } else if (type == USB_DESCRIPTOR_ENDPOINT && size >= 7 && in_keyboard) {
            boot_uint8_t address = configuration[offset + 2];
            boot_uint8_t attributes = configuration[offset + 3];
            /* Direction IN, transfer type interrupt. */
            if ((address & 0x80) &&
                (attributes & 0x03) == ENDPOINT_TRANSFER_INTERRUPT) {
                *out_endpoint = address & 0x0F;
                *out_packet = (boot_uint16_t)(configuration[offset + 4] |
                                              (configuration[offset + 5] << 8));
                *out_interval = configuration[offset + 6];
                return 1;
            }
        }
        offset += size;
    }
    return 0;
}

/* The descriptor's interval is in frames for full speed and in 2^(n-1)
   microframes for high speed; the controller wants an exponent of 125
   microsecond units either way. */
static boot_uint32_t interval_exponent(boot_uint32_t speed,
                                       boot_uint8_t interval) {
    boot_uint32_t exponent;

    if (speed == SPEED_HIGH || speed == SPEED_SUPER)
        return interval ? (boot_uint32_t)(interval - 1) : 3;

    exponent = 3;
    while ((1u << exponent) < (boot_uint32_t)interval * 8u && exponent < 15)
        exponent++;
    return exponent;
}

static int configure_keyboard(USB_DEVICE* device,
                              const boot_uint8_t* configuration,
                              boot_uint32_t length) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint8_t* input;
    TRB* trbs;
    boot_uint8_t interface = 0;
    boot_uint8_t endpoint = 0;
    boot_uint16_t packet = 8;
    boot_uint8_t interval = 10;

    if (!find_keyboard(configuration, length, &interface, &endpoint,
                       &packet, &interval))
        return 0;

    if (keyboard_ready) {
        log_controller(self);
        log("a second keyboard is present and ignored\n");
        return 0;
    }

    log_controller(self);
    log("boot keyboard on interface ");
    log_dec(interface);
    log(", endpoint ");
    log_dec(endpoint);
    log(", packet ");
    log_dec(packet);
    log("\n");

    /* Endpoint 1 IN is device context index 3: two per endpoint number, plus
       one for the IN direction. */
    keyboard_dci = endpoint * 2 + 1;

    trbs = (TRB*)alloc_page();
    report_buffer = (boot_uint8_t*)alloc_page();
    input = (boot_uint8_t*)alloc_page();
    if (!trbs || !report_buffer || !input) return 0;
    memset(report_buffer, 0, PAGE_SIZE);
    memset(input, 0, PAGE_SIZE);
    ring_init(&keyboard_ring, trbs);

    /* Add the slot context and the new endpoint. The slot has to be included
       because its Context Entries field must grow to cover the new index. */
    ((boot_uint32_t*)input)[1] = 1u | (1u << keyboard_dci);
    input_slot_context(self, input)[0] =
        (keyboard_dci << 27) | (device->speed << 20);
    input_slot_context(self, input)[1] = (device->port + 1) << 16;
    describe_endpoint(self, input, keyboard_dci, ENDPOINT_TYPE_INTERRUPT_IN,
                      packet, interval_exponent(device->speed, interval),
                      &keyboard_ring);

    if (!run_command(self, "configure endpoint",
                     (boot_uint64_t)(unsigned long long)input,
                     (TRB_CONFIGURE_ENDPOINT << TRB_TYPE_SHIFT) |
                     (device->slot << 24)))
        return 0;

    /* Select the configuration, then ask for the boot protocol - without the
       second the device would send report-descriptor-defined data that needs
       a parser we do not have. */
    if (control_in(device, 0x00, USB_SET_CONFIGURATION, configuration[5],
                   0, 0, 0) < 0) {
        log_controller(self);
        log("set configuration failed\n");
        return 0;
    }
    if (control_in(device, 0x21, HID_SET_PROTOCOL, HID_PROTOCOL_BOOT,
                   interface, 0, 0) < 0) {
        log_controller(self);
        log("set boot protocol failed\n");
        return 0;
    }

    keyboard_device = device;
    keyboard_ready = 1;
    queue_report_request();
    log_controller(self);
    log("keyboard ready\n");
    return 1;
}

/* ---- Mass storage: bulk-only transport over SCSI ------------------------- */

#define CBW_SIGNATURE 0x43425355U      /* "USBC" */
#define CSW_SIGNATURE 0x53425355U      /* "USBS" */
#define CBW_DIRECTION_IN 0x80

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A

/* The command block, exactly as it goes out on the bulk OUT endpoint: 31
   bytes, little-endian, with the SCSI command descriptor block inside it. */
typedef struct __attribute__((packed)) {
    boot_uint32_t signature;
    boot_uint32_t tag;
    boot_uint32_t transfer_length;
    boot_uint8_t flags;
    boot_uint8_t lun;
    boot_uint8_t command_length;
    boot_uint8_t command[16];
} COMMAND_BLOCK;

/* And the 13 bytes that come back on the bulk IN endpoint afterwards. */
typedef struct __attribute__((packed)) {
    boot_uint32_t signature;
    boot_uint32_t tag;
    boot_uint32_t residue;
    boot_uint8_t status;
} STATUS_BLOCK;

static USB_DEVICE* storage_device;
static RING storage_in;
static RING storage_out;
static boot_uint32_t storage_in_dci;
static boot_uint32_t storage_out_dci;
static boot_uint8_t storage_in_address;    /* endpoint address, for CLEAR_FEATURE */
static boot_uint8_t storage_out_address;
static boot_uint8_t* storage_blocks;       /* the command and status blocks */
static boot_uint8_t* storage_bounce;       /* one page of transfer buffer */
static boot_uint32_t storage_tag;
static boot_uint32_t storage_sector_size;
static boot_uint64_t storage_sectors;
static int storage_ready;
/* Why the last transfer failed, as the device explained it. 0xFF when nothing
   has failed or the device would not say. */
static boot_uint8_t storage_last_sense = 0xFF;

/* One bulk transfer: a single Normal TRB, rung and waited for.
 *
 * The buffer is always one of ours and page-aligned, which sidesteps the rule
 * that a TRB's buffer may not cross a 64 KiB boundary - a page never does. The
 * block layer's buffers come from anywhere, so reads and writes bounce through
 * `storage_bounce` rather than being handed to the controller directly.
 *
 * Returns bytes transferred, -1 on failure, -2 on a stall the caller should
 * recover from. */
static int bulk_transfer(RING* ring, boot_uint32_t dci, void* buffer,
                         boot_uint32_t length, boot_uint64_t timeout_ms) {
    XHCI_CONTROLLER* self = storage_device->controller;
    TRB event;
    boot_uint32_t code;

    ring_push(ring, (boot_uint64_t)(unsigned long long)buffer, length,
              (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
    ring_doorbell(self, storage_device->slot, dci);

    if (!wait_for_transfer(self, storage_device->slot, dci, &event, timeout_ms)) {
        log_controller(self);
        log("bulk transfer produced no event\n");
        return -1;
    }
    code = event.status >> 24;
    if (code == COMPLETION_STALL) return -2;
    if (code != COMPLETION_SUCCESS && code != COMPLETION_SHORT_PACKET) {
        log_controller(self);
        log("bulk transfer failed: ");
        log(completion_name(code));
        log("\n");
        return -1;
    }
    return (int)(length - (event.status & 0xFFFFFF));
}

/* Run one SCSI command through the bulk-only transport.
 *
 * Three phases on the wire, in order: the command block out, an optional data
 * stage in whichever direction the command wants, and the status block in.
 * The tag ties the three together - the device echoes it back, and a status
 * block carrying somebody else's tag means the two ends have lost sync.
 *
 * Returns 1 when the device reports the command succeeded. */
static int scsi_command(const boot_uint8_t* command, boot_uint32_t command_length,
                        void* data, boot_uint32_t data_length, int data_in) {
    COMMAND_BLOCK* cbw = (COMMAND_BLOCK*)storage_blocks;
    STATUS_BLOCK* csw = (STATUS_BLOCK*)(storage_blocks + 64);
    int moved;

    if (!storage_device || command_length > 16) return 0;

    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    cbw->tag = ++storage_tag;
    cbw->transfer_length = data_length;
    cbw->flags = data_length && data_in ? CBW_DIRECTION_IN : 0;
    cbw->lun = 0;
    cbw->command_length = (boot_uint8_t)command_length;
    memcpy(cbw->command, command, command_length);

    moved = bulk_transfer(&storage_out, storage_out_dci, cbw,
                          (boot_uint32_t)sizeof(*cbw), 2000);
    if (moved != (int)sizeof(*cbw)) {
        if (moved == -2)
            (void)clear_stall(storage_device, storage_out_dci, &storage_out,
                              storage_out_address);
        return 0;
    }

    if (data_length) {
        RING* ring = data_in ? &storage_in : &storage_out;
        boot_uint32_t dci = data_in ? storage_in_dci : storage_out_dci;

        moved = bulk_transfer(ring, dci, data, data_length, 5000);
        if (moved == -2) {
            /* A stalled data stage is not fatal: the device still owes us a
               status block, and it will send one once the endpoint is clear. */
            if (!clear_stall(storage_device, dci, ring,
                             data_in ? storage_in_address : storage_out_address))
                return 0;
        } else if (moved < 0) {
            return 0;
        }
    }

    memset(csw, 0, sizeof(*csw));
    moved = bulk_transfer(&storage_in, storage_in_dci, csw,
                          (boot_uint32_t)sizeof(*csw), 2000);
    if (moved == -2) {
        if (!clear_stall(storage_device, storage_in_dci, &storage_in,
                         storage_in_address))
            return 0;
        moved = bulk_transfer(&storage_in, storage_in_dci, csw,
                              (boot_uint32_t)sizeof(*csw), 2000);
    }
    if (moved < (int)sizeof(*csw)) return 0;

    if (csw->signature != CSW_SIGNATURE) {
        log("XHCI: status block signature is wrong\n");
        return 0;
    }
    if (csw->tag != storage_tag) {
        log("XHCI: status block tag does not match\n");
        return 0;
    }
    return csw->status == 0;
}

/* What a sense key means, in the words the reader needs rather than a number.
   Only the ones a USB stick actually returns are worth naming. */
static const char* sense_meaning(boot_uint8_t key) {
    switch (key) {
    case 0x00: return "no sense";
    case 0x01: return "recovered error";
    case 0x02: return "not ready";
    case 0x03: return "medium error";
    case 0x04: return "hardware error";
    case 0x05: return "illegal request";
    case 0x06: return "unit attention - the medium may have changed";
    case 0x07: return "write protected";
    case 0x0B: return "aborted command";
    default: return "unknown";
    }
}

/* Ask the device why the last command failed.
 *
 * Two reasons to do this, and the second was learned the hard way. A device
 * holding a pending condition refuses every command until somebody reads it -
 * that is the "medium may have changed" handshake that makes a stick appear
 * dead if skipped. And the answer itself is the only account anyone gets of
 * why a transfer failed: without it a write to a write-protected stick and a
 * write to a dying one produce the same three words on screen.
 *
 * Returns the sense key, or 0xFF when the device would not even say. */
static boot_uint8_t request_sense(void) {
    boot_uint8_t command[6];
    boot_uint8_t key;

    memset(command, 0, sizeof(command));
    command[0] = SCSI_REQUEST_SENSE;
    command[4] = 18;
    if (!scsi_command(command, sizeof(command), storage_bounce, 18, 1))
        return 0xFF;

    key = (boot_uint8_t)(storage_bounce[2] & 0x0F);
    log("XHCI: storage says ");
    log(sense_meaning(key));
    log(" (key ");
    log_hex(key);
    log(", code ");
    log_hex(storage_bounce[12]);
    log("/");
    log_hex(storage_bounce[13]);
    log(")\n");
    return key;
}

/* Wait for the device to say it is ready. A stick that has just been given
   power spends a second or two spinning up its controller and answers "not
   ready" until it is done. */
static int wait_until_ready(void) {
    boot_uint8_t command[6];

    memset(command, 0, sizeof(command));
    command[0] = SCSI_TEST_UNIT_READY;

    for (int attempt = 0; attempt < 20; attempt++) {
        boot_uint64_t start;

        if (scsi_command(command, sizeof(command), 0, 0, 0)) return 1;
        (void)request_sense();
        start = timer_ticks();
        while (timer_ticks() - start < 100) timer_poll();
    }
    log("XHCI: the device never reported itself ready\n");
    return 0;
}

/* SCSI numbers are big-endian, which is the opposite of everything else in
   this file and the single most common way to misread a capacity. */
static boot_uint32_t read_big32(const boot_uint8_t* data) {
    return ((boot_uint32_t)data[0] << 24) | ((boot_uint32_t)data[1] << 16) |
           ((boot_uint32_t)data[2] << 8) | (boot_uint32_t)data[3];
}

static void write_big32(boot_uint8_t* data, boot_uint32_t value) {
    data[0] = (boot_uint8_t)(value >> 24);
    data[1] = (boot_uint8_t)(value >> 16);
    data[2] = (boot_uint8_t)(value >> 8);
    data[3] = (boot_uint8_t)value;
}

/* Print the vendor and product strings from an INQUIRY response. They are
   space-padded rather than terminated, so trailing blanks are dropped. */
static void log_padded(const boot_uint8_t* text, boot_uint32_t length) {
    char buffer[33];
    boot_uint32_t used = length < 32 ? length : 32;

    while (used && text[used - 1] == ' ') used--;
    for (boot_uint32_t index = 0; index < used; index++)
        buffer[index] = (char)text[index];
    buffer[used] = 0;
    log(buffer);
}

static int inquiry(void) {
    boot_uint8_t command[6];

    memset(command, 0, sizeof(command));
    command[0] = SCSI_INQUIRY;
    command[4] = 36;
    memset(storage_bounce, 0, 64);

    if (!scsi_command(command, sizeof(command), storage_bounce, 36, 1)) {
        log("XHCI: INQUIRY failed\n");
        return 0;
    }
    log_controller(storage_device->controller);
    log("storage is ");
    log_padded(storage_bounce + 8, 8);
    log(" ");
    log_padded(storage_bounce + 16, 16);
    log(" rev ");
    log_padded(storage_bounce + 32, 4);
    log("\n");
    return 1;
}

static int read_capacity(void) {
    boot_uint8_t command[10];
    boot_uint32_t last;

    memset(command, 0, sizeof(command));
    command[0] = SCSI_READ_CAPACITY_10;
    memset(storage_bounce, 0, 16);

    if (!scsi_command(command, sizeof(command), storage_bounce, 8, 1)) {
        log("XHCI: READ CAPACITY failed\n");
        return 0;
    }
    /* The first number is the address of the last block, not a count. */
    last = read_big32(storage_bounce);
    storage_sector_size = read_big32(storage_bounce + 4);
    storage_sectors = (boot_uint64_t)last + 1;

    if (!storage_sector_size || storage_sector_size > PAGE_SIZE ||
        (storage_sector_size & (storage_sector_size - 1))) {
        log("XHCI: sector size ");
        log_dec(storage_sector_size);
        log(" is not something this driver can address\n");
        return 0;
    }
    log_controller(storage_device->controller);
    log("storage ");
    log_dec(storage_sectors * storage_sector_size / 1024U / 1024U);
    log(" MB, ");
    log_dec(storage_sector_size);
    log("-byte sectors\n");
    return 1;
}

/* Read or write through the bounce buffer, one page at a time.
 *
 * Bouncing costs a copy per chunk, and buys two things: the controller only
 * ever sees a page-aligned buffer, and the block layer above may hand us a
 * pointer from anywhere - the stack included - without the driver having to
 * care whether it straddles a 64 KiB boundary. */
static int storage_transfer(boot_uint64_t lba, boot_uint32_t count,
                            void* buffer, int write) {
    boot_uint8_t* caller = (boot_uint8_t*)buffer;
    boot_uint32_t per_chunk;

    if (!storage_ready || !count || !buffer) return 0;
    if (storage_sectors && lba + count > storage_sectors) return 0;
    storage_last_sense = 0xFF;

    per_chunk = (boot_uint32_t)(PAGE_SIZE / storage_sector_size);
    while (count) {
        boot_uint32_t chunk = count < per_chunk ? count : per_chunk;
        boot_uint32_t bytes = chunk * storage_sector_size;
        boot_uint8_t command[10];

        memset(command, 0, sizeof(command));
        command[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        write_big32(command + 2, (boot_uint32_t)lba);
        command[7] = (boot_uint8_t)(chunk >> 8);
        command[8] = (boot_uint8_t)chunk;

        if (write) memcpy(storage_bounce, caller, bytes);
        if (!scsi_command(command, sizeof(command), storage_bounce, bytes, !write)) {
            /* Ask why before giving up. The answer goes to the log, and the
               sense key is kept so the shell can say something better than
               that a copy failed. */
            log(write ? "XHCI: write to sector " : "XHCI: read of sector ");
            log_hex(lba);
            log(" failed\n");
            storage_last_sense = request_sense();
            return 0;
        }
        if (!write) memcpy(caller, storage_bounce, bytes);

        caller += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 1;
}

static int storage_block_read(BLOCK_DEVICE* device, boot_uint64_t lba,
                              boot_uint32_t count, void* buffer) {
    (void)device;
    return storage_transfer(lba, count, buffer, 0);
}

static int storage_block_write(BLOCK_DEVICE* device, boot_uint64_t lba,
                               boot_uint32_t count, const void* buffer) {
    (void)device;
    return storage_transfer(lba, count, (void*)buffer, 1);
}

/* Walk a configuration descriptor looking for a bulk-only SCSI interface and
   its pair of bulk endpoints. */
static int find_storage(const boot_uint8_t* configuration, boot_uint32_t length,
                        boot_uint8_t* out_interface, boot_uint8_t* out_in,
                        boot_uint16_t* out_in_packet, boot_uint8_t* out_out,
                        boot_uint16_t* out_out_packet) {
    boot_uint32_t offset = 0;
    int in_storage = 0;
    int found_in = 0;
    int found_out = 0;

    while (offset + 2 <= length) {
        boot_uint8_t size = configuration[offset];
        boot_uint8_t type = configuration[offset + 1];

        if (!size || offset + size > length) break;

        if (type == USB_DESCRIPTOR_INTERFACE && size >= 9) {
            in_storage = configuration[offset + 5] == USB_CLASS_MASS_STORAGE &&
                         configuration[offset + 6] == MSC_SUBCLASS_SCSI &&
                         configuration[offset + 7] == MSC_PROTOCOL_BULK_ONLY;
            if (in_storage) *out_interface = configuration[offset + 2];
        } else if (type == USB_DESCRIPTOR_ENDPOINT && size >= 7 && in_storage) {
            boot_uint8_t address = configuration[offset + 2];
            boot_uint8_t attributes = configuration[offset + 3];
            boot_uint16_t packet =
                (boot_uint16_t)(configuration[offset + 4] |
                                (configuration[offset + 5] << 8));

            if ((attributes & 0x03) != ENDPOINT_TRANSFER_BULK) {
                offset += size;
                continue;
            }
            if (address & 0x80) {
                *out_in = address;
                *out_in_packet = packet;
                found_in = 1;
            } else {
                *out_out = address;
                *out_out_packet = packet;
                found_out = 1;
            }
            if (found_in && found_out) return 1;
        }
        offset += size;
    }
    return 0;
}

static int register_storage(void) {
    BLOCK_DEVICE device;

    memset(&device, 0, sizeof(device));
    device.name[0] = 'u'; device.name[1] = 's'; device.name[2] = 'b';
    device.name[3] = '0'; device.name[4] = 0;
    device.sector_size = storage_sector_size;
    device.sector_count = storage_sectors;
    device.read = storage_block_read;
    device.write = storage_block_write;
    return block_register(&device) >= 0;
}

static int configure_storage(USB_DEVICE* device,
                             const boot_uint8_t* configuration,
                             boot_uint32_t length) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint8_t* input;
    TRB* in_trbs;
    TRB* out_trbs;
    boot_uint8_t interface = 0;
    boot_uint16_t in_packet = 64;
    boot_uint16_t out_packet = 64;

    if (!find_storage(configuration, length, &interface, &storage_in_address,
                      &in_packet, &storage_out_address, &out_packet))
        return 0;

    if (storage_ready) {
        log_controller(self);
        log("a second storage device is present and ignored\n");
        return 0;
    }

    log_controller(self);
    log("mass storage on interface ");
    log_dec(interface);
    log(", endpoints in ");
    log_dec(storage_in_address & 0x0F);
    log(" out ");
    log_dec(storage_out_address & 0x0F);
    log("\n");

    storage_in_dci = (boot_uint32_t)(storage_in_address & 0x0F) * 2 + 1;
    storage_out_dci = (boot_uint32_t)(storage_out_address & 0x0F) * 2;

    in_trbs = (TRB*)alloc_page();
    out_trbs = (TRB*)alloc_page();
    storage_blocks = (boot_uint8_t*)alloc_page();
    storage_bounce = (boot_uint8_t*)alloc_page();
    input = (boot_uint8_t*)alloc_page();
    if (!in_trbs || !out_trbs || !storage_blocks || !storage_bounce || !input) {
        log_controller(self);
        log("out of memory configuring storage\n");
        return 0;
    }
    memset(storage_blocks, 0, PAGE_SIZE);
    memset(storage_bounce, 0, PAGE_SIZE);
    memset(input, 0, PAGE_SIZE);
    ring_init(&storage_in, in_trbs);
    ring_init(&storage_out, out_trbs);

    /* Both endpoints in one Configure Endpoint command, with the slot context
       stretched to cover whichever index is higher. */
    {
        boot_uint32_t last = storage_in_dci > storage_out_dci ? storage_in_dci
                                                             : storage_out_dci;
        ((boot_uint32_t*)input)[1] =
            1u | (1u << storage_in_dci) | (1u << storage_out_dci);
        input_slot_context(self, input)[0] = (last << 27) | (device->speed << 20);
        input_slot_context(self, input)[1] = (device->port + 1) << 16;
    }
    describe_endpoint(self, input, storage_in_dci, ENDPOINT_TYPE_BULK_IN,
                      in_packet, 0, &storage_in);
    describe_endpoint(self, input, storage_out_dci, ENDPOINT_TYPE_BULK_OUT,
                      out_packet, 0, &storage_out);

    if (!run_command(self, "configure endpoint",
                     (boot_uint64_t)(unsigned long long)input,
                     (TRB_CONFIGURE_ENDPOINT << TRB_TYPE_SHIFT) |
                     (device->slot << 24)))
        return 0;

    if (control_in(device, 0x00, USB_SET_CONFIGURATION, configuration[5],
                   0, 0, 0) < 0) {
        log_controller(self);
        log("set configuration failed\n");
        return 0;
    }

    storage_device = device;
    storage_tag = 0;
    if (!wait_until_ready()) { storage_device = 0; return 0; }
    if (!inquiry()) { storage_device = 0; return 0; }
    if (!read_capacity()) { storage_device = 0; return 0; }

    storage_ready = 1;
    if (!register_storage()) {
        log_controller(self);
        log("the block layer would not take the device\n");
        storage_ready = 0;
        storage_device = 0;
        return 0;
    }
    log_controller(self);
    log("storage ready\n");
    return 1;
}

int xhci_has_storage(void) {
    return storage_ready;
}

const char* xhci_storage_error(void) {
    if (storage_last_sense == 0xFF) return (const char*)0;
    return sense_meaning(storage_last_sense);
}

/* ---- USB networking, the part that comes before any protocol -------------
 *
 * A phone sharing its connection over USB presents one of a small number of
 * shapes, and they differ in the protocol rather than in the plumbing: every
 * one of them is a control endpoint for setup, two bulk endpoints for frames,
 * and usually an interrupt endpoint the device uses to say the link came up.
 *
 * This is the plumbing. It finds the interfaces, opens the endpoints and says
 * what it found - which is the first thing worth being sure of, because
 * "networking does not work" and "the phone is in charging mode" look
 * identical from here and only one of them is a bug.
 */

/* The classes worth recognising. RNDIS is Microsoft's, and is what an Android
   phone offers first because it is what Windows accepts without a driver; it
   is declared as a vendor-specific wireless controller rather than as
   networking, which is why it needs naming rather than deducing. */
#define USB_CLASS_COMMUNICATIONS 0x02
#define USB_CLASS_CDC_DATA 0x0A
#define USB_CLASS_WIRELESS 0xE0
#define USB_CLASS_MISCELLANEOUS 0xEF

#define CDC_SUBCLASS_ACM 0x02
#define CDC_SUBCLASS_ETHERNET 0x06
#define CDC_SUBCLASS_NCM 0x0D
#define CDC_PROTOCOL_VENDOR 0xFF
#define WIRELESS_SUBCLASS_RNDIS 0x01
#define WIRELESS_PROTOCOL_RNDIS 0x03
#define MISC_SUBCLASS_RNDIS 0x04
#define MISC_PROTOCOL_RNDIS 0x01

/* RNDIS is declared three different ways, and a device picks one.
 *
 * It is Microsoft's protocol and was never given a class of its own, so it
 * arrives disguised as something else: as a modem whose protocol field says
 * "vendor" - which is what QEMU's usb-net and most desktop dongles do - or as
 * a wireless controller, which is what Android phones do, or through the
 * miscellaneous class with an interface association. Matching only the one you
 * happen to have in front of you is how a driver works on the bench and not on
 * the thing it was written for. */
static int is_rndis(boot_uint8_t class_code, boot_uint8_t subclass,
                    boot_uint8_t protocol) {
    if (class_code == USB_CLASS_COMMUNICATIONS &&
        subclass == CDC_SUBCLASS_ACM && protocol == CDC_PROTOCOL_VENDOR)
        return 1;
    if (class_code == USB_CLASS_WIRELESS &&
        subclass == WIRELESS_SUBCLASS_RNDIS &&
        protocol == WIRELESS_PROTOCOL_RNDIS)
        return 1;
    if (class_code == USB_CLASS_MISCELLANEOUS &&
        subclass == MISC_SUBCLASS_RNDIS && protocol == MISC_PROTOCOL_RNDIS)
        return 1;
    return 0;
}

#define NETWORK_NONE 0
#define NETWORK_RNDIS 1
#define NETWORK_ECM 2

typedef struct {
    int kind;                    /* NETWORK_* */
    boot_uint8_t control;        /* the interface that takes the commands */
    boot_uint8_t data;           /* the one the frames go over */
    boot_uint8_t data_alternate;  /* which of its settings carries endpoints */
    boot_uint8_t in_address;
    boot_uint8_t out_address;
    boot_uint8_t notify_address; /* 0 when the device has no interrupt pipe */
    boot_uint16_t in_packet;
    boot_uint16_t out_packet;
    boot_uint16_t notify_packet;
} USB_NETWORK;

/* Walk the configuration once, collecting the pieces a network device is made
   of. Written as one pass rather than one pass per question because the
   descriptors only mean anything in order: an endpoint belongs to whichever
   interface was named last. */
static int find_network(const boot_uint8_t* configuration, boot_uint32_t length,
                        USB_NETWORK* found) {
    boot_uint32_t offset = 0;
    boot_uint8_t current = 0xFF;
    boot_uint8_t data_alternate = 0;
    int in_control = 0;
    int in_data = 0;

    memset(found, 0, sizeof(*found));

    while (offset + 2 <= length) {
        boot_uint8_t size = configuration[offset];
        boot_uint8_t type = configuration[offset + 1];

        if (!size || offset + size > length) break;

        if (type == USB_DESCRIPTOR_INTERFACE && size >= 9) {
            boot_uint8_t number = configuration[offset + 2];
            boot_uint8_t alternate = configuration[offset + 3];

            data_alternate = alternate;
            boot_uint8_t class_code = configuration[offset + 5];
            boot_uint8_t subclass = configuration[offset + 6];
            boot_uint8_t protocol = configuration[offset + 7];

            current = number;
            in_control = 0;
            in_data = 0;

            /* Alternate settings are not noise here, and skipping them is how
               this missed a device it was looking straight at.
             *
             * A CDC Ethernet data interface is *required* to offer setting 0
             * with no endpoints at all - that is how the standard says "idle,
             * using no bandwidth" - and to put the two bulk endpoints in
             * setting 1. Take only setting 0 and the device appears to have
             * described a network with no way to carry a frame. RNDIS puts
             * them in setting 0, so both have to be allowed and the one that
             * actually has endpoints remembered, because the interface has to
             * be switched to it before anything will move. */
            if (alternate && class_code != USB_CLASS_CDC_DATA) {
                offset += size;
                continue;
            }

            if (is_rndis(class_code, subclass, protocol)) {
                found->kind = NETWORK_RNDIS;
                found->control = number;
                in_control = 1;
            } else if (class_code == USB_CLASS_COMMUNICATIONS &&
                       (subclass == CDC_SUBCLASS_ETHERNET ||
                        subclass == CDC_SUBCLASS_NCM)) {
                if (!found->kind) found->kind = NETWORK_ECM;
                found->control = number;
                in_control = 1;
            } else if (class_code == USB_CLASS_CDC_DATA) {
                found->data = number;
                in_data = 1;
                /* Provisional: overwritten by whichever setting turns out to
                   carry the endpoints, which is the one below. */
                if (!found->in_address && !found->out_address)
                    found->data_alternate = alternate;
            }
        } else if (type == USB_DESCRIPTOR_ENDPOINT && size >= 7) {
            boot_uint8_t address = configuration[offset + 2];
            boot_uint8_t attributes = configuration[offset + 3];
            boot_uint16_t packet =
                (boot_uint16_t)(configuration[offset + 4] |
                                (configuration[offset + 5] << 8));

            (void)current;
            if (in_control && (attributes & 0x03) == ENDPOINT_TRANSFER_INTERRUPT) {
                found->notify_address = address;
                found->notify_packet = packet;
            } else if (in_data && (attributes & 0x03) == ENDPOINT_TRANSFER_BULK) {
                found->data_alternate = data_alternate;
                if (address & 0x80) {
                    found->in_address = address;
                    found->in_packet = packet;
                } else {
                    found->out_address = address;
                    found->out_packet = packet;
                }
            }
        }
        offset += size;
    }

    /* A device with no data endpoints is one that has described a network
       interface and offered no way to move a frame over it - which is exactly
       what a phone looks like before the user taps "USB tethering". */
    if (!found->kind) return 0;
    return found->in_address && found->out_address;
}

static const char* network_kind_name(int kind) {
    switch (kind) {
    case NETWORK_RNDIS: return "RNDIS";
    case NETWORK_ECM: return "CDC Ethernet";
    default: return "unknown";
    }
}

/* Report what is on the other end of the cable. Opening the endpoints and
   speaking the protocol come next; being certain which shape this device is,
   and that it has agreed to be a network at all, comes first. */
static int configure_network(USB_DEVICE* device,
                             const boot_uint8_t* configuration,
                             boot_uint32_t length) {
    XHCI_CONTROLLER* self = device->controller;
    USB_NETWORK network;

    if (!find_network(configuration, length, &network)) return 0;

    log_controller(self);
    log(network_kind_name(network.kind));
    log(" network device: control interface ");
    log_dec(network.control);
    log(", data interface ");
    log_dec(network.data);
    log(", endpoints in ");
    log_dec(network.in_address & 0x0F);
    log(" out ");
    log_dec(network.out_address & 0x0F);
    if (network.data_alternate) {
        log(" (setting ");
        log_dec(network.data_alternate);
        log(")");
    }
    if (network.notify_address) {
        log(", notify ");
        log_dec(network.notify_address & 0x0F);
    }
    log("\n");
    log_controller(self);
    log("no protocol for it yet - found, not driven\n");
    return 0;
}


/* ---- Bringing devices up ------------------------------------------------ */

/* Enumerate whatever is plugged into one port and hand it to a class driver.
   A device nobody claims is left addressed and idle rather than torn down:
   it costs one slot, and saying what it was is more useful than silence. */
static void attach_device(XHCI_CONTROLLER* self, boot_uint32_t port) {
    USB_DEVICE* device;
    boot_uint8_t* configuration;
    boot_uint32_t length;
    boot_uint32_t status;

    if (self->device_count >= USB_MAX_DEVICES) return;
    if (!reset_port(self, port)) return;

    device = &self->devices[self->device_count];
    memset(device, 0, sizeof(*device));
    status = op_read32(self, OP_PORTSC(port));
    device->controller = self;
    device->port = port;
    device->speed = (status >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
    device->slot = enable_slot(self);
    if (!device->slot) return;

    if (!address_device(device)) return;
    if (!identify_device(device)) return;

    configuration = (boot_uint8_t*)alloc_page();
    if (!configuration) return;
    memset(configuration, 0, PAGE_SIZE);
    length = read_configuration(device, configuration);
    if (!length) {
        log_controller(self);
        log("could not read the configuration descriptor\n");
        free_page(configuration);
        return;
    }

    device->used = 1;
    self->device_count++;

    if (!configure_keyboard(device, configuration, length) &&
        !configure_storage(device, configuration, length) &&
        !configure_network(device, configuration, length)) {
        log_controller(self);
        log("no driver for this device\n");
    }

    free_page(configuration);
}

/* Something was unplugged. Let go of it before anything tries to talk to it.
 *
 * A device that has gone still answers every register read with plausible
 * values, so nothing notices on its own: a transfer posted to a dead slot
 * simply never completes, and the caller waits out its timeout for as long as
 * the machine is on. */
static void detach_device(XHCI_CONTROLLER* self, boot_uint32_t port) {
    for (boot_uint32_t index = 0; index < self->device_count; index++) {
        USB_DEVICE* device = &self->devices[index];

        if (!device->used || device->port != port) continue;

        log_controller(self);
        log("port ");
        log_dec(port + 1);
        log(" was unplugged\n");

        if (device == storage_device) {
            storage_ready = 0;
            storage_device = (USB_DEVICE*)0;
            block_forget("usb0");
        }
        if (device == keyboard_device) {
            keyboard_ready = 0;
            keyboard_device = (USB_DEVICE*)0;
        }

        disable_slot(self, device->slot);
        device->used = 0;
        device->slot = 0;
    }
}

/* One pass over the root hub, acting on anything that has changed.
 *
 * Polled rather than driven by the port change event, because the controller's
 * interrupt is not wired to a handler: the events are collected anyway, and a
 * port that changed says so in its own register whether or not anybody read
 * the ring. */
void xhci_service(void) {
    for (boot_uint32_t index = 0; index < controller_count; index++) {
        XHCI_CONTROLLER* self = &controllers[index];

        if (!self->running) continue;
        for (boot_uint32_t port = 0; port < self->port_count; port++) {
            boot_uint32_t status = op_read32(self, OP_PORTSC(port));
            boot_uint32_t now = (status & PORTSC_CONNECTED) ? 1 : 0;

            /* Acknowledge the change first. Leaving it set means the next pass
               sees the same news again, and a port that is being plugged and
               unplugged faster than this runs would otherwise never settle. */
            if (status & PORTSC_CHANGE_BITS)
                op_write32(self, OP_PORTSC(port),
                           (status & ~PORTSC_WRITE_MASK) |
                           (status & PORTSC_CHANGE_BITS));

            if (now == self->connected[port]) continue;
            self->connected[port] = now;

            if (now) {
                log_controller(self);
                log("port ");
                log_dec(port + 1);
                log(" has a new device, speed ");
                log_dec((status >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);
                log("\n");
                attach_device(self, port);
            } else {
                detach_device(self, port);
            }
            self->connected_ports = 0;
            for (boot_uint32_t other = 0; other < self->port_count; other++)
                self->connected_ports += self->connected[other];
        }
    }
}

static void survey_ports(XHCI_CONTROLLER* self) {
    self->connected_ports = 0;
    for (boot_uint32_t port = 0; port < self->port_count; port++) {
        boot_uint32_t status = op_read32(self, OP_PORTSC(port));
        if (!(status & PORTSC_CONNECTED)) continue;
        self->connected[port] = 1;
        self->connected_ports++;
        log_controller(self);
        log("port ");
        log_dec(port + 1);
        log(" has a device, speed ");
        log_dec((status >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);
        log("\n");
        attach_device(self, port);
    }
}

int xhci_init(const PCI_DEVICE* controller) {
    XHCI_CONTROLLER* self;
    boot_uint64_t base;
    boot_uint32_t hcsparams1;
    boot_uint32_t capability_length;

    if (!controller) return 0;
    if (controller_count >= XHCI_MAX_CONTROLLERS) {
        log("XHCI: more controllers than this driver holds\n");
        return 0;
    }
    self = &controllers[controller_count];
    memset(self, 0, sizeof(*self));

    base = pci_bar_address(controller, 0);
    if (!base) {
        log("XHCI: BAR0 is not a memory window\n");
        return 0;
    }
    /* Before the first read. On QEMU the BAR lands at 768 GiB, far outside
       anything the boot-time identity map covers, and touching it unmapped is
       a page fault rather than a zero. */
    if (!paging_map_device(base, XHCI_WINDOW_SIZE)) {
        log("XHCI: could not map its register window\n");
        return 0;
    }
    pci_enable_bus_mastering(controller);
    self->registers = (volatile boot_uint8_t*)(unsigned long long)base;

    /* The slot is only claimed once the registers are reachable, so a
       controller that fails here does not consume one. */
    controller_count++;

    /* CAPLENGTH and HCIVERSION share the first dword. Read it as one: several
       controllers only answer 32-bit accesses to their register space, and a
       16-bit read of the version comes back as zero. */
    {
        boot_uint32_t first = read32(self, 0);
        capability_length = first & 0xFF;
        self->operational = self->registers + capability_length;

        log_controller(self);
        log("version ");
        log_hex(first >> 16);
        log(" at ");
        log_hex(base);
        log("\n");
    }

    claim_from_firmware(self);
    if (!reset_controller(self)) return 0;

    hcsparams1 = read32(self, CAP_HCSPARAMS1);
    self->slot_count = hcsparams1 & 0xFF;
    self->port_count = (hcsparams1 >> 24) & 0xFF;

    log_controller(self);
    log_dec(self->slot_count);
    log(" slots, ");
    log_dec(self->port_count);
    log(" ports\n");

    /* Bit 2 of HCCPARAMS1 doubles every context structure to 64 bytes. Getting
       this wrong misplaces every field the controller reads. */
    self->context_size = (read32(self, CAP_HCCPARAMS1) & 0x4) ? 64 : 32;

    self->runtime = self->registers + (read32(self, CAP_RTSOFF) & ~0x1FU);
    self->doorbells = (volatile boot_uint32_t*)
        (self->registers + (read32(self, CAP_DBOFF) & ~0x3U));

    if (!build_rings(self)) return 0;
    if (!start_controller(self)) return 0;
    if (!noop_round_trip(self)) return 0;

    survey_ports(self);
    self->running = 1;
    return 1;
}

boot_uint32_t xhci_controller_count(void) {
    return controller_count;
}

boot_uint32_t xhci_port_count(void) {
    boot_uint32_t total = 0;
    for (boot_uint32_t index = 0; index < controller_count; index++)
        if (controllers[index].running) total += controllers[index].port_count;
    return total;
}

boot_uint32_t xhci_ports_connected(void) {
    boot_uint32_t total = 0;
    for (boot_uint32_t index = 0; index < controller_count; index++)
        if (controllers[index].running)
            total += controllers[index].connected_ports;
    return total;
}
