#include "ehci.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"

/* EHCI, which is USB 2.0 and nothing like xHCI underneath.
 *
 * xHCI is rings of transfer descriptors and an event ring the controller
 * posts completions to. EHCI has neither. It has two schedules - a circular
 * list of queue heads the controller walks whenever it has nothing better to
 * do, and a thousand-entry table indexed by frame number for anything that has
 * to happen on time - and no events at all. A transfer is finished when the
 * Active bit in its descriptor goes away, and the only way to know is to look.
 *
 * Two things about it are traps rather than differences.
 *
 * The first is that the firmware owns the controller at boot and will not let
 * go unless asked. It watches the registers through a system management
 * interrupt, and a driver that starts writing to them without asking is
 * fighting something it cannot see for a device neither of them ends up
 * driving.
 *
 * The second is that EHCI cannot talk to a slow device at all. A USB 1 mouse
 * plugged into a USB 2 socket is handled by a second, separate controller
 * sharing the same physical port, and EHCI's job is to notice and hand the
 * port over. That is why a port can be connected, correct, and then simply not
 * ours.
 */

#define EHCI_MAX_CONTROLLERS 4
#define EHCI_MAX_PORTS 16

/* Capability registers, at the start of the register block. */
#define CAP_LENGTH 0x00           /* byte: where the operational registers are */
#define CAP_HCSPARAMS 0x04
#define CAP_HCCPARAMS 0x08

#define HCSPARAMS_PORTS 0x0F
#define HCSPARAMS_COMPANIONS_SHIFT 12
#define HCSPARAMS_COMPANIONS_MASK 0x0F

#define HCCPARAMS_64BIT 0x01
#define HCCPARAMS_EECP_SHIFT 8
#define HCCPARAMS_EECP_MASK 0xFF

/* Operational registers, offset from the operational base. */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_USBINTR 0x08
#define OP_FRINDEX 0x0C
#define OP_CTRLDSSEGMENT 0x10
#define OP_PERIODICLISTBASE 0x14
#define OP_ASYNCLISTADDR 0x18
#define OP_CONFIGFLAG 0x40
#define OP_PORTSC 0x44

#define USBCMD_RUN 0x00000001U
#define USBCMD_RESET 0x00000002U
#define USBCMD_ASYNC_ENABLE 0x00000020U
#define USBCMD_ASYNC_DOORBELL 0x00000040U
#define USBCMD_FRAME_LIST_SIZE 0x0000000CU

#define USBSTS_HALTED 0x00001000U
#define USBSTS_ASYNC_RUNNING 0x00008000U

/* Port status and control. The write-clear bits are the trap here: writing the
   register back with one of them set clears it, so every read-modify-write has
   to mask them out or it silently acknowledges news it never looked at. */
#define PORTSC_CONNECTED 0x00000001U
#define PORTSC_CONNECT_CHANGE 0x00000002U
#define PORTSC_ENABLED 0x00000004U
#define PORTSC_ENABLE_CHANGE 0x00000008U
#define PORTSC_OVERCURRENT_CHANGE 0x00000020U
#define PORTSC_RESET 0x00000100U
#define PORTSC_LINE_STATUS_SHIFT 10
#define PORTSC_LINE_STATUS_MASK 0x03
#define PORTSC_POWER 0x00001000U
#define PORTSC_OWNER 0x00002000U
#define PORTSC_WRITE_CLEAR (PORTSC_CONNECT_CHANGE | PORTSC_ENABLE_CHANGE | \
                            PORTSC_OVERCURRENT_CHANGE)

/* Line status 01 is K-state, which only a low speed device drives. It is
   readable before the port is reset and is the one chance to hand a slow
   device over without resetting it first. */
#define LINE_STATUS_LOW_SPEED 1

/* The legacy support capability, which is how the firmware is asked to let
   go. Its id is 1; the other capabilities in the list are not our business. */
#define EHCI_CAPABILITY_LEGACY 1
#define LEGACY_BIOS_OWNED 0x00010000U
#define LEGACY_OS_OWNED 0x01000000U

/* Queue head and transfer descriptor. Both must be 32-byte aligned, which a
   page allocation gives for free as long as they are spaced by 32 or 64. */
typedef struct {
    boot_uint32_t next;
    boot_uint32_t alternate;
    boot_uint32_t token;
    boot_uint32_t buffer[5];
    boot_uint32_t buffer_high[5];
} QTD;

typedef struct {
    boot_uint32_t horizontal;
    boot_uint32_t characteristics;
    boot_uint32_t capabilities;
    boot_uint32_t current;
    /* The overlay: the controller copies the active qTD in here and works on
       it, so these are written once at setup and read for results after. */
    QTD overlay;
} QUEUE_HEAD;

#define LINK_TERMINATE 0x00000001U
#define LINK_TYPE_QH 0x00000002U

