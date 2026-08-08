#include "nvme.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "paging.h"
#include "block.h"

/* NVMe.
 *
 * The third storage driver, and the one that took least effort, because the
 * shape was already familiar: a ring of commands, a doorbell saying how far it
 * has been filled, and a second ring of completions with a phase bit marking
 * whose entries are whose. That is the xHCI cycle bit under a different name,
 * and the same discipline applies - a controller set up incorrectly does not
 * complain, it just never completes anything.
 *
 * Built in slices with an observable at the end of each. Identify Controller
 * is the one that matters: a model string coming back proves the doorbells,
 * the queue layout and the phase bit are all right together, before a single
 * sector is at stake.
 */

/* Controller current->registers, at BAR0. */
#define REG_CAP 0x00            /* capabilities, 64-bit */
#define REG_VS 0x08             /* version */
#define REG_INTMS 0x0C
#define REG_INTMC 0x10
#define REG_CC 0x14             /* configuration */
#define REG_CSTS 0x1C           /* status */
#define REG_AQA 0x24            /* admin queue attributes */
#define REG_ASQ 0x28            /* admin submission queue base, 64-bit */
#define REG_ACQ 0x30            /* admin completion queue base, 64-bit */
#define REG_DOORBELL_BASE 0x1000

#define CC_ENABLE 0x00000001U
#define CC_CSS_NVM (0u << 4)
#define CC_MPS_SHIFT 7
#define CC_AMS_ROUND_ROBIN (0u << 11)
#define CC_IOSQES_SHIFT 16      /* log2 of the submission entry size */
#define CC_IOCQES_SHIFT 20      /* log2 of the completion entry size */

#define CSTS_READY 0x00000001U
#define CSTS_FATAL 0x00000002U

/* Admin opcodes. */
#define ADMIN_CREATE_SQ 0x01
#define ADMIN_CREATE_CQ 0x05
#define ADMIN_IDENTIFY 0x06

/* I/O opcodes. Write really is 1 and read 2 - the opposite way round from the
   habit of putting read first, and an easy thing to transpose. */
#define IO_WRITE 0x01
#define IO_READ 0x02

#define IDENTIFY_NAMESPACE 0
#define IDENTIFY_CONTROLLER 1

/* Queue depths. Small on purpose: commands are issued one at a time and
   waited for, so depth buys nothing and every entry is memory the controller
   must be able to reach. */
#define ADMIN_QUEUE_SIZE 8
#define IO_QUEUE_SIZE 32
#define IO_QUEUE_ID 1

/* Enough for the register block and the doorbell array of any controller. */
#define NVME_WINDOW_SIZE 0x2000

/* A submission queue entry: 64 bytes, and the layout is fixed by the spec
   down to the dword. */
typedef struct __attribute__((packed)) {
    boot_uint32_t command;        /* opcode, flags, and the command identifier */
    boot_uint32_t namespace_id;
    boot_uint64_t reserved;
    boot_uint64_t metadata;
    boot_uint64_t prp1;
    boot_uint64_t prp2;
    boot_uint32_t dword10;
    boot_uint32_t dword11;
    boot_uint32_t dword12;
    boot_uint32_t dword13;
    boot_uint32_t dword14;
    boot_uint32_t dword15;
} NVME_COMMAND;

/* And a completion queue entry: 16 bytes, with the phase bit in the low bit of
   the status field. */
typedef struct __attribute__((packed)) {
    boot_uint32_t result;
    boot_uint32_t reserved;
    boot_uint16_t submission_head;
    boot_uint16_t submission_id;
    boot_uint16_t command_id;
    boot_uint16_t status;         /* bit 0 phase, bits 1-15 the status code */
} NVME_COMPLETION;

typedef struct {
    NVME_COMMAND* commands;
    NVME_COMPLETION* completions;
    boot_uint32_t size;
    boot_uint32_t submission_tail;
    boot_uint32_t completion_head;
    boot_uint32_t phase;          /* what the controller stamps as "new" */
    boot_uint32_t id;
} NVME_QUEUE;

