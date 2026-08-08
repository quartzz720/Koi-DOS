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
#define TRANSFER_TYPE_OUT (2u << 16)

/* USB standard requests. */
#define USB_GET_DESCRIPTOR 6
#define USB_SET_CONFIGURATION 9
#define USB_CLEAR_FEATURE 1
#define USB_SET_INTERFACE 11
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
#define USB_MAX_DEVICES 32

/* Root-hub ports a controller may have. The field in HCSPARAMS1 is eight bits
   wide; nothing real has more than a couple of dozen, and the table this sizes
   is one word per port. */
#define XHCI_MAX_PORTS 64

/* Attempts on one port before it is left alone. Three is enough to ride out a
   device that is merely slow and few enough that a broken one does not drown
   everything else being said. */
#define PORT_REFUSAL_LIMIT 3
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
    boot_uint32_t port;          /* the root port the whole chain hangs off */
    boot_uint32_t speed;
    boot_uint32_t packet_size;   /* of endpoint zero */
    boot_uint8_t* context;       /* the output device context */
    RING control;

    /* Where it is, when it is not simply on a root port.
     *
     * The route string is how a host controller finds a device through a
     * chain of hubs: four bits per tier, the downstream port number at each
     * one, up to five tiers deep. Zero means the device is on the root port
     * itself, which is why everything worked before hubs existed.
     *
     * `tier` is how many hubs deep, and it is kept because the next nibble to
     * write is at four bits per tier and computing it from the route means
     * finding the highest non-zero nibble. */
    boot_uint32_t route;
    boot_uint32_t tier;

    /* A full or low speed device behind a high speed hub does not talk to the
       controller directly: the hub translates for it, and the controller has
       to be told which hub and which of its ports is doing the translating.
       Zero when the device speaks for itself. */
    boot_uint32_t tt_slot;
    boot_uint32_t tt_port;

    /* Set on a hub, with the number of downstream ports, because the
       controller schedules differently for something that has devices behind
       it. */
    int is_hub;
    boot_uint32_t hub_ports;
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
    /* How many times in a row a port has been noticed and then failed to come
       up. A port that flaps forever is a real thing - a phone with a tired
       socket connects, fails to reset, drops, and connects again - and without
       a limit it fills the log with the same four lines until the machine is
       switched off. */
    boot_uint32_t refusals[XHCI_MAX_PORTS];

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
static int network_event(const XHCI_CONTROLLER* self, const TRB* event);

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
        return keyboard_event(self, event) || network_event(self, event);
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

/* Fill in the slot context: where the device is and how to reach it.
 *
 * This was written out by hand in four places - addressing a device, and each
 * of the three class drivers opening its endpoints - and every one of them
 * assumed a device sitting on a root port, because that was the only kind
 * there was. A route string added in one of the four and forgotten in the
 * other three is a device that answers its descriptors and then goes silent
 * the moment a class driver configures it. So there is one of them now.
 *
 * `last_entry` is the highest device context index in use, which is what tells
 * the controller how much of the context to read. */