#define QTD_STATUS_ACTIVE 0x80U
#define QTD_STATUS_HALTED 0x40U
#define QTD_STATUS_BUFFER_ERROR 0x20U
#define QTD_STATUS_BABBLE 0x10U
#define QTD_STATUS_TRANSACTION 0x08U

#define QTD_PID_OUT 0
#define QTD_PID_IN 1
#define QTD_PID_SETUP 2

#define QTD_TOKEN(status, pid, bytes, toggle) \
    ((boot_uint32_t)(status) | ((boot_uint32_t)(pid) << 8) | \
     (3U << 10) /* three error retries */ | \
     ((boot_uint32_t)(bytes) << 16) | ((boot_uint32_t)(toggle) << 31))

/* Endpoint speeds, as the characteristics field spells them. */
#define EPS_FULL 0
#define EPS_LOW 1
#define EPS_HIGH 2

/* One opened bulk endpoint. */
#define EHCI_MAX_PIPES 6

typedef struct {
    QUEUE_HEAD* queue;
    QTD* descriptor;
    boot_uint32_t address;       /* the device's, not the endpoint's */
    boot_uint8_t endpoint;
    int in;
    int used;
} EHCI_PIPE;

typedef struct {
    volatile boot_uint8_t* capability;
    volatile boot_uint8_t* operational;
    boot_uint32_t port_count;
    boot_uint32_t companion_count;
    int supports_64bit;
    int running;

    /* The head of the asynchronous schedule: an empty queue head pointing at
       itself, marked as the place the controller may reclaim bandwidth from.
       Transfers are linked in behind it and taken out again. */
    QUEUE_HEAD* async_head;
    QUEUE_HEAD* transfer_head;
    boot_uint8_t* descriptors;   /* slots 64 bytes apart - see `slot` below */
    boot_uint8_t* buffer;        /* one page for the data stage */

    /* Bulk endpoints, each with its own queue head permanently in the
       schedule. A bulk endpoint carries its data toggle across transfers -
       unlike a control endpoint, where each transfer starts over - so the
       queue head has to be the one that remembers it, and there has to be one
       per endpoint rather than one shared. */
    EHCI_PIPE pipes[EHCI_MAX_PIPES];
    boot_uint32_t pipe_count;

    boot_uint32_t connected[EHCI_MAX_PORTS];
    boot_uint32_t released;
} EHCI_CONTROLLER;

static EHCI_CONTROLLER controllers[EHCI_MAX_CONTROLLERS];
static boot_uint32_t controller_count;

static void survey_ports(EHCI_CONTROLLER* self);

/* One descriptor slot.
 *
 * Every structure the controller reads has to be 32-byte aligned, and the C
 * struct is 52 bytes - the 32 bytes the hardware defines plus the upper halves
 * of the five buffer pointers. Laying them out as an array puts the second one
 * at offset 52, which is aligned to four and to nothing else. The controller
 * does not report this; it reads a descriptor from the address with the low
 * bits masked off, finds a field boundary in the middle of a number, and the
 * transfer simply never completes.
 *
 * Sixty-four apart, then, which is aligned and leaves the padding unread. */
#define DESCRIPTOR_STRIDE 64

static QTD* slot(const EHCI_CONTROLLER* self, int index) {
    return (QTD*)(self->descriptors + index * DESCRIPTOR_STRIDE);
}

static void log(const char* text) { serial_write(text); }
static void log_dec(boot_uint64_t value) { serial_write_dec(value); }
static void log_hex(boot_uint64_t value) { serial_write_hex(value); }

static void log_controller(const EHCI_CONTROLLER* self) {
    log("EHCI");
    log_dec((boot_uint64_t)(self - controllers));
    log(": ");
}

static boot_uint32_t cap_read32(const EHCI_CONTROLLER* self,
                                boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(self->capability + offset);
}

static boot_uint32_t op_read32(const EHCI_CONTROLLER* self,
                               boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(self->operational + offset);
}

static void op_write32(const EHCI_CONTROLLER* self, boot_uint32_t offset,
                       boot_uint32_t value) {
    *(volatile boot_uint32_t*)(self->operational + offset) = value;
}

/* Change a port register without acknowledging anything by accident. */
static void port_write(const EHCI_CONTROLLER* self, boot_uint32_t port,
                       boot_uint32_t value) {
    op_write32(self, OP_PORTSC + port * 4, value & ~PORTSC_WRITE_CLEAR);
}

static boot_uint32_t port_read(const EHCI_CONTROLLER* self,
                               boot_uint32_t port) {
    return op_read32(self, OP_PORTSC + port * 4);
}

static int wait_clear(const EHCI_CONTROLLER* self, boot_uint32_t offset,
                      boot_uint32_t mask, boot_uint64_t timeout_ms) {
    boot_uint64_t start = timer_ticks();

    while (op_read32(self, offset) & mask) {
        timer_poll();
        if (timer_expired(start, timeout_ms)) return 0;
    }
    return 1;
}