/* One namespace, which is what the block layer above sees as a disk. A
   consumer drive has exactly one; enterprise hardware carves a drive into
   several, and they differ in size and in block size. */
#define NVME_MAX_NAMESPACES 4

typedef struct {
    boot_uint32_t id;
    boot_uint64_t sectors;
    boot_uint32_t sector_size;
    int used;
} NVME_NAMESPACE;

/* One controller and everything that belongs to it.
 *
 * All of this was file-scope, which said that a machine has one NVMe drive.
 * Two M.2 sockets is an ordinary desktop, and the second drive was invisible -
 * not failing, not reported, simply never looked at. */
#define NVME_MAX_DRIVES 4

typedef struct {
    volatile boot_uint8_t* registers;
    boot_uint32_t doorbell_stride;
    boot_uint64_t ready_timeout_ms;
    boot_uint32_t max_queue_entries;

    NVME_QUEUE admin_queue;
    NVME_QUEUE io_queue;

    boot_uint8_t* identify_buffer;   /* 4 KiB, for Identify results */
    boot_uint8_t* bounce;            /* 4 KiB of transfer buffer */

    NVME_NAMESPACE namespaces[NVME_MAX_NAMESPACES];
    boot_uint32_t namespace_count;
    int ready;
} NVME_DRIVE;

static NVME_DRIVE drives[NVME_MAX_DRIVES];
static boot_uint32_t drive_count;

/* Which drive the register and queue helpers below are talking to.
 *
 * A pointer rather than a parameter threaded through thirty functions, and
 * that is a deliberate trade: nothing here is re-entrant and nothing here is
 * interrupt driven - every completion is polled by the caller that submitted
 * it - so there is exactly one conversation happening at a time. The two ways
 * in, bringing a controller up and a block transfer, both set it first. */
static NVME_DRIVE* current;

/* Which drive a namespace belongs to. Found by range rather than stored,
   because a namespace lives inside its drive's own table and a back pointer
   would be a second thing to keep true. */
static NVME_DRIVE* drive_of(const NVME_NAMESPACE* space) {
    for (boot_uint32_t index = 0; index < drive_count; index++)
        if (space >= drives[index].namespaces &&
            space < drives[index].namespaces + NVME_MAX_NAMESPACES)
            return &drives[index];
    return (NVME_DRIVE*)0;
}

static void log(const char* text) {
    serial_write(text);
}

static void log_dec(boot_uint64_t value) {
    serial_write_dec(value);
}

static void log_hex(boot_uint64_t value) {
    serial_write_hex(value);
}

static boot_uint32_t read32(boot_uint32_t offset) {
    return *(volatile boot_uint32_t*)(current->registers + offset);
}

static void write32(boot_uint32_t offset, boot_uint32_t value) {
    *(volatile boot_uint32_t*)(current->registers + offset) = value;
}

/* The 64-bit current->registers are read and written as two dwords. The spec permits
   both widths, and halves work everywhere - a single 64-bit access to a
   register block that only decodes 32 bits does not. */
static boot_uint64_t read64(boot_uint32_t offset) {
    return (boot_uint64_t)read32(offset) |
           ((boot_uint64_t)read32(offset + 4) << 32);
}

static void write64(boot_uint32_t offset, boot_uint64_t value) {
    write32(offset, (boot_uint32_t)value);
    write32(offset + 4, (boot_uint32_t)(value >> 32));
}

/* Doorbells live past the register block, two per queue - submission tail
   first, then completion head - spaced by a stride the controller declares. */
static void ring_submission(const NVME_QUEUE* queue) {
    write32(REG_DOORBELL_BASE + (queue->id * 2) * current->doorbell_stride,
            queue->submission_tail);
}