static void describe_slot(const XHCI_CONTROLLER* self, boot_uint8_t* input,
                          const USB_DEVICE* device, boot_uint32_t last_entry) {
    boot_uint32_t* slot = input_slot_context(self, input);

    slot[0] = (last_entry << 27) | (device->speed << 20) |
              (device->route & 0xFFFFF);
    if (device->is_hub) slot[0] |= 1u << 26;

    slot[1] = (device->port + 1) << 16;
    if (device->is_hub) slot[1] |= (device->hub_ports & 0xFF) << 24;

    /* Which hub is translating for a slow device on a fast bus, if any. */
    slot[2] = (slot[2] & 0xFFC00000u) |
              (device->tt_slot & 0xFF) | ((device->tt_port & 0xFF) << 8);
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

    /* One context entry: endpoint zero and nothing else yet. */
    describe_slot(self, input, device, 1);

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

/* The same, the other way: send the device something.
 *
 * A separate function rather than a flag on the one above, because every phase
 * changes direction with the transfer and a single function with three
 * conditionals in it reads worse than two that each say one thing. */
static int control_out(USB_DEVICE* device, boot_uint8_t request_type,
                       boot_uint8_t request, boot_uint16_t value,
                       boot_uint16_t index, const void* buffer,
                       boot_uint16_t length) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint64_t setup;
    TRB event;
    boot_uint32_t code;

    setup = (boot_uint64_t)request_type |
            ((boot_uint64_t)request << 8) |
            ((boot_uint64_t)value << 16) |
            ((boot_uint64_t)index << 32) |
            ((boot_uint64_t)length << 48);

    ring_push(&device->control, setup, 8,
              (TRB_SETUP_STAGE << TRB_TYPE_SHIFT) | TRB_IMMEDIATE_DATA |
              (length ? TRANSFER_TYPE_OUT : TRANSFER_TYPE_NO_DATA));

    if (length) {
        ring_push(&device->control, (boot_uint64_t)(unsigned long long)buffer,
                  length, (TRB_DATA_STAGE << TRB_TYPE_SHIFT));
    }

    /* An OUT transfer is acknowledged by an IN status stage. */
    ring_push(&device->control, 0, 0,
              (TRB_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC | TRB_DIRECTION_IN);

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
    return (int)length;
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

/* Keyboards, plural, and that is the whole point.
 *
 * This used to claim the first device with a boot-keyboard interface and log
 * "a second keyboard is present and ignored" for the rest. On a desk with one
 * keyboard that is fine. On a desk with a gaming mouse it is a disaster: those
 * mice carry a boot-keyboard interface of their own for their macro keys, they
 * enumerate before the keyboard does, and the driver ends up listening
 * intently to a mouse. Everything reports success, nothing is typed, and the
 * log says "keyboard ready" - which is true, and about the wrong device.
 *
 * So all of them are taken and all of them feed the same queue. Nothing here
 * has to decide which one somebody is going to type on, which is good, because
 * nothing here could. */
#define KEYBOARD_MAX 4

typedef struct {
    USB_DEVICE* device;
    RING ring;
    boot_uint32_t dci;
    boot_uint8_t* report;
    boot_uint8_t previous[8];
    int used;
} USB_KEYBOARD;

static USB_KEYBOARD keyboards[KEYBOARD_MAX];
static boot_uint32_t keyboard_count;

/* How many are actually here, which is not the same as how many entries have
   ever been used - the difference is a keyboard that has been unplugged. */
static boot_uint32_t keyboards_attached(void) {
    boot_uint32_t total = 0;
    for (boot_uint32_t index = 0; index < keyboard_count; index++)
        if (keyboards[index].used) total++;
    return total;
}

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
static void queue_report_request(USB_KEYBOARD* keyboard) {
    ring_push(&keyboard->ring, (boot_uint64_t)(unsigned long long)keyboard->report,
              8, (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
    ring_doorbell(keyboard->device->controller, keyboard->device->slot,
                  keyboard->dci);
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
static void handle_report(USB_KEYBOARD* keyboard, const boot_uint8_t* report) {
    boot_uint8_t modifiers = report[0];
    boot_uint8_t was = keyboard->previous[0];
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
        if (report_holds(keyboard->previous, usage)) continue;

        keyboard_submit_event(hid_identity(usage), 0);
        keyboard_submit(hid_to_key(usage, modifiers));
    }

    for (int index = 2; index < 8; index++) {
        boot_uint8_t usage = keyboard->previous[index];

        if (!usage || usage == 1) continue;
        if (report_holds(report, usage)) continue;
        keyboard_submit_event(hid_identity(usage), 1);
    }
    memcpy(keyboard->previous, report, 8);
}

/* Is this transfer event the keyboard's? Called for every transfer event that
   somebody else was not waiting for, which is how keystrokes keep arriving
   while a disk transfer is in flight. The controller has to match too: slot
   numbers are per-controller, so slot 1 on one is a different device from
   slot 1 on the other. */
static int keyboard_event(const XHCI_CONTROLLER* self, const TRB* event) {
    for (boot_uint32_t index = 0; index < keyboard_count; index++) {
        USB_KEYBOARD* keyboard = &keyboards[index];

        if (!keyboard->used) continue;
        if (keyboard->device->controller != self) continue;
        if (event_slot(event) != keyboard->device->slot) continue;
        if (event_endpoint(event) != keyboard->dci) continue;
        handle_report(keyboard, keyboard->report);
        queue_report_request(keyboard);
        return 1;
    }
    return 0;
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
    for (boot_uint32_t index = 0; index < keyboard_count; index++)
        if (keyboards[index].used) return 1;
    return 0;
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

    boot_uint32_t keyboard_slot;

    if (!find_keyboard(configuration, length, &interface, &endpoint,
                       &packet, &interval))
        return 0;

    {
        boot_uint32_t room = KEYBOARD_MAX;
        for (boot_uint32_t index = 0; index < KEYBOARD_MAX; index++)
            if (!keyboards[index].used) { room = index; break; }
        if (room == KEYBOARD_MAX) {
            log_controller(self);
            log("more keyboards than this can hold; ignoring one\n");
            return 0;
        }
        keyboard_slot = room;
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
    {
        USB_KEYBOARD* keyboard = &keyboards[keyboard_slot];
        memset(keyboard, 0, sizeof(*keyboard));
        keyboard->dci = endpoint * 2 + 1;

    trbs = (TRB*)alloc_page();
    keyboard->report = (boot_uint8_t*)alloc_page();
    input = (boot_uint8_t*)alloc_page();
    if (!trbs || !keyboard->report || !input) return 0;
    memset(keyboard->report, 0, PAGE_SIZE);
    memset(input, 0, PAGE_SIZE);
    ring_init(&keyboard->ring, trbs);

    /* Add the slot context and the new endpoint. The slot has to be included
       because its Context Entries field must grow to cover the new index. */
    ((boot_uint32_t*)input)[1] = 1u | (1u << keyboard->dci);
    describe_slot(self, input, device, keyboard->dci);
    describe_endpoint(self, input, keyboard->dci, ENDPOINT_TYPE_INTERRUPT_IN,
                      packet, interval_exponent(device->speed, interval),
                      &keyboard->ring);

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

        keyboard->device = device;
        keyboard->used = 1;
        if (keyboard_slot >= keyboard_count) keyboard_count = keyboard_slot + 1;
        queue_report_request(keyboard);
        log_controller(self);
        log("keyboard ready (");
        log_dec(keyboards_attached());
        log(" attached)\n");
    }
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

/* One USB stick. Plural, because a machine with two of them plugged in showed
   this driver one - and not the second one failing, the second one never being
   looked at: everything below was a file-scope singleton, so claiming a stick
   overwrote the one already claimed. */
#define USB_STORAGE_MAX 4

typedef struct {
    USB_DEVICE* device;
    RING in;
    RING out;
    boot_uint32_t in_dci;
    boot_uint32_t out_dci;
    boot_uint8_t in_address;     /* endpoint address, for CLEAR_FEATURE */
    boot_uint8_t out_address;
    boot_uint8_t* blocks;        /* the command and status blocks */
    boot_uint8_t* bounce;        /* one page of transfer buffer */
    boot_uint32_t tag;
    boot_uint32_t sector_size;
    boot_uint64_t sectors;
    int ready;
    /* Why the last transfer failed, as the device explained it. 0xFF when
       nothing has failed or the device would not say. */
    boot_uint8_t last_sense;
    char name[8];
    int used;
} USB_STORAGE;

static USB_STORAGE storages[USB_STORAGE_MAX];
static boot_uint32_t storage_count;

/* Which stick the SCSI helpers below are talking to.
 *
 * The same trade the NVMe driver makes and for the same reason: nothing here
 * is re-entrant and every completion is polled by whoever submitted it, so
 * there is one conversation at a time. Set at the two ways in - claiming a
 * device, and a block transfer arriving from above. */
static USB_STORAGE* storage;

/* One bulk transfer: a single Normal TRB, rung and waited for.
 *
 * The buffer is always one of ours and page-aligned, which sidesteps the rule
 * that a TRB's buffer may not cross a 64 KiB boundary - a page never does. The
 * block layer's buffers come from anywhere, so reads and writes bounce through
 * `storage->bounce` rather than being handed to the controller directly.
 *
 * Returns bytes transferred, -1 on failure, -2 on a stall the caller should
 * recover from. */
static int bulk_transfer(USB_DEVICE* device, RING* ring, boot_uint32_t dci,
                         void* buffer, boot_uint32_t length,
                         boot_uint64_t timeout_ms) {
    XHCI_CONTROLLER* self = device->controller;
    TRB event;
    boot_uint32_t code;

    ring_push(ring, (boot_uint64_t)(unsigned long long)buffer, length,
              (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
    ring_doorbell(self, device->slot, dci);

    if (!wait_for_transfer(self, device->slot, dci, &event, timeout_ms)) {
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
    COMMAND_BLOCK* cbw = (COMMAND_BLOCK*)storage->blocks;
    STATUS_BLOCK* csw = (STATUS_BLOCK*)(storage->blocks + 64);
    int moved;

    if (!storage->device || command_length > 16) return 0;

    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    cbw->tag = ++storage->tag;
    cbw->transfer_length = data_length;
    cbw->flags = data_length && data_in ? CBW_DIRECTION_IN : 0;
    cbw->lun = 0;
    cbw->command_length = (boot_uint8_t)command_length;
    memcpy(cbw->command, command, command_length);

    moved = bulk_transfer(storage->device, &storage->out, storage->out_dci, cbw,
                          (boot_uint32_t)sizeof(*cbw), 2000);
    if (moved != (int)sizeof(*cbw)) {
        if (moved == -2)
            (void)clear_stall(storage->device, storage->out_dci, &storage->out,
                              storage->out_address);
        return 0;
    }

    if (data_length) {
        RING* ring = data_in ? &storage->in : &storage->out;
        boot_uint32_t dci = data_in ? storage->in_dci : storage->out_dci;

        moved = bulk_transfer(storage->device, ring, dci, data, data_length, 5000);
        if (moved == -2) {
            /* A stalled data stage is not fatal: the device still owes us a
               status block, and it will send one once the endpoint is clear. */
            if (!clear_stall(storage->device, dci, ring,
                             data_in ? storage->in_address : storage->out_address))
                return 0;
        } else if (moved < 0) {
            return 0;
        }
    }

    memset(csw, 0, sizeof(*csw));
    moved = bulk_transfer(storage->device, &storage->in, storage->in_dci, csw,
                          (boot_uint32_t)sizeof(*csw), 2000);
    if (moved == -2) {
        if (!clear_stall(storage->device, storage->in_dci, &storage->in,
                         storage->in_address))
            return 0;
        moved = bulk_transfer(storage->device, &storage->in, storage->in_dci, csw,
                              (boot_uint32_t)sizeof(*csw), 2000);
    }
    if (moved < (int)sizeof(*csw)) return 0;

    if (csw->signature != CSW_SIGNATURE) {
        log("XHCI: status block signature is wrong\n");
        return 0;
    }
    if (csw->tag != storage->tag) {
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
    if (!scsi_command(command, sizeof(command), storage->bounce, 18, 1))
        return 0xFF;

    key = (boot_uint8_t)(storage->bounce[2] & 0x0F);
    log("XHCI: storage says ");
    log(sense_meaning(key));
    log(" (key ");
    log_hex(key);
    log(", code ");
    log_hex(storage->bounce[12]);
    log("/");
    log_hex(storage->bounce[13]);
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
    memset(storage->bounce, 0, 64);

    if (!scsi_command(command, sizeof(command), storage->bounce, 36, 1)) {
        log("XHCI: INQUIRY failed\n");
        return 0;
    }
    log_controller(storage->device->controller);
    log("storage is ");
    log_padded(storage->bounce + 8, 8);
    log(" ");
    log_padded(storage->bounce + 16, 16);
    log(" rev ");
    log_padded(storage->bounce + 32, 4);
    log("\n");
    return 1;
}

static int read_capacity(void) {
    boot_uint8_t command[10];
    boot_uint32_t last;

    memset(command, 0, sizeof(command));
    command[0] = SCSI_READ_CAPACITY_10;
    memset(storage->bounce, 0, 16);

    if (!scsi_command(command, sizeof(command), storage->bounce, 8, 1)) {
        log("XHCI: READ CAPACITY failed\n");
        return 0;
    }
    /* The first number is the address of the last block, not a count. */
    last = read_big32(storage->bounce);
    storage->sector_size = read_big32(storage->bounce + 4);
    storage->sectors = (boot_uint64_t)last + 1;

    if (!storage->sector_size || storage->sector_size > PAGE_SIZE ||
        (storage->sector_size & (storage->sector_size - 1))) {
        log("XHCI: sector size ");
        log_dec(storage->sector_size);
        log(" is not something this driver can address\n");
        return 0;
    }
    log_controller(storage->device->controller);
    log("storage ");
    log_dec(storage->sectors * storage->sector_size / 1024U / 1024U);
    log(" MB, ");
    log_dec(storage->sector_size);
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

    if (!storage->ready || !count || !buffer) return 0;
    if (storage->sectors && lba + count > storage->sectors) return 0;
    storage->last_sense = 0xFF;

    per_chunk = (boot_uint32_t)(PAGE_SIZE / storage->sector_size);
    while (count) {
        boot_uint32_t chunk = count < per_chunk ? count : per_chunk;
        boot_uint32_t bytes = chunk * storage->sector_size;
        boot_uint8_t command[10];

        memset(command, 0, sizeof(command));
        command[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        write_big32(command + 2, (boot_uint32_t)lba);
        command[7] = (boot_uint8_t)(chunk >> 8);
        command[8] = (boot_uint8_t)chunk;

        if (write) memcpy(storage->bounce, caller, bytes);
        if (!scsi_command(command, sizeof(command), storage->bounce, bytes, !write)) {
            /* Ask why before giving up. The answer goes to the log, and the
               sense key is kept so the shell can say something better than
               that a copy failed. */
            log(write ? "XHCI: write to sector " : "XHCI: read of sector ");
            log_hex(lba);
            log(" failed\n");
            storage->last_sense = request_sense();
            return 0;
        }
        if (!write) memcpy(caller, storage->bounce, bytes);

        caller += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 1;
}

/* The two ways in from above, and the only places the current stick is
   chosen. */
static int storage_block_read(BLOCK_DEVICE* device, boot_uint64_t lba,
                              boot_uint32_t count, void* buffer) {
    storage = (USB_STORAGE*)device->driver_data;
    if (!storage || !storage->used) return 0;
    return storage_transfer(lba, count, buffer, 0);
}

static int storage_block_write(BLOCK_DEVICE* device, boot_uint64_t lba,
                               boot_uint32_t count, const void* buffer) {
    storage = (USB_STORAGE*)device->driver_data;
    if (!storage || !storage->used) return 0;
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

static int register_storage(boot_uint32_t number) {
    BLOCK_DEVICE device;

    storage->name[0] = 'u'; storage->name[1] = 's'; storage->name[2] = 'b';
    storage->name[3] = (char)('0' + (number % 10));
    storage->name[4] = 0;

    memset(&device, 0, sizeof(device));
    memcpy(device.name, storage->name, 5);
    device.sector_size = storage->sector_size;
    device.sector_count = storage->sectors;
    device.driver_data = storage;
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
    boot_uint8_t in_address = 0;
    boot_uint8_t out_address = 0;

    if (!find_storage(configuration, length, &interface, &in_address,
                      &in_packet, &out_address, &out_packet))
        return 0;

    /* Somewhere to put it. This used to say "a second storage device is
       present and ignored" and mean it, which is a strange thing for an
       operating system to say to somebody holding two USB sticks. */
    storage = (USB_STORAGE*)0;
    for (boot_uint32_t index = 0; index < USB_STORAGE_MAX; index++)
        if (!storages[index].used) { storage = &storages[index]; break; }
    if (!storage) {
        log_controller(self);
        log("more storage devices than this can hold; ignoring one\n");
        return 0;
    }
    memset(storage, 0, sizeof(*storage));
    storage->in_address = in_address;
    storage->out_address = out_address;

    log_controller(self);
    log("mass storage on interface ");
    log_dec(interface);
    log(", endpoints in ");
    log_dec(storage->in_address & 0x0F);
    log(" out ");
    log_dec(storage->out_address & 0x0F);
    log("\n");

    storage->in_dci = (boot_uint32_t)(storage->in_address & 0x0F) * 2 + 1;
    storage->out_dci = (boot_uint32_t)(storage->out_address & 0x0F) * 2;

    in_trbs = (TRB*)alloc_page();
    out_trbs = (TRB*)alloc_page();
    storage->blocks = (boot_uint8_t*)alloc_page();
    storage->bounce = (boot_uint8_t*)alloc_page();
    input = (boot_uint8_t*)alloc_page();
    if (!in_trbs || !out_trbs || !storage->blocks || !storage->bounce || !input) {
        log_controller(self);
        log("out of memory configuring storage\n");
        return 0;
    }
    memset(storage->blocks, 0, PAGE_SIZE);
    memset(storage->bounce, 0, PAGE_SIZE);
    memset(input, 0, PAGE_SIZE);
    ring_init(&storage->in, in_trbs);
    ring_init(&storage->out, out_trbs);

    /* Both endpoints in one Configure Endpoint command, with the slot context
       stretched to cover whichever index is higher. */
    {
        boot_uint32_t last = storage->in_dci > storage->out_dci ? storage->in_dci
                                                             : storage->out_dci;
        ((boot_uint32_t*)input)[1] =
            1u | (1u << storage->in_dci) | (1u << storage->out_dci);
        describe_slot(self, input, device, last);
    }
    describe_endpoint(self, input, storage->in_dci, ENDPOINT_TYPE_BULK_IN,
                      in_packet, 0, &storage->in);
    describe_endpoint(self, input, storage->out_dci, ENDPOINT_TYPE_BULK_OUT,
                      out_packet, 0, &storage->out);

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

    storage->device = device;
    storage->tag = 0;
    storage->last_sense = 0xFF;
    if (!wait_until_ready()) { storage->device = 0; return 0; }
    if (!inquiry()) { storage->device = 0; return 0; }
    if (!read_capacity()) { storage->device = 0; return 0; }

    storage->ready = 1;
    if (!register_storage((boot_uint32_t)(storage - storages))) {
        log_controller(self);
        log("the block layer would not take the device\n");
        storage->ready = 0;
        storage->device = 0;
        return 0;
    }
    storage->used = 1;
    if ((boot_uint32_t)(storage - storages) >= storage_count)
        storage_count = (boot_uint32_t)(storage - storages) + 1;
    log_controller(self);
    log("storage ready\n");
    /* And the volume table is out of date the moment this line is printed. */
    block_changed();
    return 1;
}

int xhci_has_storage(void) {
    for (boot_uint32_t index = 0; index < storage_count; index++)
        if (storages[index].used && storages[index].ready) return 1;
    return 0;
}

/* The most recent explanation from any of them. One message for several sticks
   is a compromise, and the alternative - a command that takes which stick it
   means - is not worth it while "copy failed" is the thing being improved on. */
const char* xhci_storage_error(void) {
    for (boot_uint32_t index = 0; index < storage_count; index++)
        if (storages[index].used && storages[index].last_sense != 0xFF)
            return sense_meaning(storages[index].last_sense);
    return (const char*)0;
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

/* The device the frames will go over. One, for now: a machine with two phones
   plugged into it is a problem for later and a rare one. */
static USB_DEVICE* network_device;
static RING network_in;
static RING network_out;
static boot_uint32_t network_in_dci;
static boot_uint32_t network_out_dci;
static boot_uint32_t network_frame_limit;
static boot_uint8_t network_in_address;
static boot_uint8_t network_out_address;
static boot_uint16_t network_out_packet;
static boot_uint8_t network_mac[6];
static int network_ready;

/* The two buffers the controller does DMA into and out of. One page each:
   pages are what this kernel allocates, and a TRB's buffer may not cross a
   64 KiB boundary, which a single page never does. */
static boot_uint8_t* network_receive_buffer;
static boot_uint8_t* network_send_buffer;

/* Frames that have arrived and nobody has asked for yet.
 *
 * A queue rather than a single buffer, because the receive TRB has to be
 * re-armed the instant it completes - an endpoint with nothing outstanding is
 * a device the host has stopped listening to - and re-arming overwrites the
 * page. So the frames are copied out here, where they can wait for whatever
 * asks. RNDIS batches several into one transfer, so one completion can fill
 * several of these. */
#define NETWORK_FRAME_MAX 1514
#define NETWORK_QUEUE 16
static boot_uint8_t network_queue[NETWORK_QUEUE][NETWORK_FRAME_MAX];
static boot_uint16_t network_queue_length[NETWORK_QUEUE];
static boot_uint32_t network_queue_head;
static boot_uint32_t network_queue_tail;
static boot_uint32_t network_dropped;
/* Frames that went out, frames that came in, and sends the controller refused.
 *
 * "Nobody offered an address" is one sentence for three completely different
 * failures: nothing was sent, something was sent and nothing came back, or
 * things came back and none of them was an answer. Without these it takes a
 * machine and an afternoon to tell them apart; with them it takes a line. */
static boot_uint32_t network_sent;
static boot_uint32_t network_received;
static boot_uint32_t network_send_failed;
static boot_uint32_t network_receive_errors;

static void arm_network_receive(void);

static const char* network_kind_name(int kind) {
    switch (kind) {
    case NETWORK_RNDIS: return "RNDIS";
    case NETWORK_ECM: return "CDC Ethernet";
    default: return "unknown";
    }
}

/* ---- RNDIS, as far as asking the device who it is -------------------------
 *
 * RNDIS is a remote procedure call dressed as a network protocol: messages go
 * to the device inside a control transfer, and the answer comes back from a
 * second control transfer that has to be asked for separately. Every message
 * carries a request id, and the reply carries it back - which is the only way
 * to know that an answer belongs to the question just asked rather than to the
 * one before it.
 */
#define RNDIS_SEND_COMMAND 0x00         /* class request, host to device */
#define RNDIS_GET_RESPONSE 0x01

#define RNDIS_INITIALIZE 0x00000002U
#define RNDIS_QUERY 0x00000004U
#define RNDIS_SET 0x00000005U
#define RNDIS_COMPLETE 0x80000000U      /* set in the reply to any of them */

#define RNDIS_STATUS_SUCCESS 0x00000000U

/* The message the device sends without being asked, and the two things it
   says with it. A link that is down is not a broken driver: it is a phone
   that has not finished bringing tethering up, and the difference is worth
   being able to read. */
#define RNDIS_INDICATE_STATUS 0x00000007U
#define RNDIS_KEEPALIVE 0x00000008U
#define RNDIS_STATUS_MEDIA_CONNECT 0x4001000BU
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000CU

#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010EU
#define OID_802_3_PERMANENT_ADDRESS 0x01010101U

/* Directed, multicast and broadcast. Without a filter the device is entitled
   to hand over nothing at all, which looks exactly like a cable that is not
   plugged in. */
#define RNDIS_PACKET_FILTER 0x0000000FU

/* One message buffer, reused: RNDIS is strictly one exchange at a time. */
static boot_uint32_t rndis_request_id = 1;

/* Two hex digits, always. A hardware address written with the leading zeroes
   dropped is not a hardware address, it is a puzzle. */
static void log_hex_byte(boot_uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[3];
    text[0] = digits[(value >> 4) & 0xF];
    text[1] = digits[value & 0xF];
    text[2] = 0;
    log(text);
}

static void put32(boot_uint8_t* at, boot_uint32_t value) {
    at[0] = (boot_uint8_t)value;
    at[1] = (boot_uint8_t)(value >> 8);
    at[2] = (boot_uint8_t)(value >> 16);
    at[3] = (boot_uint8_t)(value >> 24);
}

static boot_uint32_t get32(const boot_uint8_t* at) {
    return (boot_uint32_t)at[0] | ((boot_uint32_t)at[1] << 8) |
           ((boot_uint32_t)at[2] << 16) | ((boot_uint32_t)at[3] << 24);
}

/* Send one message and collect its reply.
 *
 * The reply is not offered; it has to be fetched, and a device that has not
 * finished thinking answers the fetch with nothing. Retried a few times rather
 * than once, because "not yet" and "never" are the same answer here and only
 * waiting tells them apart. */
static int rndis_exchange(USB_DEVICE* device, boot_uint8_t interface,
                          boot_uint8_t* message, boot_uint32_t length,
                          boot_uint8_t* reply, boot_uint32_t reply_size) {
    int got = -1;

    if (control_out(device, 0x21, RNDIS_SEND_COMMAND, 0, interface,
                    message, (boot_uint16_t)length) < 0)
        return -1;

    for (int attempt = 0; attempt < 10; attempt++) {
        got = control_in(device, 0xA1, RNDIS_GET_RESPONSE, 0, interface,
                         reply, (boot_uint16_t)reply_size);
        if (got > 0) break;
        timer_wait(20);
    }
    return got;
}

/* A stalled control endpoint stays stalled.
 *
 * That is the part worth remembering: a device that refuses one request does
 * not merely fail it, it halts endpoint zero, and every request after it fails
 * too - with a different message each time, none of them naming the one that
 * actually went wrong. Clearing the halt turns a cascade back into a single
 * failure.
 *
 * This was first written as a CLEAR_FEATURE sent to the device - which cannot
 * work, because the only road to the device is the endpoint that is halted.
 * The request to unstick it went out over the stuck thing and failed, and the
 * log filled with "control transfer produced no event" for every request after
 * it. Nothing here is the device's business at all: the halt that matters is
 * the controller's idea of the endpoint, cleared by Reset Endpoint, and the
 * dequeue pointer left parked on the TRB that stalled, moved on by hand. USB
 * clears endpoint zero on the device side at the next SETUP with no help from
 * us, which is exactly why there is no CLEAR_FEATURE here any more. */
static void clear_control_halt(USB_DEVICE* device) {
    XHCI_CONTROLLER* self = device->controller;

    if (!run_command(self, "reset endpoint zero", 0,
                     (TRB_RESET_ENDPOINT << TRB_TYPE_SHIFT) |
                     (1u << 16) | (device->slot << 24)))
        return;
    (void)run_command(self, "set dequeue pointer",
                      ring_position(&device->control),
                      (TRB_SET_TR_DEQUEUE << TRB_TYPE_SHIFT) |
                      (1u << 16) | (device->slot << 24));
}

/* Ask the device for one thing it knows about itself. */
static int rndis_query(USB_DEVICE* device, boot_uint8_t interface,
                       boot_uint32_t oid, boot_uint8_t* buffer,
                       boot_uint32_t size, boot_uint8_t* out,
                       boot_uint32_t* out_length) {
    boot_uint32_t id = rndis_request_id++;
    boot_uint32_t offset;
    boot_uint32_t info;
    int got;

    memset(buffer, 0, size);
    put32(buffer + 0, RNDIS_QUERY);
    put32(buffer + 4, 28);              /* header only: nothing is being sent */
    put32(buffer + 8, id);
    put32(buffer + 12, oid);
    put32(buffer + 16, 0);              /* information length */
    /* And the offset zero with it.
     *
     * This said 20 - the offset of the byte after the header, which is where
     * an information buffer would start if there were one. There is not: a
     * query sends no data, it asks for some. A device that takes the pair
     * seriously reads "a zero-length buffer beginning one byte past the end of
     * the message" and stalls, which is exactly what QEMU's adapter did while
     * the phone let it pass. The phone was the more forgiving of the two and
     * the emulator was right. */
    put32(buffer + 20, 0);              /* offset, from byte 8 */
    put32(buffer + 24, 0);              /* device VC handle, always zero */

    got = rndis_exchange(device, interface, buffer, 28, buffer, size);
    if (got < 24) { clear_control_halt(device); return 0; }
    if (get32(buffer + 0) != (RNDIS_QUERY | RNDIS_COMPLETE)) return 0;
    if (get32(buffer + 8) != id) return 0;
    if (get32(buffer + 12) != RNDIS_STATUS_SUCCESS) return 0;

    info = get32(buffer + 16);
    offset = get32(buffer + 20);
    /* The offset counts from byte 8 of the message, which is the one detail of
       this protocol that is easiest to get wrong and hardest to see. */
    if (offset + 8 + info > (boot_uint32_t)got) return 0;
    if (info > *out_length) info = *out_length;
    memcpy(out, buffer + 8 + offset, info);
    *out_length = info;
    return 1;
}

static int rndis_set(USB_DEVICE* device, boot_uint8_t interface,
                     boot_uint32_t oid, boot_uint32_t value,
                     boot_uint8_t* buffer, boot_uint32_t size) {
    boot_uint32_t id = rndis_request_id++;
    int got;

    memset(buffer, 0, size);
    put32(buffer + 0, RNDIS_SET);
    put32(buffer + 4, 32);
    put32(buffer + 8, id);
    put32(buffer + 12, oid);
    put32(buffer + 16, 4);              /* four bytes of information */
    put32(buffer + 20, 20);
    put32(buffer + 24, 0);
    put32(buffer + 28, value);

    got = rndis_exchange(device, interface, buffer, 32, buffer, size);
    if (got < 16) { clear_control_halt(device); return 0; }
    if (get32(buffer + 0) != (RNDIS_SET | RNDIS_COMPLETE)) return 0;
    if (get32(buffer + 8) != id) return 0;
    return get32(buffer + 12) == RNDIS_STATUS_SUCCESS;
}

/* Introduce ourselves, and report what the device says it can do. */
static int rndis_initialize(USB_DEVICE* device, boot_uint8_t interface,
                            boot_uint8_t* buffer, boot_uint32_t size,
                            boot_uint32_t* out_max_transfer) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint32_t id = rndis_request_id++;
    int got;

    memset(buffer, 0, size);
    put32(buffer + 0, RNDIS_INITIALIZE);
    put32(buffer + 4, 24);
    put32(buffer + 8, id);
    put32(buffer + 12, 1);              /* major version */
    put32(buffer + 16, 0);              /* minor version */
    /* The most the device may send us in one bus transfer, and the number is
       not a preference: the device batches frames up to it, and anything over
       it arrives as babble on an endpoint that then has to be reset. Our
       receive buffer is one page, so one page is what we promise. */
    put32(buffer + 20, PAGE_SIZE);

    got = rndis_exchange(device, interface, buffer, 24, buffer, size);
    if (got < 0) {
        log_controller(self);
        log("RNDIS initialize got no reply\n");
        return 0;
    }
    if (got < 48 || get32(buffer + 0) != (RNDIS_INITIALIZE | RNDIS_COMPLETE) ||
        get32(buffer + 8) != id) {
        log_controller(self);
        log("RNDIS initialize was answered with something else\n");
        return 0;
    }
    if (get32(buffer + 12) != RNDIS_STATUS_SUCCESS) {
        log_controller(self);
        log("RNDIS initialize refused\n");
        return 0;
    }

    /* The reply's fields, in the order the protocol puts them:
     *
     *   16 major version   20 minor version   24 device flags
     *   28 medium          32 packets per transfer   36 max transfer size
     *
     * Written out because reading them one field early is a mistake with no
     * symptom of its own: this took the device flags for the medium and
     * announced that a perfectly ordinary Ethernet adapter was not Ethernet. */
    if (get32(buffer + 28) != 0) {
        log_controller(self);
        log("RNDIS device is not Ethernet\n");
        return 0;
    }

    *out_max_transfer = get32(buffer + 36);
    log_controller(self);
    log("RNDIS version ");
    log_dec(get32(buffer + 16));
    log(".");
    log_dec(get32(buffer + 20));
    log(", up to ");
    log_dec(get32(buffer + 32));
    log(" packet(s) and ");
    log_dec(*out_max_transfer);
    log(" bytes per transfer\n");
    return 1;
}


/* Report what is on the other end of the cable. Opening the endpoints and
   speaking the protocol come next; being certain which shape this device is,
   and that it has agreed to be a network at all, comes first. */
static int configure_network(USB_DEVICE* device,
                             const boot_uint8_t* configuration,
                             boot_uint32_t length) {
    XHCI_CONTROLLER* self = device->controller;
    USB_NETWORK network;
    boot_uint32_t max_frame = 0;

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

    if (network.kind != NETWORK_RNDIS) {
        log_controller(self);
        log("CDC Ethernet is not driven yet\n");
        return 0;
    }

    /* Configure the device before saying a word to it.
     *
     * Every class request is addressed to an interface, and a device that has
     * not been given a configuration has no interfaces yet - so it answers
     * with a stall, which reads as "I do not understand that" and is really
     * "you have not told me who I am". The other two class drivers each do
     * this; this one did not, and the reply was a stall on a message that was
     * byte for byte correct. */
    if (control_in(device, 0x00, USB_SET_CONFIGURATION, configuration[5],
                   0, (void*)0, 0) < 0) {
        log_controller(self);
        log("the device would not take its configuration\n");
        return 0;
    }

    {
        boot_uint8_t* buffer = (boot_uint8_t*)alloc_page();
        boot_uint32_t max_transfer = 0;
        boot_uint8_t mac[6];
        boot_uint32_t mac_length = sizeof(mac);

        if (!buffer) return 0;
        if (!rndis_initialize(device, network.control, buffer, PAGE_SIZE,
                              &max_transfer)) {
            free_page(buffer);
            return 0;
        }

        if (rndis_query(device, network.control, OID_802_3_PERMANENT_ADDRESS,
                        buffer, PAGE_SIZE, mac, &mac_length) &&
            mac_length == 6) {
            memcpy(network_mac, mac, 6);
            log_controller(self);
            log("hardware address ");
            for (int index = 0; index < 6; index++) {
                if (index) log(":");
                log_hex_byte(mac[index]);
            }
            log("\n");
        } else {
            log_controller(self);
            log("the device would not say what its address is\n");
        }

        if (!rndis_set(device, network.control, OID_GEN_CURRENT_PACKET_FILTER,
                       RNDIS_PACKET_FILTER, buffer, PAGE_SIZE)) {
            log_controller(self);
            log("the packet filter was refused - it will hand over nothing\n");
        }

        /* And whatever the device has been holding for us.
         *
         * RNDIS has a second channel the other way: the device raises its
         * interrupt endpoint to say "there is a response waiting" and the host
         * fetches it. After initialize, Android puts a media-connect
         * indication there and waits. Nothing here ever collected it, and a
         * device whose response queue is never drained is a device that has
         * said the link is up and had nobody listen - which is exactly what
         * four frames sent and none received looks like.
         *
         * Drained by asking rather than by watching the interrupt endpoint.
         * That endpoint is the proper way and is worth having; this is the
         * cheap half of it, and it either changes the symptom or rules the
         * whole idea out. */
        for (int drain = 0; drain < 4; drain++) {
            int got = control_in(device, 0xA1, RNDIS_GET_RESPONSE, 0,
                                 network.control, buffer, PAGE_SIZE);
            boot_uint32_t type;

            if (got < 12) break;
            type = get32(buffer + 0);
            /* Only the two kinds a device sends unasked. A controller with
               nothing queued may hand back the last reply it gave rather than
               nothing at all - QEMU's does - and treating that as news would
               be reading an echo as a message. */
            if (type != RNDIS_INDICATE_STATUS && type != RNDIS_KEEPALIVE) break;

            log_controller(self);
            if (type == RNDIS_KEEPALIVE) {
                log("the device asked whether we are still here\n");
            } else {
                boot_uint32_t status = get32(buffer + 8);
                log("the device says the link is ");
                if (status == RNDIS_STATUS_MEDIA_CONNECT) log("up\n");
                else if (status == RNDIS_STATUS_MEDIA_DISCONNECT)
                    log("down - tethering is on but not carrying anything\n");
                else { log("in state "); log_hex(status); log("\n"); }
            }
            timer_wait(20);
        }

        max_frame = max_transfer;
        free_page(buffer);
    }

    /* The two bulk endpoints, which is where frames will go.
     *
     * The alternate setting comes first and is not a formality: a CDC data
     * interface sitting in setting 0 has no endpoints at all, by design, and
     * asking the controller to configure endpoints the device has not offered
     * yet is a Configure Endpoint command that fails for a reason that reads
     * like a driver bug. RNDIS keeps them in setting 0, so this is a no-op
     * there and essential everywhere else. */
    if (network.data_alternate &&
        control_in(device, 0x01, USB_SET_INTERFACE, network.data_alternate,
                   network.data, 0, 0) < 0) {
        log_controller(self);
        log("the data interface would not switch to its working setting\n");
        return 0;
    }

    {
        boot_uint32_t in_dci = (boot_uint32_t)(network.in_address & 0x0F) * 2 + 1;
        boot_uint32_t out_dci = (boot_uint32_t)(network.out_address & 0x0F) * 2;
        boot_uint32_t last = in_dci > out_dci ? in_dci : out_dci;
        TRB* in_trbs = (TRB*)alloc_page();
        TRB* out_trbs = (TRB*)alloc_page();
        boot_uint8_t* input = (boot_uint8_t*)alloc_page();

        network_receive_buffer = (boot_uint8_t*)alloc_page();
        network_send_buffer = (boot_uint8_t*)alloc_page();
        if (!in_trbs || !out_trbs || !input ||
            !network_receive_buffer || !network_send_buffer) {
            log_controller(self);
            log("out of memory opening the network endpoints\n");
            return 0;
        }
        memset(input, 0, PAGE_SIZE);
        ring_init(&network_in, in_trbs);
        ring_init(&network_out, out_trbs);

        ((boot_uint32_t*)input)[1] = 1u | (1u << in_dci) | (1u << out_dci);
        describe_slot(self, input, device, last);
        describe_endpoint(self, input, in_dci, ENDPOINT_TYPE_BULK_IN,
                          network.in_packet, 0, &network_in);
        describe_endpoint(self, input, out_dci, ENDPOINT_TYPE_BULK_OUT,
                          network.out_packet, 0, &network_out);

        if (!run_command(self, "configure endpoint",
                         (boot_uint64_t)(unsigned long long)input,
                         (TRB_CONFIGURE_ENDPOINT << TRB_TYPE_SHIFT) |
                         (device->slot << 24))) {
            free_page(input);
            return 0;
        }
        free_page(input);

        network_device = device;
        network_in_dci = in_dci;
        network_out_dci = out_dci;
        network_in_address = network.in_address;
        network_out_address = network.out_address;
        network_out_packet = network.out_packet;
        network_frame_limit = max_frame;
        log_controller(self);
        log("frame channels open, up to ");
        log_dec(max_frame);
        log(" bytes each\n");
    }

    network_ready = 1;
    arm_network_receive();
    return 1;
}

/* ---- Frames -------------------------------------------------------------
 *
 * RNDIS carries an Ethernet frame inside a 44-byte header, and the header is
 * mostly zeroes: what it really says is where the frame starts and how long it
 * is. The fields that matter:
 *
 *    0  message type, 1 for a packet     4  total length, header included
 *    8  data offset, counted from byte 8 (so 36 for a frame straight after)
 *   12  data length
 *
 * Everything from 16 on is out-of-band data and per-packet information, which
 * nothing here sends and nothing here needs to read.
 */
#define RNDIS_PACKET 0x00000001U
#define RNDIS_PACKET_HEADER 44

/* Put the frame in the queue, or count it as lost.
 *
 * Dropping is the right answer when the queue is full: the alternative is to
 * stop collecting from the device, and a receive endpoint that nobody empties
 * backs up into the phone. A count is kept because a driver that silently
 * loses frames is indistinguishable from a network that is not working. */
static void queue_frame(const boot_uint8_t* frame, boot_uint32_t length) {
    boot_uint32_t next = (network_queue_tail + 1) % NETWORK_QUEUE;

    if (length > NETWORK_FRAME_MAX || next == network_queue_head) {
        network_dropped++;
        return;
    }
    network_received++;
    memcpy(network_queue[network_queue_tail], frame, length);
    network_queue_length[network_queue_tail] = (boot_uint16_t)length;
    network_queue_tail = next;
}

/* Split one bulk transfer into the frames it holds. */
static void unwrap_frames(boot_uint32_t total) {
    boot_uint32_t offset = 0;

    while (offset + RNDIS_PACKET_HEADER <= total) {
        const boot_uint8_t* message = network_receive_buffer + offset;
        boot_uint32_t message_length = get32(message + 4);
        boot_uint32_t data_offset = get32(message + 8);
        boot_uint32_t data_length = get32(message + 12);

        if (get32(message + 0) != RNDIS_PACKET) break;
        /* Every one of these has been seen from something: a length that runs
           past the transfer, an offset that points outside its own message.
           Trusting them would be reading the kernel's memory at a device's
           direction. */
        if (message_length < RNDIS_PACKET_HEADER ||
            offset + message_length > total) break;
        if (data_offset + 8 + data_length > message_length) break;

        queue_frame(message + 8 + data_offset, data_length);
        offset += message_length;
    }
}

/* Keep one receive request outstanding at all times.
 *
 * A bulk IN endpoint hands over nothing until it is asked, and it is asked by
 * a TRB sitting on its ring. The moment one completes the next has to go down,
 * or the device has arriving frames and nowhere to put them. */
static void arm_network_receive(void) {
    if (!network_ready) return;
    ring_push(&network_in,
              (boot_uint64_t)(unsigned long long)network_receive_buffer,
              PAGE_SIZE, (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC);
    ring_doorbell(network_device->controller, network_device->slot,
                  network_in_dci);
}

/* Is this transfer event a frame arriving? Same shape as the keyboard's hook,
   and for the same reason: these land unbidden, in the middle of whatever else
   is going on. */
static int network_event(const XHCI_CONTROLLER* self, const TRB* event) {
    boot_uint32_t code;
    boot_uint32_t moved;

    if (!network_ready || !network_device) return 0;
    if (network_device->controller != self) return 0;
    if (event_slot(event) != network_device->slot) return 0;
    if (event_endpoint(event) != network_in_dci) return 0;

    code = event->status >> 24;
    moved = PAGE_SIZE - (event->status & 0xFFFFFF);
    if (code == COMPLETION_SUCCESS || code == COMPLETION_SHORT_PACKET) {
        unwrap_frames(moved);
    } else if (code == COMPLETION_STALL) {
        clear_stall(network_device, network_in_dci, &network_in,
                    network_in_address);
    } else {
        /* Everything else used to be swallowed here, which meant a receive
           endpoint failing every single time looked identical to a network
           with nothing on it. Said once and then counted, because if it is
           happening at all it is happening a thousand times. */
        if (!network_receive_errors) {
            log_controller(self);
            log("frames are not arriving: ");
            log(completion_name(code));
            log("\n");
        }
        network_receive_errors++;
    }
    arm_network_receive();
    return 1;
}

/* Hand over one frame, or 0 if none has arrived. Never waits: this is called
   from the same loop that draws a prompt. */
boot_uint32_t usb_net_receive(void* frame, boot_uint32_t size) {
    boot_uint32_t length;

    if (network_queue_head == network_queue_tail) return 0;
    length = network_queue_length[network_queue_head];
    if (length > size) length = size;
    memcpy(frame, network_queue[network_queue_head], length);
    network_queue_head = (network_queue_head + 1) % NETWORK_QUEUE;
    return length;
}

/* Send one Ethernet frame. Returns 1 when the controller says it went. */
int usb_net_send(const void* frame, boot_uint32_t length) {
    boot_uint32_t total = RNDIS_PACKET_HEADER + length;
    int moved;

    if (!network_ready || !length || length > NETWORK_FRAME_MAX) return 0;
    if (total > network_frame_limit) return 0;

    memset(network_send_buffer, 0, RNDIS_PACKET_HEADER);
    put32(network_send_buffer + 0, RNDIS_PACKET);
    put32(network_send_buffer + 4, total);
    put32(network_send_buffer + 8, RNDIS_PACKET_HEADER - 8);
    put32(network_send_buffer + 12, length);
    memcpy(network_send_buffer + RNDIS_PACKET_HEADER, frame, length);

    moved = bulk_transfer(network_device, &network_out, network_out_dci,
                          network_send_buffer, total, 1000);
    if (moved == -2) {
        clear_stall(network_device, network_out_dci, &network_out,
                    network_out_address);
        network_send_failed++;
        return 0;
    }
    if (moved < 0) { network_send_failed++; return 0; }
    network_sent++;

    /* A transfer that is an exact multiple of the packet size does not end as
       far as the device is concerned - it is still waiting for the short
       packet that says "that was all of it". An empty transfer says it. */
    if (network_out_packet && total % network_out_packet == 0)
        (void)bulk_transfer(network_device, &network_out, network_out_dci,
                            network_send_buffer, 0, 1000);
    return 1;
}

int usb_net_ready(void) {
    return network_ready;
}

const boot_uint8_t* usb_net_address(void) {
    return network_mac;
}

boot_uint32_t usb_net_dropped(void) {
    return network_dropped;
}

/* What the controller thinks of the endpoints, in its own words.
 *
 * "Four frames sent and none received" is the end of what the driver can say
 * about itself. Everything it believes about those transfers comes from
 * completion codes, and a completion code says the controller finished with a
 * descriptor - not that anything left the machine.
 *
 * The output device context is the other side of that: the controller writes
 * its own state there and it is never read here at all. An endpoint in Error
 * or Halted while every transfer reports success is a thing that can happen
 * and would look exactly like this. */
void usb_net_diagnose(void) {
    XHCI_CONTROLLER* self;
    static const char* states[] = {
        "disabled", "running", "halted", "stopped", "error",
        "state 5", "state 6", "state 7"
    };

    if (!network_device || !network_device->context) {
        log("XHCI: there is no network device to ask about\n");
        return;
    }
    self = network_device->controller;

    {
        const boot_uint32_t* slot =
            (const boot_uint32_t*)network_device->context;
        log_controller(self);
        log("slot state ");
        log_dec((slot[3] >> 27) & 0x1F);
        log(", address ");
        log_dec(slot[3] & 0xFF);
        log(", route ");
        log_hex(slot[0] & 0xFFFFF);
        log("\n");
    }

    for (int which = 0; which < 2; which++) {
        boot_uint32_t dci = which ? network_out_dci : network_in_dci;
        const boot_uint32_t* endpoint = (const boot_uint32_t*)
            (network_device->context + self->context_size * dci);

        log_controller(self);
        log(which ? "out endpoint " : "in endpoint ");
        log_dec(dci);
        log(": ");
        log(states[endpoint[0] & 0x07]);
        log(", packet ");
        log_dec((endpoint[1] >> 16) & 0xFFFF);
        log(", dequeue ");
        log_hex(((boot_uint64_t)endpoint[3] |
                 ((boot_uint64_t)endpoint[2] << 32)) & ~0xFULL);
        log("\n");
    }
}

void usb_net_counters(boot_uint32_t* sent, boot_uint32_t* received,
                      boot_uint32_t* failed) {
    *sent = network_sent;
    *received = network_received;
    *failed = network_send_failed + network_receive_errors;
}


/* ---- Hubs ---------------------------------------------------------------
 *
 * A hub is the reason a machine with four keyboards in its BIOS shows one to
 * this driver. Everything plugged into it is invisible until somebody asks
 * the hub what it has, and the hub is a USB device like any other: it answers
 * class requests over endpoint zero, one per downstream port.
 *
 * Two things make this more than a loop over ports.
 *
 * The first is the route string. A host controller reaches a device through a
 * chain of hubs by being told the path: four bits per tier, the downstream
 * port number at each one, up to five tiers. Every command about the device
 * carries it, which is why the slot context is now built in one place.
 *
 * The second is the transaction translator. A full or low speed device on a
 * high speed bus cannot talk to the controller at all - the hub speaks to it
 * slowly and to the controller quickly, and the controller has to be told
 * which hub and which port is doing that. Get it wrong and the device
 * enumerates perfectly and then never delivers a transfer, which is a failure
 * this driver has already met once, from a different cause.
 */

#define USB_CLASS_HUB 9

/* Class requests, which differ from the standard ones only in the recipient
   and the direction bits. Everything about a port is addressed to the port. */
#define HUB_GET_STATUS 0
#define HUB_CLEAR_FEATURE 1
#define HUB_SET_FEATURE 3
#define HUB_DESCRIPTOR 0x29         /* 0x2A on a SuperSpeed hub */
#define HUB_DESCRIPTOR_SUPER 0x2A

#define PORT_CONNECTION 0
#define PORT_ENABLE 1
#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET 20

#define PORT_STATUS_CONNECTED 0x0001
#define PORT_STATUS_ENABLED 0x0002
#define PORT_STATUS_RESET 0x0010
#define PORT_STATUS_LOW_SPEED 0x0200
#define PORT_STATUS_HIGH_SPEED 0x0400

/* Five tiers is the whole of the route string, and also the whole of what USB
   allows: beyond it the round trip is longer than the protocol's timeouts. */
#define HUB_MAX_TIER 5
#define HUB_MAX 8

typedef struct {
    USB_DEVICE* device;
    boot_uint32_t ports;
    /* Which ports we last saw something on, so a change can be noticed
       without re-enumerating a device that has not moved. */
    boot_uint32_t occupied;
    int used;
} USB_HUB;

static USB_HUB hubs[HUB_MAX];
static boot_uint32_t hub_count;

static USB_DEVICE* attach_at(XHCI_CONTROLLER* self, boot_uint32_t root_port,
                             boot_uint32_t route, boot_uint32_t tier,
                             boot_uint32_t speed, boot_uint32_t tt_slot,
                             boot_uint32_t tt_port);
static void forget_below(XHCI_CONTROLLER* self, boot_uint32_t route,
                         boot_uint32_t tier);

static int hub_port_status(USB_DEVICE* device, boot_uint32_t port,
                           boot_uint16_t* status, boot_uint16_t* change) {
    boot_uint8_t buffer[4];

    if (control_in(device, 0xA3, HUB_GET_STATUS, 0, (boot_uint16_t)port,
                   buffer, 4) < 4)
        return 0;
    *status = (boot_uint16_t)(buffer[0] | (buffer[1] << 8));
    *change = (boot_uint16_t)(buffer[2] | (buffer[3] << 8));
    return 1;
}

static int hub_port_feature(USB_DEVICE* device, boot_uint8_t request,
                            boot_uint16_t feature, boot_uint32_t port) {
    return control_in(device, 0x23, request, feature, (boot_uint16_t)port,
                      (void*)0, 0) >= 0;
}

/* Reset one downstream port and report what speed came up on it.
 *
 * A hub port has to be reset before the device on it will answer to address
 * zero, exactly like a root port - and like a root port, the speed is only
 * meaningful afterwards. Returns 0 if nothing usable came up. */
static boot_uint32_t hub_reset_port(USB_HUB* hub, boot_uint32_t port) {
    USB_DEVICE* device = hub->device;
    boot_uint16_t status = 0;
    boot_uint16_t change = 0;
    boot_uint64_t start;

    if (!hub_port_feature(device, HUB_SET_FEATURE, PORT_RESET, port)) return 0;

    start = timer_ticks();
    while (!timer_expired(start, 800)) {
        if (!hub_port_status(device, port, &status, &change)) return 0;
        if (!(status & PORT_STATUS_RESET) && (status & PORT_STATUS_ENABLED))
            break;
        timer_wait(10);
    }
    if (status & PORT_STATUS_RESET) return 0;
    if (!(status & PORT_STATUS_ENABLED)) return 0;

    (void)hub_port_feature(device, HUB_CLEAR_FEATURE, C_PORT_RESET, port);
    /* USB asks for 10 ms of quiet after a reset before the device is spoken
       to. Skipping it works on most devices and not on all of them, and the
       ones it fails on fail at the first descriptor read. */
    timer_wait(10);

    /* A SuperSpeed hub only has SuperSpeed devices behind it; the speed bits
       below are USB 2's and do not appear there. */
    if (device->speed == SPEED_SUPER) return SPEED_SUPER;
    if (status & PORT_STATUS_LOW_SPEED) return SPEED_LOW;
    if (status & PORT_STATUS_HIGH_SPEED) return SPEED_HIGH;
    return SPEED_FULL;
}

/* Enumerate whatever is on one downstream port. */
static void hub_attach(USB_HUB* hub, boot_uint32_t port) {
    USB_DEVICE* device = hub->device;
    XHCI_CONTROLLER* self = device->controller;
    boot_uint32_t speed;
    boot_uint32_t route;
    boot_uint32_t tt_slot;
    boot_uint32_t tt_port;

    if (device->tier >= HUB_MAX_TIER) {
        log_controller(self);
        log("a hub too many tiers deep to reach\n");
        return;
    }

    speed = hub_reset_port(hub, port);
    if (!speed) {
        log_controller(self);
        log("hub port ");
        log_dec(port);
        log(" would not come up\n");
        return;
    }

    /* The port number goes into the nibble for this hub's tier. */
    route = device->route | ((port & 0xF) << (4 * device->tier));

    /* Who translates, if anybody. A slow device on a fast hub is translated
       by that hub; a slow device on a slow hub is translated by whatever was
       already translating for the hub, further up. */
    if (speed == SPEED_LOW || speed == SPEED_FULL) {
        if (device->speed == SPEED_HIGH) {
            tt_slot = device->slot;
            tt_port = port;
        } else {
            tt_slot = device->tt_slot;
            tt_port = device->tt_port;
        }
    } else {
        tt_slot = 0;
        tt_port = 0;
    }

    log_controller(self);
    log("hub port ");
    log_dec(port);
    log(" has a device, speed ");
    log_dec(speed);
    log("\n");
    (void)attach_at(self, device->port, route, device->tier + 1, speed,
                    tt_slot, tt_port);
}

/* Claim a hub: learn its shape, tell the controller about it, switch its ports
   on, and enumerate whatever is already there. */
static int configure_hub(USB_DEVICE* device, const boot_uint8_t* configuration,
                         boot_uint32_t length) {
    XHCI_CONTROLLER* self = device->controller;
    boot_uint8_t descriptor[16];
    boot_uint8_t* input;
    boot_uint32_t offset = 0;
    boot_uint32_t settle;
    USB_HUB* hub;
    int found = 0;

    while (offset + 2 <= length) {
        boot_uint8_t size = configuration[offset];

        if (!size || offset + size > length) break;
        if (configuration[offset + 1] == USB_DESCRIPTOR_INTERFACE && size >= 9 &&
            configuration[offset + 5] == USB_CLASS_HUB)
            found = 1;
        offset += size;
    }
    if (!found) return 0;

    hub = (USB_HUB*)0;
    for (boot_uint32_t index = 0; index < HUB_MAX; index++)
        if (!hubs[index].used) {
            hub = &hubs[index];
            if (index >= hub_count) hub_count = index + 1;
            break;
        }
    if (!hub) {
        log_controller(self);
        log("no room for another hub\n");
        return 0;
    }

    if (control_in(device, 0x00, USB_SET_CONFIGURATION, configuration[5],
                   0, (void*)0, 0) < 0) {
        log_controller(self);
        log("the hub would not take its configuration\n");
        return 0;
    }

    /* Both descriptor types put the port count at byte 2 and the power-on
       settling time at byte 5, so the only thing the version changes is which
       one the hub will admit to having. */
    memset(descriptor, 0, sizeof(descriptor));
    if (control_in(device, 0xA0, USB_GET_DESCRIPTOR,
                   (boot_uint16_t)(device->speed == SPEED_SUPER
                                       ? (HUB_DESCRIPTOR_SUPER << 8)
                                       : (HUB_DESCRIPTOR << 8)),
                   0, descriptor, sizeof(descriptor)) < 6) {
        log_controller(self);
        log("the hub would not describe itself\n");
        return 0;
    }

    device->is_hub = 1;
    device->hub_ports = descriptor[2];
    if (!device->hub_ports || device->hub_ports > 15) {
        log_controller(self);
        log("a hub claiming ");
        log_dec(descriptor[2]);
        log(" ports, which cannot be right\n");
        device->is_hub = 0;
        return 0;
    }

    /* The controller has to know it is a hub before anything behind it can be
       addressed: it schedules differently for a device with devices under it,
       and the port count is how it sizes that. Configure Endpoint with only
       the slot flag set updates the slot context and touches nothing else. */
    input = (boot_uint8_t*)alloc_page();
    if (!input) {
        log_controller(self);
        log("out of memory claiming a hub\n");
        return 0;
    }
    memset(input, 0, PAGE_SIZE);
    ((boot_uint32_t*)input)[1] = 1u;
    describe_slot(self, input, device, 1);
    if (!run_command(self, "configure hub",
                     (boot_uint64_t)(unsigned long long)input,
                     (TRB_CONFIGURE_ENDPOINT << TRB_TYPE_SHIFT) |
                     (device->slot << 24))) {
        free_page(input);
        return 0;
    }
    free_page(input);

    hub->device = device;
    hub->ports = device->hub_ports;
    hub->occupied = 0;
    hub->used = 1;

    log_controller(self);
    log("hub with ");
    log_dec(hub->ports);
    log(" port(s) at tier ");
    log_dec(device->tier + 1);
    log("\n");

    /* Switch the ports on. A bus-powered hub may have them off at reset, and
       a port with no power looks exactly like a port with nothing in it. The
       descriptor says how long to wait afterwards, in two-millisecond units;
       the floor is because some hubs report an optimistic zero. */
    for (boot_uint32_t port = 1; port <= hub->ports; port++)
        (void)hub_port_feature(device, HUB_SET_FEATURE, PORT_POWER, port);
    settle = (boot_uint32_t)descriptor[5] * 2;
    timer_wait(settle < 100 ? 100 : settle);

    {
        boot_uint32_t occupied = 0;
        boot_uint32_t unanswered = 0;

        for (boot_uint32_t port = 1; port <= hub->ports; port++) {
            boot_uint16_t status = 0;
            boot_uint16_t change = 0;

            if (!hub_port_status(device, port, &status, &change)) {
                unanswered++;
                continue;
            }
            if (change & 1) (void)hub_port_feature(device, HUB_CLEAR_FEATURE,
                                                   C_PORT_CONNECTION, port);
            if (!(status & PORT_STATUS_CONNECTED)) continue;
            occupied++;
            hub->occupied |= 1u << port;
            hub_attach(hub, port);
        }

        /* Said out loud even when it is nothing, because "the hub has nothing
           in it" and "the hub would not answer" are the same silence
           otherwise - and they are completely different problems. */
        log_controller(self);
        log("hub ports: ");
        log_dec(occupied);
        log(" in use");
        if (unanswered) {
            log(", ");
            log_dec(unanswered);
            log(" would not say");
        }
        log("\n");
    }
    return 1;
}

/* One pass over every hub's ports, for things plugged in or pulled out since
   the last look. The same job survey_ports does for the root hub, and it has
   to be a poll for the same reason: no interrupt from any of this is wired to
   anything yet. */
static void service_hubs(void) {
    for (boot_uint32_t index = 0; index < hub_count; index++) {
        USB_HUB* hub = &hubs[index];
        USB_DEVICE* device;

        if (!hub->used || !hub->device || !hub->device->used) continue;
        device = hub->device;

        for (boot_uint32_t port = 1; port <= hub->ports; port++) {
            boot_uint16_t status = 0;
            boot_uint16_t change = 0;
            boot_uint32_t bit = 1u << port;
            int here;

            if (!hub_port_status(device, port, &status, &change)) {
                /* A hub that stops answering has been unplugged, or is on its
                   way to it. Asking the other ports will not go better, and
                   the root-port scan will notice the unplug soon enough. */
                break;
            }
            if (change & 1)
                (void)hub_port_feature(device, HUB_CLEAR_FEATURE,
                                       C_PORT_CONNECTION, port);
            here = (status & PORT_STATUS_CONNECTED) != 0;
            if (here == ((hub->occupied & bit) != 0)) continue;

            if (here) {
                hub->occupied |= bit;
                hub_attach(hub, port);
            } else {
                hub->occupied &= ~bit;
                log_controller(device->controller);
                log("hub port ");
                log_dec(port);
                log(" was unplugged\n");
                forget_below(device->controller,
                             device->route |
                                 ((port & 0xF) << (4 * device->tier)),
                             device->tier + 1);
            }
        }
    }
}

/* ---- Bringing devices up ------------------------------------------------ */

/* Somewhere to put one.
 *
 * The list used to only grow, because a device only ever arrived. Plugging and
 * unplugging the same stick thirty-two times would then fill it with entries
 * for devices that are not there - which nothing would notice until the
 * thirty-third, and then it would look like a limit rather than a leak. A
 * released entry is reused. */
static USB_DEVICE* free_device(XHCI_CONTROLLER* self) {
    for (boot_uint32_t index = 0; index < USB_MAX_DEVICES; index++)
        if (!self->devices[index].used) return &self->devices[index];
    return (USB_DEVICE*)0;
}

/* Enumerate a device wherever it is, and hand it to a class driver.
 *
 * "Wherever it is" is the part that changed: a device used to be a root port
 * number and nothing else, because a root port was the only place one could
 * be. Behind a hub it is a root port plus a route through the hubs to reach
 * it, and possibly a hub translating for it. All of that arrives here from
 * whoever found the device - the root port scan or a hub - because only they
 * know it.
 *
 * A device nobody claims is left addressed and idle rather than torn down: it
 * costs one slot, and saying what it was is more useful than silence.
 *
 * Returns the device, so a hub can be told about the one it just found. */
static USB_DEVICE* attach_at(XHCI_CONTROLLER* self, boot_uint32_t root_port,
                             boot_uint32_t route, boot_uint32_t tier,
                             boot_uint32_t speed, boot_uint32_t tt_slot,
                             boot_uint32_t tt_port) {
    USB_DEVICE* device;
    boot_uint8_t* configuration;
    boot_uint32_t length;

    device = free_device(self);
    if (!device) {
        log_controller(self);
        log("no room left for another device\n");
        return (USB_DEVICE*)0;
    }
    memset(device, 0, sizeof(*device));
    device->controller = self;
    device->port = root_port;
    device->route = route;
    device->tier = tier;
    device->tt_slot = tt_slot;
    device->tt_port = tt_port;
    device->speed = speed;

    /* Zero is not a speed.
     *
     * The field has four defined values - full, low, high, super - and no
     * meaning for zero at all, yet a real controller hands it out for every
     * port it has. It goes straight into the slot context, where the
     * controller uses it to schedule the device's transfers, so a device
     * addressed at speed zero can hold a whole conversation over its control
     * endpoint and never deliver a single interrupt transfer. Which is exactly
     * what a keyboard that reports itself ready and then types nothing looks
     * like.
     *
     * Nothing here can know the real speed, so it takes the most conservative
     * one that works for any USB 2 device and says loudly what it did. The
     * raw register comes with it: whatever is wrong is in those bits. */
    if (!device->speed) {
        log_controller(self);
        log("port ");
        log_dec(root_port + 1);
        log(" reports no speed at all, assuming full speed\n");
        device->speed = SPEED_FULL;
    }
    device->slot = enable_slot(self);
    if (!device->slot) return (USB_DEVICE*)0;   /* enable_slot says why */

    /* Every failure from here gives the slot back.
     *
     * They did not, and the log of a machine with a flaky socket shows it: the
     * slot number climbing by one on every attempt, because each abandoned
     * enumeration kept the slot the controller had handed out. A controller
     * with thirty-two of them survives that for a while and not forever. */
    if (!address_device(device)) {
        log_controller(self);
        log("the device would not take an address\n");
        disable_slot(self, device->slot);
        return (USB_DEVICE*)0;
    }
    if (!identify_device(device)) {
        log_controller(self);
        log("the device would not describe itself\n");
        disable_slot(self, device->slot);
        return (USB_DEVICE*)0;
    }

    configuration = (boot_uint8_t*)alloc_page();
    if (!configuration) {
        log_controller(self);
        log("out of memory enumerating a device\n");
        disable_slot(self, device->slot);
        return (USB_DEVICE*)0;
    }
    memset(configuration, 0, PAGE_SIZE);
    length = read_configuration(device, configuration);
    if (!length) {
        log_controller(self);
        log("could not read the configuration descriptor\n");
        free_page(configuration);
        disable_slot(self, device->slot);
        return (USB_DEVICE*)0;
    }

    device->used = 1;
    if (device - self->devices >= (long)self->device_count)
        self->device_count = (boot_uint32_t)(device - self->devices) + 1;

    if (!configure_hub(device, configuration, length) &&
        !configure_keyboard(device, configuration, length) &&
        !configure_storage(device, configuration, length) &&
        !configure_network(device, configuration, length)) {
        log_controller(self);
        log("no driver for this device\n");
    }

    free_page(configuration);
    return device;
}

/* Everything above, for something plugged straight into the machine.
   Returns whether a device came up, so a port that keeps failing can be
   noticed as such rather than retried forever. */
static int attach_device(XHCI_CONTROLLER* self, boot_uint32_t port) {
    boot_uint32_t status;

    /* Three goes at it, each starting from a fresh port reset.
     *
     * Address Device failing once is not a broken device. A phone with a tired
     * socket, a stick that has not finished powering up, a cable moved while
     * the reset was in flight - all of them fail here and all of them work on
     * the next attempt. Without a retry the device is present, connected, and
     * gone until it is physically unplugged, because the port stays connected
     * and nothing ever asks it again. That is what "address device failed" and
     * then silence looked like on a real machine. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) {
            log_controller(self);
            log("port ");
            log_dec(port + 1);
            log(": trying again\n");
            timer_wait(100);
        }
        if (!reset_port(self, port)) {
            log_controller(self);
            log("port ");
            log_dec(port + 1);
            log(" would not reset\n");
            continue;
        }
        status = op_read32(self, OP_PORTSC(port));
        if (attach_at(self, port, 0, 0,
                      (status >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK,
                      0, 0))
            return 1;
        /* Anything still connected is worth another go; anything that has been
           pulled out in the meantime is not. */
        if (!(op_read32(self, OP_PORTSC(port)) & PORTSC_CONNECTED)) break;
    }
    return 0;
}

/* Something was unplugged. Let go of it before anything tries to talk to it.
 *
 * A device that has gone still answers every register read with plausible
 * values, so nothing notices on its own: a transfer posted to a dead slot
 * simply never completes, and the caller waits out its timeout for as long as
 * the machine is on. */
/* Let go of one device: tell every class driver that has a piece of it, then
   hand the slot back. Split out of the root-port path because a hub can lose a
   device too, and forgetting half of it in one of the two places is a driver
   that talks to hardware that is not there any more. */
static void release_device(XHCI_CONTROLLER* self, USB_DEVICE* device) {
    for (boot_uint32_t index = 0; index < storage_count; index++) {
        USB_STORAGE* gone = &storages[index];

        if (!gone->used || gone->device != device) continue;
        block_forget(gone->name);
        block_changed();
        gone->used = 0;
        gone->ready = 0;
        gone->device = (USB_DEVICE*)0;
        /* The "current" pointer must not be left aimed at a stick that has
           gone: the next block transfer sets it, but nothing else does. */
        if (storage == gone) storage = (USB_STORAGE*)0;
    }
    if (device == network_device) {
        network_ready = 0;
        network_device = (USB_DEVICE*)0;
    }
    for (boot_uint32_t slot = 0; slot < keyboard_count; slot++)
        if (keyboards[slot].used && keyboards[slot].device == device)
            keyboards[slot].used = 0;
    for (boot_uint32_t index = 0; index < hub_count; index++)
        if (hubs[index].used && hubs[index].device == device)
            hubs[index].used = 0;

    disable_slot(self, device->slot);
    device->used = 0;
    device->slot = 0;
}

/* Everything at a route, and everything below it.
 *
 * A hub that loses a port loses whatever was on it - and if that was another
 * hub, everything under that too. They are found by prefix: a device below
 * this one has the same route in the nibbles up to this tier, whatever it has
 * beyond them. */
static void forget_below(XHCI_CONTROLLER* self, boot_uint32_t route,
                         boot_uint32_t tier) {
    boot_uint32_t mask = tier >= 5 ? 0xFFFFF : ((1u << (4 * tier)) - 1);

    for (boot_uint32_t index = 0; index < self->device_count; index++) {
        USB_DEVICE* device = &self->devices[index];

        if (!device->used) continue;
        if (device->tier < tier) continue;
        if ((device->route & mask) != (route & mask)) continue;
        release_device(self, device);
    }
}

static void detach_device(XHCI_CONTROLLER* self, boot_uint32_t port) {
    for (boot_uint32_t index = 0; index < self->device_count; index++) {
        USB_DEVICE* device = &self->devices[index];

        if (!device->used || device->port != port) continue;

        log_controller(self);
        log("port ");
        log_dec(port + 1);
        log(" was unplugged\n");
        release_device(self, device);
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
                if (self->refusals[port] >= PORT_REFUSAL_LIMIT) continue;
                log_controller(self);
                log("port ");
                log_dec(port + 1);
                log(" has a new device, speed ");
                log_dec((status >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);
                log("\n");
                if (attach_device(self, port)) {
                    self->refusals[port] = 0;
                } else if (++self->refusals[port] >= PORT_REFUSAL_LIMIT) {
                    log_controller(self);
                    log("port ");
                    log_dec(port + 1);
                    log(" has failed ");
                    log_dec(PORT_REFUSAL_LIMIT);
                    log(" times running; leaving it alone until it is quiet\n");
                }
            } else {
                /* Gone is as good as fixed: whatever was wrong with it, a
                   different device deserves a clean start. */
                self->refusals[port] = 0;
                detach_device(self, port);
            }
            self->connected_ports = 0;
            for (boot_uint32_t other = 0; other < self->port_count; other++)
                self->connected_ports += self->connected[other];
        }
    }

    /* And the same question of everything hanging off a hub, which has no
       register to read and has to be asked. */
    service_hubs();
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
        (void)attach_device(self, port);
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

/* Every device the controllers have taken, wherever it is. The port count
   answers a different question now that hubs exist: seven things plugged into
   a machine can occupy one root port. */
boot_uint32_t xhci_device_count(void) {
    boot_uint32_t total = 0;

    for (boot_uint32_t index = 0; index < controller_count; index++) {
        XHCI_CONTROLLER* self = &controllers[index];
        for (boot_uint32_t slot = 0; slot < self->device_count; slot++)
            if (self->devices[slot].used) total++;
    }
    return total;
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