/* ---- Taking the controller away from the firmware ------------------------
 *
 * The firmware has been driving this since power-on, so that a USB keyboard
 * could be used in the setup screens, and it does it by watching the registers
 * through a system management interrupt. Nothing about that is visible from
 * here: the machine simply stops for a moment, and the value just written is
 * not the value read back.
 *
 * The handshake is one bit each way. We set "OS owned" and wait for "BIOS
 * owned" to clear, which is the firmware finishing whatever it was doing and
 * standing down. Then every reason it had to be interrupted is switched off,
 * because a system management interrupt that still fires for a controller
 * nobody is watching is a machine that pauses for no reason.
 */
static void take_from_firmware(EHCI_CONTROLLER* self,
                               const PCI_DEVICE* controller,
                               boot_uint32_t capabilities) {
    boot_uint32_t offset = (capabilities >> HCCPARAMS_EECP_SHIFT) &
                           HCCPARAMS_EECP_MASK;
    boot_uint64_t start;
    boot_uint32_t legacy;
    int guard = 0;

    /* Below 0x40 is the standard PCI header, where no capability may live. */
    while (offset >= 0x40 && guard++ < 16) {
        legacy = pci_config_read(controller, (boot_uint8_t)offset);
        if ((legacy & 0xFF) == EHCI_CAPABILITY_LEGACY) break;
        offset = (legacy >> 8) & 0xFF;
    }
    if (offset < 0x40 || (legacy & 0xFF) != EHCI_CAPABILITY_LEGACY) return;

    if (!(legacy & LEGACY_BIOS_OWNED)) {
        /* Already ours, or never claimed. Still worth switching the
           interrupts off below. */
    } else {
        pci_config_write(controller, (boot_uint8_t)offset,
                         legacy | LEGACY_OS_OWNED);
        start = timer_ticks();
        for (;;) {
            legacy = pci_config_read(controller, (boot_uint8_t)offset);
            if (!(legacy & LEGACY_BIOS_OWNED)) break;
            timer_poll();
            if (timer_expired(start, 1000)) {
                log_controller(self);
                log("the firmware would not let go of the controller; "
                    "taking it anyway\n");
                /* Claiming it regardless is the lesser evil: the alternative
                   is a controller nobody drives on a machine whose only
                   keyboard is on it. */
                pci_config_write(controller, (boot_uint8_t)offset,
                                 LEGACY_OS_OWNED);
                break;
            }
        }
        log_controller(self);
        log("taken from the firmware\n");
    }

    /* The control/status register that follows says which events raise a
       system management interrupt. All of them, off. */
    pci_config_write(controller, (boot_uint8_t)(offset + 4), 0);
}

/* ---- Bringing it up ------------------------------------------------------ */

static int reset_controller(EHCI_CONTROLLER* self) {
    /* Stopped before reset: resetting a running controller is undefined, and
       "undefined" on this hardware means a machine that hangs. */
    boot_uint64_t start;

    op_write32(self, OP_USBCMD, op_read32(self, OP_USBCMD) & ~USBCMD_RUN);
    start = timer_ticks();
    while (!(op_read32(self, OP_USBSTS) & USBSTS_HALTED)) {
        timer_poll();
        if (timer_expired(start, 100)) break;
    }

    op_write32(self, OP_USBCMD, USBCMD_RESET);
    if (!wait_clear(self, OP_USBCMD, USBCMD_RESET, 250)) {
        log_controller(self);
        log("reset did not complete\n");
        return 0;
    }
    return 1;
}

/* An empty queue head that points at itself. The controller walks the async
   schedule forever; with nothing to do it arrives back here and goes round
   again, which is exactly what should happen. */
static void build_async_head(EHCI_CONTROLLER* self) {
    QUEUE_HEAD* head = self->async_head;
    QUEUE_HEAD* queue = self->transfer_head;

    memset(head, 0, sizeof(*head));
    memset(queue, 0, sizeof(*queue));

    /* Two queue heads in a ring: the head, and the one every transfer runs
       through. The second is idle whenever nothing is in flight - overlay not
       active, nothing to fetch - and the controller steps over it. */
    head->horizontal = (boot_uint32_t)(unsigned long long)queue | LINK_TYPE_QH;
    queue->horizontal = (boot_uint32_t)(unsigned long long)head | LINK_TYPE_QH;
    queue->characteristics = (EPS_HIGH << 12) | (1U << 14) | (64U << 16);
    queue->capabilities = (1U << 30);
    queue->overlay.next = LINK_TERMINATE;
    queue->overlay.alternate = LINK_TERMINATE;
    queue->overlay.token = 0;
    /* Bit 15 marks this as the head of the reclamation list, and exactly one
       queue head in the schedule must have it. Without it the controller has
       no way to tell a full circuit of an idle schedule from progress, and
       stops walking. */
    head->characteristics = (1U << 15) | (EPS_HIGH << 12) | (64U << 16);
    head->overlay.next = LINK_TERMINATE;
    head->overlay.alternate = LINK_TERMINATE;
    head->overlay.token = QTD_STATUS_HALTED;
}