static void ring_completion(const NVME_QUEUE* queue) {
    write32(REG_DOORBELL_BASE + (queue->id * 2 + 1) * current->doorbell_stride,
            queue->completion_head);
}

/* Give a queue its two rings. Both must be page-aligned and physically
   contiguous, which is exactly what the page allocator hands out. */
static int queue_init(NVME_QUEUE* queue, boot_uint32_t id, boot_uint32_t size) {
    queue->commands = (NVME_COMMAND*)alloc_page();
    queue->completions = (NVME_COMPLETION*)alloc_page();
    if (!queue->commands || !queue->completions) return 0;
    memset(queue->commands, 0, PAGE_SIZE);
    memset(queue->completions, 0, PAGE_SIZE);
    queue->size = size;
    queue->submission_tail = 0;
    queue->completion_head = 0;
    /* The controller writes 1 into the phase bit of the first pass, so that is
       what "this entry is new" means until the ring wraps. */
    queue->phase = 1;
    queue->id = id;
    return 1;
}

static const char* status_name(boot_uint32_t status) {
    switch (status & 0x7FF) {
    case 0x000: return "success";
    case 0x001: return "invalid command opcode";
    case 0x002: return "invalid field";
    case 0x004: return "data transfer error";
    case 0x00B: return "invalid namespace or format";
    case 0x081: return "invalid LBA range";
    case 0x180: return "LBA out of range";
    case 0x181: return "capacity exceeded";
    default: return "other";
    }
}

/* Put one command on a queue and wait for its completion.
 *
 * Synchronous on purpose: nothing in a DOS-like system issues two disk
 * commands at once, and a single outstanding command means the completion
 * being waited for is unambiguously the one just submitted.
 *
 * Returns 1 on success, and fills `result` from the completion's first dword
 * when asked. */
static int submit(NVME_QUEUE* queue, NVME_COMMAND* command,
                  boot_uint32_t* result, boot_uint64_t timeout_ms) {
    NVME_COMPLETION* completion;
    boot_uint32_t identifier = queue->submission_tail;
    boot_uint64_t start;
    boot_uint32_t status;

    /* The command identifier goes in the top half of the first dword; using
       the slot index means a completion names the slot it came from. */
    command->command = (command->command & 0x0000FFFFU) | (identifier << 16);
    queue->commands[queue->submission_tail] = *command;

    queue->submission_tail = (queue->submission_tail + 1) % queue->size;
    ring_submission(queue);

    completion = &queue->completions[queue->completion_head];
    start = timer_ticks();
    while ((completion->status & 1u) != queue->phase) {
        if (timer_expired(start, timeout_ms)) {
            log("NVME: command produced no completion\n");
            return 0;
        }
    }

    status = (boot_uint32_t)(completion->status >> 1);
    if (result) *result = completion->result;

    if (++queue->completion_head == queue->size) {
        queue->completion_head = 0;
        queue->phase ^= 1;
    }
    ring_completion(queue);

    if (status) {
        log("NVME: command failed: ");
        log(status_name(status));
        log(" (status ");
        log_hex(status);
        log(")\n");
        return 0;
    }
    return 1;
}

/* Stop the controller and wait for it to admit it has stopped. Enabling a
   controller that is already enabled is undefined, and firmware that booted
   from this drive left it running. */
static int disable_controller(void) {
    boot_uint64_t start;

    if (!(read32(REG_CC) & CC_ENABLE)) return 1;
    write32(REG_CC, read32(REG_CC) & ~CC_ENABLE);

    start = timer_ticks();
    while (read32(REG_CSTS) & CSTS_READY) {
        if (timer_expired(start, current->ready_timeout_ms)) {
            log("NVME: controller would not disable\n");
            return 0;
        }
    }
    return 1;
}

