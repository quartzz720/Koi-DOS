#include "block.h"
#include "string.h"

static BLOCK_DEVICE devices[BLOCK_MAX_DEVICES];
static boot_uint32_t device_count;

int block_register(const BLOCK_DEVICE* device) {
    if (device_count >= BLOCK_MAX_DEVICES || !device || !device->read) return -1;
    devices[device_count] = *device;
    return (int)device_count++;
}

boot_uint32_t block_device_count(void) {
    return device_count;
}

BLOCK_DEVICE* block_device(boot_uint32_t index) {
    return index < device_count ? &devices[index] : (BLOCK_DEVICE*)0;
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