int ehci_init(const PCI_DEVICE* controller) {
    EHCI_CONTROLLER* self;
    boot_uint32_t structural;
    boot_uint32_t capabilities;
    boot_uint64_t base;
    void* structures;

    if (controller_count >= EHCI_MAX_CONTROLLERS) return 0;
    base = (boot_uint64_t)(controller->bar[0] & ~0x0FU);
    if (!base || (controller->bar[0] & 1U)) return 0;

    self = &controllers[controller_count];
    memset(self, 0, sizeof(*self));
    self->capability = (volatile boot_uint8_t*)(unsigned long long)base;

    pci_enable_bus_mastering(controller);

    capabilities = cap_read32(self, CAP_HCCPARAMS);
    take_from_firmware(self, controller, capabilities);

    self->operational = self->capability +
                        (*(volatile boot_uint8_t*)self->capability);
    self->supports_64bit = (capabilities & HCCPARAMS_64BIT) != 0;

    structural = cap_read32(self, CAP_HCSPARAMS);
    self->port_count = structural & HCSPARAMS_PORTS;
    self->companion_count = (structural >> HCSPARAMS_COMPANIONS_SHIFT) &
                            HCSPARAMS_COMPANIONS_MASK;
    if (!self->port_count || self->port_count > EHCI_MAX_PORTS) {
        log_controller(self);
        log("claims ");
        log_dec(self->port_count);
        log(" ports, which cannot be right\n");
        return 0;
    }

    log_controller(self);
    log("version ");
    log_hex(*(volatile boot_uint16_t*)(self->capability + 2));
    log(" at ");
    log_hex(base);
    log("\n");
    log_controller(self);
    log_dec(self->port_count);
    log(" port(s), ");
    log_dec(self->companion_count);
    log(" companion controller(s)\n");

    /* Interrupts off before the reset, and left off: nothing here is
       interrupt driven, and an unhandled one from a device the firmware was
       watching is a machine that stops. */
    op_write32(self, OP_USBINTR, 0);
    if (!reset_controller(self)) return 0;
    op_write32(self, OP_USBINTR, 0);

    /* Everything the controller reads by address, from low memory. EHCI
       addresses are 32 bits with a separate segment register for the upper
       half, and one segment for all of it is only correct if all of it is in
       the same four gigabytes - which it is, below four. */
    structures = alloc_page_low();
    self->buffer = (boot_uint8_t*)alloc_page_low();
    if (!structures || !self->buffer) {
        log_controller(self);
        log("out of low memory for the schedule\n");
        return 0;
    }
    memset(structures, 0, PAGE_SIZE);
    memset(self->buffer, 0, PAGE_SIZE);
    /* Spaced generously and all of it 64-byte aligned. A queue head is 68
       bytes once the upper buffer halves are counted, so 64 apart would have
       them overlapping. */
    self->async_head = (QUEUE_HEAD*)structures;
    self->transfer_head = (QUEUE_HEAD*)((boot_uint8_t*)structures + 128);
    self->descriptors = (boot_uint8_t*)structures + 256;

    build_async_head(self);

    op_write32(self, OP_CTRLDSSEGMENT, 0);
    op_write32(self, OP_PERIODICLISTBASE, 0);
    op_write32(self, OP_ASYNCLISTADDR,
               (boot_uint32_t)(unsigned long long)self->async_head);
    op_write32(self, OP_USBCMD, USBCMD_RUN | USBCMD_ASYNC_ENABLE);

    {
        boot_uint64_t start = timer_ticks();
        while (op_read32(self, OP_USBSTS) & USBSTS_HALTED) {
            timer_poll();
            if (timer_expired(start, 100)) {
                log_controller(self);
                log("would not start\n");
                return 0;
            }
        }
    }

    /* And the ports are ours rather than the companions'. Until this is
       written every port is routed to a USB 1 controller, whatever is plugged
       into it - which is why an EHCI driver that skips this line finds an
       empty machine. */
    /* The controller acknowledges the asynchronous schedule separately from
       being told to run it, and the gap between the two is the one place a
       transfer can be queued into a schedule nobody is walking. */
    {
        boot_uint64_t start = timer_ticks();
        while (!(op_read32(self, OP_USBSTS) & USBSTS_ASYNC_RUNNING)) {
            timer_poll();
            if (timer_expired(start, 100)) {
                log_controller(self);
                log("the asynchronous schedule never started\n");
                return 0;
            }
        }
    }

    op_write32(self, OP_CONFIGFLAG, 1);
    timer_wait(5);

    self->running = 1;
    controller_count++;
    survey_ports(self);
    return 1;
}