static int enable_controller(void) {
    boot_uint64_t start;
    boot_uint32_t configuration;

    /* Entry sizes are declared as powers of two: 64-byte commands, 16-byte
       completions. Getting these wrong makes the controller read commands at
       the wrong stride, which looks exactly like a controller that ignores
       the doorbell. */
    configuration = CC_ENABLE | CC_CSS_NVM | CC_AMS_ROUND_ROBIN |
                    (0u << CC_MPS_SHIFT) |      /* 4 KiB pages, as we use */
                    (6u << CC_IOSQES_SHIFT) |
                    (4u << CC_IOCQES_SHIFT);
    write32(REG_CC, configuration);

    start = timer_ticks();
    for (;;) {
        boot_uint32_t status = read32(REG_CSTS);
        if (status & CSTS_READY) return 1;
        if (status & CSTS_FATAL) {
            log("NVME: controller reported a fatal error while starting\n");
            return 0;
        }
        if (timer_expired(start, current->ready_timeout_ms)) {
            log("NVME: controller never became ready\n");
            return 0;
        }
    }
}

/* Print a space-padded field from an Identify result. They are not
   terminated, so trailing blanks come off. */
static void log_padded(const boot_uint8_t* text, boot_uint32_t length) {
    char buffer[41];
    boot_uint32_t used = length < 40 ? length : 40;

    while (used && text[used - 1] == ' ') used--;
    for (boot_uint32_t index = 0; index < used; index++)
        buffer[index] = (char)text[index];
    buffer[used] = 0;
    log(buffer);
}

static int identify(boot_uint32_t selector, boot_uint32_t nsid) {
    NVME_COMMAND command;

    memset(&command, 0, sizeof(command));
    command.command = ADMIN_IDENTIFY;
    command.namespace_id = nsid;
    command.prp1 = (boot_uint64_t)(unsigned long long)current->identify_buffer;
    command.dword10 = selector;

    memset(current->identify_buffer, 0, PAGE_SIZE);
    return submit(&current->admin_queue, &command, 0, 5000);
}

/* The checkpoint. A model string coming back means the queues, the doorbells
   and the phase bit are all correct together, and every later failure is about
   namespaces or transfers rather than about the controller. */
static int identify_controller(void) {
    if (!identify(IDENTIFY_CONTROLLER, 0)) {
        log("NVME: Identify Controller failed\n");
        return 0;
    }
    log("NVME: ");
    log_padded(current->identify_buffer + 24, 40);      /* model */
    log(", firmware ");
    log_padded(current->identify_buffer + 64, 8);
    log(", ");
    log_dec((boot_uint32_t)current->identify_buffer[516] |
            ((boot_uint32_t)current->identify_buffer[517] << 8) |
            ((boot_uint32_t)current->identify_buffer[518] << 16) |
            ((boot_uint32_t)current->identify_buffer[519] << 24));
    log(" namespace(s)\n");
    return 1;
}

/* Read the first namespace's size and block size.
 *
 * A namespace declares several possible formats and which one it is currently
 * using; the block size is the base-two logarithm held in that format, not a
 * byte count. Reading the wrong format's entry gives a plausible-looking
 * number that is wrong by a factor of eight. */
