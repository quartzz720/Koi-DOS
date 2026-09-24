#include "block.h"
#include "string.h"

static BLOCK_DEVICE devices[BLOCK_MAX_DEVICES];
static boot_uint32_t device_count;

/* How many times the set of disks has changed.
 *
 * A number rather than a flag, because more than one thing wants to know and
 * each of them wants to know whether it has changed since *it* last looked.
 * The shell asks at its prompt; a file browser asks while it is running. A
 * flag would be read by the first of them and gone for the second. */
static boot_uint32_t generation;

boot_uint32_t block_generation(void) { return generation; }

int block_register(const BLOCK_DEVICE* device) {
    if (device_count >= BLOCK_MAX_DEVICES || !device || !device->read) return -1;
    devices[device_count] = *device;
    generation++;
    return (int)device_count++;
}

/* An emptied slot is not a device. Returning it would hand out an entry whose
   read and write are null and whose size is zero, and every caller would have
   to know that. */
int block_forget(const char* name) {
    if (!name) return 0;
    generation++;
    for (boot_uint32_t index = 0; index < device_count; index++) {
        if (strcmp(devices[index].name, name)) continue;
        /* Emptied in place, not removed from the list. An index handed out
           earlier has to go on meaning what it meant, and anything still
           holding a pointer to this entry now finds a device with no read
           handler - which fails - rather than one that belongs to whatever is
           in that socket now. */
        memset(&devices[index], 0, sizeof(devices[index]));
        return 1;
    }
    return 0;
}

static void (*change_handler)(void);

void block_on_change(void (*handler)(void)) {
    change_handler = handler;
}

void block_changed(void) {
    if (change_handler) change_handler();
}

boot_uint32_t block_device_count(void) {
    return device_count;
}

BLOCK_DEVICE* block_device(boot_uint32_t index) {
    if (index >= device_count) return (BLOCK_DEVICE*)0;
    /* An entry that has been forgotten is not a device. Handing it back would
       give every caller an object with no read, no write and no size, and
       leave each of them to work out what that meant. */
    if (!devices[index].read) return (BLOCK_DEVICE*)0;
    return &devices[index];
}

int block_read(BLOCK_DEVICE* device, boot_uint64_t lba, boot_uint32_t count,
               void* buffer) {
    if (!device || !device->read || !buffer || !count) return 0;
    /* A driver that knows its size gets the range checked for free. Reading
       past the end of a disk returns garbage rather than an error on some
       controllers, which is exactly the kind of thing that turns into a
       confusing filesystem bug three layers up. */
    if (device->sector_count && lba + count > device->sector_count) return 0;
    return device->read(device, lba, count, buffer);
}

int block_write(BLOCK_DEVICE* device, boot_uint64_t lba, boot_uint32_t count,
                const void* buffer) {
    if (!device || !device->write || !buffer || !count) return 0;
    if (device->sector_count && lba + count > device->sector_count) return 0;
    return device->write(device, lba, count, buffer);
}