boot_uint32_t ehci_controller_count(void) { return controller_count; }

boot_uint32_t ehci_port_count(void) {
    boot_uint32_t total = 0;

    for (boot_uint32_t index = 0; index < controller_count; index++)
        total += controllers[index].port_count;
    return total;
}

boot_uint32_t ehci_ports_connected(void) {
    boot_uint32_t total = 0;

    for (boot_uint32_t index = 0; index < controller_count; index++)
        for (boot_uint32_t port = 0; port < controllers[index].port_count; port++)
            total += controllers[index].connected[port];
    return total;
}

boot_uint32_t ehci_ports_released(void) {
    boot_uint32_t total = 0;

    for (boot_uint32_t index = 0; index < controller_count; index++)
        total += controllers[index].released;
    return total;
}

/* ---- Transfers -----------------------------------------------------------
 *
 * One control transfer at a time, built out of three descriptors and hung off
 * a queue head that is linked into the asynchronous schedule for as long as it
 * takes. There is no event to wait for: the controller clears the Active bit
 * in the token when it is done with a descriptor, and the only way to find out
 * is to keep reading it.
 *
 * A transfer is three stages and the toggles are not a detail. Setup always
 * goes out with toggle 0; the data stage starts at 1 and the controller
 * alternates from there on its own; the status stage is always 1 and always
 * the opposite direction from the data. Get the last one wrong and the device
 * answers the wrong packet, which reads as a device that does not respond.
 */

/* Fill in a descriptor's buffer pointers. EHCI splits a transfer across up to
   five pages and the first may start anywhere inside one, which is the whole
   of the arithmetic here. */
static void describe_buffer(QTD* qtd, boot_uint64_t address,
                            boot_uint32_t bytes) {
    for (int page = 0; page < 5; page++) {
        qtd->buffer[page] = 0;
        qtd->buffer_high[page] = 0;
    }
    if (!bytes) return;
    for (int page = 0; page < 5 && bytes; page++) {
        boot_uint32_t in_page = PAGE_SIZE - (boot_uint32_t)(address & 0xFFF);
        boot_uint32_t chunk = bytes < in_page ? bytes : in_page;

        qtd->buffer[page] = (boot_uint32_t)address;
        qtd->buffer_high[page] = (boot_uint32_t)(address >> 32);
        address += chunk;
        bytes -= chunk;
    }
}

/* Wait for a chain to finish, and say what went wrong if it did.
   Returns 1 when every descriptor completed without error. */
static int await_transfer(EHCI_CONTROLLER* self, QTD* last,
                          boot_uint64_t timeout_ms) {
    boot_uint64_t start = timer_ticks();
    boot_uint32_t status;

    for (;;) {
        status = last->token & 0xFF;
        if (!(status & QTD_STATUS_ACTIVE)) break;
        /* The overlay halting is the other way a transfer ends: the queue
           head stops and the descriptor it was working on never clears. */
        if (self->transfer_head->overlay.token & QTD_STATUS_HALTED) break;
        timer_poll();
        if (timer_expired(start, timeout_ms)) {
            log_controller(self);
            log("transfer timed out\n");
            return 0;
        }
    }

    status = self->transfer_head->overlay.token & 0xFF;
    if (status & QTD_STATUS_HALTED) {
        log_controller(self);
        /* Named separately because they mean completely different things: a
           stall is the device refusing, and the others are the wire. */
        if (status & QTD_STATUS_BABBLE) log("transfer: the device babbled\n");
        else if (status & QTD_STATUS_BUFFER_ERROR) log("transfer: buffer error\n");
        else if (status & QTD_STATUS_TRANSACTION) log("transfer: no reply\n");
        else log("transfer: the device refused it\n");
        return 0;
    }
    return 1;
}

/* The same wait, against a pipe's own queue head rather than the shared one. */
static int await_pipe(EHCI_CONTROLLER* self, EHCI_PIPE* pipe, QTD* last,
                      boot_uint64_t timeout_ms) {
    boot_uint64_t start = timer_ticks();
    boot_uint32_t status;

    for (;;) {
        status = last->token & 0xFF;
        if (!(status & QTD_STATUS_ACTIVE)) break;
        if (pipe->queue->overlay.token & QTD_STATUS_HALTED) break;
        timer_poll();
        if (timer_expired(start, timeout_ms)) {
            log_controller(self);
            log("bulk transfer timed out\n");
            return 0;
        }
    }
    if (pipe->queue->overlay.token & QTD_STATUS_HALTED) {
        log_controller(self);
        log("bulk transfer: the device refused it\n");
        return 0;
    }
    return 1;
}

/* One control transfer. `buffer` is our own low page, so the caller copies in
   and out of it rather than handing us anything. */