static int identify_namespace(boot_uint32_t nsid, NVME_NAMESPACE* out) {
    boot_uint32_t format_index;
    boot_uint32_t format;
    boot_uint32_t data_shift;
    boot_uint64_t namespace_sectors;
    boot_uint32_t namespace_sector_size;

    if (!identify(IDENTIFY_NAMESPACE, nsid)) {
        log("NVME: Identify Namespace failed\n");
        return 0;
    }

    namespace_sectors = 0;
    for (int byte = 7; byte >= 0; byte--)
        namespace_sectors = (namespace_sectors << 8) | current->identify_buffer[byte];

    /* A namespace that is not there answers with zeroes rather than an error,
       which is the controller saying "no such thing" in the only way the
       command allows. Asked before the format is looked at, because a format
       of zero is what that same answer looks like from one field down - and
       reported as absence rather than as a driver that cannot cope. */
    if (!namespace_sectors) return 0;

    format_index = current->identify_buffer[26] & 0x0F;   /* FLBAS */
    format = (boot_uint32_t)current->identify_buffer[128 + format_index * 4] |
             ((boot_uint32_t)current->identify_buffer[129 + format_index * 4] << 8) |
             ((boot_uint32_t)current->identify_buffer[130 + format_index * 4] << 16) |
             ((boot_uint32_t)current->identify_buffer[131 + format_index * 4] << 24);
    data_shift = (format >> 16) & 0xFF;

    if (data_shift < 9 || data_shift > 12) {
        log("NVME: block size 2^");
        log_dec(data_shift);
        log(" is not something this driver can address\n");
        return 0;
    }
    namespace_sector_size = 1u << data_shift;

    /* Metadata bytes per block. A namespace formatted with metadata interleaved
       into the data stream has a different layout on the wire, and reading it
       as though it were plain blocks would silently misalign everything. */
    if (current->identify_buffer[128 + format_index * 4] |
        current->identify_buffer[129 + format_index * 4]) {
        log("NVME: namespace has interleaved metadata, refusing it\n");
        return 0;
    }

    log("NVME: namespace ");
    log_dec(nsid);
    log(" is ");
    log_dec(namespace_sectors * namespace_sector_size / 1024U / 1024U);
    log(" MB, ");
    log_dec(namespace_sector_size);
    log("-byte blocks\n");

    out->id = nsid;
    out->sectors = namespace_sectors;
    out->sector_size = namespace_sector_size;
    out->used = 1;
    return 1;
}

/* Create the I/O pair. Completion queue first: the submission queue names it,
   so it has to exist before the reference can be made. */
static int create_io_queues(void) {
    NVME_COMMAND command;

    if (!queue_init(&current->io_queue, IO_QUEUE_ID, IO_QUEUE_SIZE)) {
        log("NVME: out of memory for the I/O queues\n");
        return 0;
    }

    memset(&command, 0, sizeof(command));
    command.command = ADMIN_CREATE_CQ;
    command.prp1 = (boot_uint64_t)(unsigned long long)current->io_queue.completions;
    command.dword10 = ((IO_QUEUE_SIZE - 1) << 16) | IO_QUEUE_ID;
    /* Physically contiguous, and no interrupt: completions are polled. */
    command.dword11 = 1;
    if (!submit(&current->admin_queue, &command, 0, 5000)) {
        log("NVME: could not create the I/O completion queue\n");
        return 0;
    }

    memset(&command, 0, sizeof(command));
    command.command = ADMIN_CREATE_SQ;
    command.prp1 = (boot_uint64_t)(unsigned long long)current->io_queue.commands;
    command.dword10 = ((IO_QUEUE_SIZE - 1) << 16) | IO_QUEUE_ID;
    command.dword11 = (IO_QUEUE_ID << 16) | 1;
    if (!submit(&current->admin_queue, &command, 0, 5000)) {
        log("NVME: could not create the I/O submission queue\n");
        return 0;
    }
    return 1;
}

/* Read or write through the current->bounce buffer, one page at a time.
 *
 * A transfer described by PRP1 alone may not cross a page boundary, and the
 * block layer above hands down pointers from anywhere. Bouncing costs a copy
 * per chunk and removes the question entirely - the same trade the USB storage
 * driver makes, for the same reason. Chained PRP lists would lift the limit
 * when there is a reason to want longer transfers. */