static int control_transfer(EHCI_CONTROLLER* self, boot_uint32_t address,
                            boot_uint32_t speed, boot_uint32_t packet_size,
                            boot_uint8_t request_type, boot_uint8_t request,
                            boot_uint16_t value, boot_uint16_t index,
                            boot_uint32_t length) {
    QUEUE_HEAD* queue = self->transfer_head;
    QTD* setup = slot(self, 0);
    QTD* data = slot(self, 1);
    QTD* status = slot(self, 2);
    boot_uint8_t* packet = self->buffer + PAGE_SIZE - 8;
    int reading = (request_type & 0x80) != 0;
    int ok;

    /* The eight bytes of the setup packet, in the order USB puts them. */
    packet[0] = request_type;
    packet[1] = request;
    packet[2] = (boot_uint8_t)value;
    packet[3] = (boot_uint8_t)(value >> 8);
    packet[4] = (boot_uint8_t)index;
    packet[5] = (boot_uint8_t)(index >> 8);
    packet[6] = (boot_uint8_t)length;
    packet[7] = (boot_uint8_t)(length >> 8);

    memset(setup, 0, sizeof(*setup));
    memset(data, 0, sizeof(*data));
    memset(status, 0, sizeof(*status));
    setup->next = (boot_uint32_t)(unsigned long long)
                  (length ? data : status);
    setup->alternate = LINK_TERMINATE;
    setup->token = QTD_TOKEN(QTD_STATUS_ACTIVE, QTD_PID_SETUP, 8, 0);
    describe_buffer(setup, (boot_uint64_t)(unsigned long long)packet, 8);

    if (length) {
        data->next = (boot_uint32_t)(unsigned long long)status;
        data->alternate = LINK_TERMINATE;
        data->token = QTD_TOKEN(QTD_STATUS_ACTIVE,
                                reading ? QTD_PID_IN : QTD_PID_OUT, length, 1);
        describe_buffer(data, (boot_uint64_t)(unsigned long long)self->buffer,
                        length);
    }

    status->next = LINK_TERMINATE;
    status->alternate = LINK_TERMINATE;
    status->token = QTD_TOKEN(QTD_STATUS_ACTIVE,
                              reading ? QTD_PID_OUT : QTD_PID_IN, 0, 1);
    describe_buffer(status, 0, 0);

    /* The queue head is rewritten in place rather than rebuilt, because it is
       linked into a schedule the controller is walking right now. Everything
       here is written while it is idle and read only once the overlay goes
       active, which is the last line of it. */
    /* Bit 14 says the toggle comes from the descriptors rather than from the
       queue head, which is what a control transfer needs: its three stages
       have toggles that do not follow one from the next. */
    queue->characteristics = (address & 0x7F) |
                             (speed << 12) | (1U << 14) |
                             ((packet_size & 0x7FF) << 16);
    /* One transaction per microframe. A full or low speed device would also
       need a split-transaction mask and the address of the hub translating for
       it; there is no hub driver on this controller yet, and a slow device on
       a root port has already been handed to a companion by the time we get
       here. */
    queue->capabilities = (1U << 30);
    queue->current = 0;
    queue->overlay.alternate = LINK_TERMINATE;
    queue->overlay.token = 0;
    for (int page = 0; page < 5; page++) {
        queue->overlay.buffer[page] = 0;
        queue->overlay.buffer_high[page] = 0;
    }
    queue->overlay.next = (boot_uint32_t)(unsigned long long)setup;

    /* And now it is live. The queue head is already in the schedule and stays
       there; what starts a transfer is the overlay being pointed at a
       descriptor with its Active bit set.
     *
     * Linking and unlinking around each transfer was the first shape of this
     * and it worked exactly once. Taking a queue head out of a running
     * schedule is not a pointer assignment: the controller may be inside it,
     * and the handshake for finding out it has left - a doorbell, and an
     * acknowledgement that arrives as an interrupt status bit - exists
     * precisely because there is no other way to know. A permanently linked
     * queue head that sits idle between transfers has none of that problem:
     * an idle one has nothing to fetch and is stepped over. */
    ok = await_transfer(self, status, 1000);

    /* Idle again, so the next pass of the schedule finds nothing to do here
       rather than the descriptors from the transfer that just finished. */
    queue->overlay.next = LINK_TERMINATE;
    queue->overlay.alternate = LINK_TERMINATE;
    queue->overlay.token = 0;
    return ok;
}

/* ---- Ports and what is on them ------------------------------------------- */

#define USB_DESCRIPTOR_DEVICE 1
#define USB_GET_DESCRIPTOR 6
#define USB_SET_ADDRESS 5

/* Reset one port and report whether a high speed device came up on it.
 *
 * Three outcomes, and only one of them is ours. A low speed device announces
 * itself before the reset by driving the line into K-state; a full speed one
 * is only distinguishable afterwards, by the port failing to enable. Both are
 * handed to the companion controller, which is a real controller on the same
 * socket that this system does not drive - so the device is present, correct,
 * and invisible. That is worth a line in the log rather than silence. */