static int transfer(NVME_NAMESPACE* space, boot_uint64_t lba,
                    boot_uint32_t count, void* buffer, int write) {
    boot_uint8_t* caller = (boot_uint8_t*)buffer;
    boot_uint32_t per_chunk;

    if (!current->ready || !space || !space->used || !count || !buffer) return 0;
    if (lba + count > space->sectors) return 0;

    per_chunk = (boot_uint32_t)(PAGE_SIZE / space->sector_size);
    while (count) {
        boot_uint32_t chunk = count < per_chunk ? count : per_chunk;
        boot_uint32_t bytes = chunk * space->sector_size;
        NVME_COMMAND command;

        if (write) memcpy(current->bounce, caller, bytes);

        memset(&command, 0, sizeof(command));
        command.command = write ? IO_WRITE : IO_READ;
        command.namespace_id = space->id;
        command.prp1 = (boot_uint64_t)(unsigned long long)current->bounce;
        command.dword10 = (boot_uint32_t)lba;
        command.dword11 = (boot_uint32_t)(lba >> 32);
        /* The block count is written one less than it is, so zero means one
           block and a command can never ask for nothing. */
        command.dword12 = chunk - 1;

        if (!submit(&current->io_queue, &command, 0, 10000)) return 0;
        if (!write) memcpy(caller, current->bounce, bytes);

        caller += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 1;
}

/* The two ways in from above, and the only places the current drive is
   chosen. `driver_data` holds which namespace, and the namespace knows which
   drive it belongs to by where it sits in the table. */
static int nvme_block_read(BLOCK_DEVICE* device, boot_uint64_t lba,
                           boot_uint32_t count, void* buffer) {
    NVME_NAMESPACE* space = (NVME_NAMESPACE*)device->driver_data;

    current = drive_of(space);
    if (!current) return 0;
    return transfer(space, lba, count, buffer, 0);
}

static int nvme_block_write(BLOCK_DEVICE* device, boot_uint64_t lba,
                            boot_uint32_t count, const void* buffer) {
    NVME_NAMESPACE* space = (NVME_NAMESPACE*)device->driver_data;

    current = drive_of(space);
    if (!current) return 0;
    return transfer(space, lba, count, (void*)buffer, 1);
}

static int register_namespace(NVME_NAMESPACE* space, boot_uint32_t number) {
    BLOCK_DEVICE device;

    memset(&device, 0, sizeof(device));
    device.name[0] = 'n'; device.name[1] = 'v'; device.name[2] = 'm';
    device.name[3] = 'e';
    device.name[4] = (char)('0' + (number % 10));
    device.name[5] = 0;
    device.sector_size = space->sector_size;
    device.sector_count = space->sectors;
    device.driver_data = space;
    device.read = nvme_block_read;
    device.write = nvme_block_write;
    return block_register(&device) >= 0;
}

/* How many namespaces have been handed to the block layer in total, which is
   what names them: nvme0, nvme1, ... across every controller. */
static boot_uint32_t namespaces_registered;

int nvme_init(const PCI_DEVICE* controller) {
    boot_uint64_t base;
    boot_uint64_t capabilities;
    boot_uint32_t version;
    boot_uint32_t minimum_page_shift;

    if (drive_count >= NVME_MAX_DRIVES) {
        log("NVME: no room for another controller\n");
        return 0;
    }
    current = &drives[drive_count];
    memset(current, 0, sizeof(*current));

    current->ready = 0;
    if (!controller) return 0;

    base = pci_bar_address(controller, 0);
    if (!base) {
        log("NVME: BAR0 is not a memory window\n");
        return 0;
    }
    if (!paging_map_device(base, NVME_WINDOW_SIZE)) {
        log("NVME: could not map its register window\n");
        return 0;
    }
    pci_enable_bus_mastering(controller);
    current->registers = (volatile boot_uint8_t*)(unsigned long long)base;

    capabilities = read64(REG_CAP);
    version = read32(REG_VS);

    current->max_queue_entries = (boot_uint32_t)(capabilities & 0xFFFF) + 1;
    current->doorbell_stride = 4u << ((capabilities >> 32) & 0xF);
    /* The timeout field counts half-seconds, and is how long the controller
       may take to become ready. Some take most of it. */
    current->ready_timeout_ms = ((capabilities >> 24) & 0xFF) * 500ULL;
    if (!current->ready_timeout_ms) current->ready_timeout_ms = 500;
    minimum_page_shift = 12 + (boot_uint32_t)((capabilities >> 48) & 0xF);

    log("NVME: version ");
    log_dec((version >> 16) & 0xFFFF);
    log(".");
    log_dec((version >> 8) & 0xFF);
    log(" at ");
    log_hex(base);
    log(", ");
    log_dec(current->max_queue_entries);
    log(" max queue entries, ready timeout ");
    log_dec(current->ready_timeout_ms);
    log(" ms\n");

    if (minimum_page_shift > 12) {
        log("NVME: the controller cannot address 4 KiB pages\n");
        return 0;
    }
    if (current->max_queue_entries < IO_QUEUE_SIZE) {
        log("NVME: queues are shallower than this driver asks for\n");
        return 0;
    }

    if (!disable_controller()) return 0;

    if (!queue_init(&current->admin_queue, 0, ADMIN_QUEUE_SIZE)) {
        log("NVME: out of memory for the admin queues\n");
        return 0;
    }
    current->identify_buffer = (boot_uint8_t*)alloc_page();
    current->bounce = (boot_uint8_t*)alloc_page();
    if (!current->identify_buffer || !current->bounce) {
        log("NVME: out of memory for its buffers\n");
        return 0;
    }

    /* Both sizes are written one less than they are, in the same register. */
    write32(REG_AQA, ((ADMIN_QUEUE_SIZE - 1) << 16) | (ADMIN_QUEUE_SIZE - 1));
    write64(REG_ASQ, (boot_uint64_t)(unsigned long long)current->admin_queue.commands);
    write64(REG_ACQ, (boot_uint64_t)(unsigned long long)current->admin_queue.completions);

    if (!enable_controller()) return 0;
    if (!identify_controller()) return 0;

    if (!create_io_queues()) return 0;
    current->ready = 1;

    /* Every namespace, not just the first.
     *
     * A consumer drive puts everything in namespace 1 and there is nothing
     * else to find, which is why looking only there worked. Enterprise
     * hardware carves a drive into several, and asking about a namespace that
     * does not exist is answered rather than fatal - so this walks the low
     * numbers and keeps whatever answers. */
    for (boot_uint32_t nsid = 1;
         nsid <= NVME_MAX_NAMESPACES &&
         current->namespace_count < NVME_MAX_NAMESPACES; nsid++) {
        NVME_NAMESPACE* space = &current->namespaces[current->namespace_count];

        memset(space, 0, sizeof(*space));
        if (!identify_namespace(nsid, space)) continue;
        if (!register_namespace(space, namespaces_registered)) {
            log("NVME: the block layer would not take the namespace\n");
            space->used = 0;
            continue;
        }
        current->namespace_count++;
        namespaces_registered++;
    }

    if (!current->namespace_count) {
        log("NVME: the controller has no namespace this can use\n");
        current->ready = 0;
        return 0;
    }

    drive_count++;
    log("NVME: ready\n");
    return 1;
}

boot_uint32_t nvme_namespace_count(void) {
    return namespaces_registered;
}

boot_uint64_t nvme_sector_count(boot_uint32_t index) {
    boot_uint32_t seen = 0;

    for (boot_uint32_t drive = 0; drive < drive_count; drive++)
        for (boot_uint32_t space = 0; space < drives[drive].namespace_count; space++)
            if (seen++ == index) return drives[drive].namespaces[space].sectors;
    return 0;
}

boot_uint32_t nvme_sector_size(boot_uint32_t index) {
    boot_uint32_t seen = 0;

    for (boot_uint32_t drive = 0; drive < drive_count; drive++)
        for (boot_uint32_t space = 0; space < drives[drive].namespace_count; space++)
            if (seen++ == index) return drives[drive].namespaces[space].sector_size;
    return 0;
}