static int reset_port(EHCI_CONTROLLER* self, boot_uint32_t port) {
    boot_uint32_t status = port_read(self, port);
    boot_uint64_t start;

    if (((status >> PORTSC_LINE_STATUS_SHIFT) & PORTSC_LINE_STATUS_MASK) ==
        LINE_STATUS_LOW_SPEED) {
        log_controller(self);
        log("port ");
        log_dec(port + 1);
        log(" has a low speed device; handing it to the companion\n");
        port_write(self, port, status | PORTSC_OWNER);
        self->released++;
        return 0;
    }

    port_write(self, port, (status & ~PORTSC_ENABLED) | PORTSC_RESET);
    /* USB asks for 50 ms of reset on a root port. Shorter works on most
       devices and not on all of them, and the ones it fails on fail at the
       first descriptor read - which looks like a broken device. */
    timer_wait(50);
    port_write(self, port, port_read(self, port) & ~PORTSC_RESET);

    /* The controller clears the reset bit itself once the port has settled,
       and the spec allows it two milliseconds. */
    start = timer_ticks();
    while (port_read(self, port) & PORTSC_RESET) {
        timer_poll();
        if (timer_expired(start, 100)) {
            log_controller(self);
            log("port ");
            log_dec(port + 1);
            log(" would not come out of reset\n");
            return 0;
        }
    }

    status = port_read(self, port);
    if (!(status & PORTSC_ENABLED)) {
        log_controller(self);
        log("port ");
        log_dec(port + 1);
        log(" has a full speed device; handing it to the companion\n");
        port_write(self, port, status | PORTSC_OWNER);
        self->released++;
        return 0;
    }
    return 1;
}

/* Enumerate as far as the device's own name, which is as far as this goes
   today: the class drivers live in the xHCI file and speak its rings. Saying
   what is on a port is the half that proves the controller works. */
static void attach_device(EHCI_CONTROLLER* self, boot_uint32_t port,
                          boot_uint32_t address) {
    boot_uint32_t packet_size = 64;   /* every high speed device, always */
    boot_uint16_t vendor;
    boot_uint16_t product;

    if (!control_transfer(self, 0, EPS_HIGH, packet_size,
                          0x80, USB_GET_DESCRIPTOR,
                          USB_DESCRIPTOR_DEVICE << 8, 0, 18)) {
        log_controller(self);
        log("the device would not describe itself\n");
        return;
    }

    /* A cleared Active bit says the controller finished, not that the device
       said anything sensible. Eighteen bytes of type 1 is what a device
       descriptor is; anything else means the transfer completed against
       memory rather than against hardware. */
    if (self->buffer[0] != 18 || self->buffer[1] != USB_DESCRIPTOR_DEVICE) {
        log_controller(self);
        log("the device descriptor is not one (length ");
        log_dec(self->buffer[0]);
        log(", type ");
        log_dec(self->buffer[1]);
        log(")\n");
        return;
    }

    vendor = (boot_uint16_t)(self->buffer[8] | (self->buffer[9] << 8));
    product = (boot_uint16_t)(self->buffer[10] | (self->buffer[11] << 8));

    if (!control_transfer(self, 0, EPS_HIGH, packet_size,
                          0x00, USB_SET_ADDRESS, (boot_uint16_t)address, 0, 0)) {
        log_controller(self);
        log("the device would not take an address\n");
        return;
    }
    /* The device is allowed two milliseconds to start answering to it. */
    timer_wait(5);

    log_controller(self);
    log("port ");
    log_dec(port + 1);
    log(": device ");
    log_hex(vendor);
    log(":");
    log_hex(product);
    log(" class ");
    log_dec(self->buffer[4]);
    log(", address ");
    log_dec(address);
    log("\n");
}

static void survey_ports(EHCI_CONTROLLER* self) {
    boot_uint32_t address = 1;

    for (boot_uint32_t port = 0; port < self->port_count; port++) {
        boot_uint32_t status = port_read(self, port);

        /* A port with no power switch reads as powered already; one with a
           switch has to be asked, and a port with no power is
           indistinguishable from a port with nothing in it. */
        if (!(status & PORTSC_POWER)) {
            port_write(self, port, status | PORTSC_POWER);
            timer_wait(20);
            status = port_read(self, port);
        }
        if (!(status & PORTSC_CONNECTED)) continue;

        self->connected[port] = 1;
        if (!reset_port(self, port)) {
            self->connected[port] = 0;   /* somebody else's now */
            continue;
        }
        attach_device(self, port, address++);
    }
}

/* ---- Bulk endpoints ------------------------------------------------------
 *
 * A control transfer is three stages with toggles decided by the protocol, and
 * the queue head is told to take them from the descriptors. A bulk endpoint is
 * the opposite: one stage, and a toggle that carries from one transfer to the
 * next for as long as the endpoint is open. That difference is why each of
 * these gets a queue head of its own and keeps it - the queue head is the
 * thing that remembers, and sharing one would have two endpoints trading a
 * toggle neither of them can then get right.
 */

/* Open one. Returns a handle, or -1. */
int ehci_open_bulk(boot_uint32_t controller, boot_uint32_t address,
                   boot_uint8_t endpoint, int in, boot_uint32_t packet_size) {
    EHCI_CONTROLLER* self;
    EHCI_PIPE* pipe = (EHCI_PIPE*)0;
    boot_uint32_t index;

    if (controller >= controller_count) return -1;
    self = &controllers[controller];

    for (index = 0; index < EHCI_MAX_PIPES; index++)
        if (!self->pipes[index].used) { pipe = &self->pipes[index]; break; }
    if (!pipe) {
        log_controller(self);
        log("no room for another endpoint\n");
        return -1;
    }

    /* Two structures per pipe out of the same page the schedule lives in:
       a queue head and the single descriptor its transfers run through. */
    pipe->queue = (QUEUE_HEAD*)(self->descriptors +
                                (4 + index * 3) * DESCRIPTOR_STRIDE);
    pipe->descriptor = (QTD*)(self->descriptors +
                              (6 + index * 3) * DESCRIPTOR_STRIDE);
    pipe->address = address;
    pipe->endpoint = endpoint;
    pipe->in = in;

    memset(pipe->queue, 0, sizeof(*pipe->queue));
    memset(pipe->descriptor, 0, sizeof(*pipe->descriptor));
    /* No bit 14 here, which is the whole point: the toggle lives in the queue
       head and the controller carries it forward itself. */
    pipe->queue->characteristics = (address & 0x7F) |
                                   ((boot_uint32_t)(endpoint & 0x0F) << 8) |
                                   (EPS_HIGH << 12) |
                                   ((packet_size & 0x7FF) << 16);
    pipe->queue->capabilities = (1U << 30);
    pipe->queue->overlay.next = LINK_TERMINATE;
    pipe->queue->overlay.alternate = LINK_TERMINATE;
    pipe->queue->overlay.token = 0;

    /* Into the ring behind the head, once, and it stays there. */
    pipe->queue->horizontal = self->async_head->horizontal;
    self->async_head->horizontal =
        (boot_uint32_t)(unsigned long long)pipe->queue | LINK_TYPE_QH;

    pipe->used = 1;
    if (index >= self->pipe_count) self->pipe_count = index + 1;
    return (int)index;
}

/* One bulk transfer through an open endpoint, waited for.
 *
 * `buffer` must be the controller's own page - the caller copies through it,
 * the same trade the other two storage drivers make, and for the same reason:
 * a descriptor's buffer may not cross the wrong boundary and the layer above
 * hands down pointers from anywhere.
 *
 * Returns bytes transferred, or -1. */
int ehci_bulk(boot_uint32_t controller, int handle, boot_uint32_t offset,
              boot_uint32_t length, boot_uint64_t timeout_ms) {
    EHCI_CONTROLLER* self;
    EHCI_PIPE* pipe;
    QTD* qtd;
    boot_uint32_t remaining;

    if (controller >= controller_count || handle < 0 ||
        handle >= EHCI_MAX_PIPES)
        return -1;
    self = &controllers[controller];
    pipe = &self->pipes[handle];
    if (!pipe->used || offset + length > PAGE_SIZE) return -1;

    qtd = pipe->descriptor;
    memset(qtd, 0, sizeof(*qtd));
    qtd->next = LINK_TERMINATE;
    qtd->alternate = LINK_TERMINATE;
    /* Toggle zero here is not the toggle used: bit 14 is clear in the queue
       head, so the controller supplies its own and this field is ignored. */
    qtd->token = QTD_TOKEN(QTD_STATUS_ACTIVE,
                           pipe->in ? QTD_PID_IN : QTD_PID_OUT, length, 0);
    describe_buffer(qtd, (boot_uint64_t)(unsigned long long)
                    (self->buffer + offset), length);

    pipe->queue->overlay.alternate = LINK_TERMINATE;
    pipe->queue->overlay.token = 0;
    for (int page = 0; page < 5; page++) {
        pipe->queue->overlay.buffer[page] = 0;
        pipe->queue->overlay.buffer_high[page] = 0;
    }
    pipe->queue->current = 0;
    pipe->queue->overlay.next = (boot_uint32_t)(unsigned long long)qtd;

    if (!await_pipe(self, pipe, qtd, timeout_ms)) return -1;

    /* What is left is what did not move: a short packet is normal on a read
       and is how a device says "that was all of it". */
    remaining = (qtd->token >> 16) & 0x7FFF;
    pipe->queue->overlay.next = LINK_TERMINATE;
    return (int)(length - remaining);
}
