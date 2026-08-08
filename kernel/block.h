#ifndef KERNEL_BLOCK_H
#define KERNEL_BLOCK_H

#include "../include/bootinfo.h"

#define BLOCK_MAX_DEVICES 16
#define BLOCK_NAME_LENGTH 16

/* A sector-addressable device. AHCI provides the only implementation today;
   NVMe will register through the same interface, which is the whole reason
   this layer exists - the filesystem above it should never learn what kind of
   controller it is sitting on. */
typedef struct BLOCK_DEVICE {
    char name[BLOCK_NAME_LENGTH];
    boot_uint32_t sector_size;
    boot_uint64_t sector_count;   /* 0 when the driver could not determine it */
    void* driver_data;
    int (*read)(struct BLOCK_DEVICE* device, boot_uint64_t lba,
                boot_uint32_t count, void* buffer);
    int (*write)(struct BLOCK_DEVICE* device, boot_uint64_t lba,
                 boot_uint32_t count, const void* buffer);
} BLOCK_DEVICE;

/* Register a device. Returns the assigned index, or -1 when full. */
int block_register(const BLOCK_DEVICE* device);

/* Forget one by name, for a device that has been unplugged.
 *
 * The entry is emptied rather than the list compacted: an index handed out
 * earlier has to keep meaning what it meant, and a volume still holding a
 * pointer to this device would otherwise be holding a pointer to a different
 * one. Reads and writes to a forgotten device fail rather than reaching
 * whatever is in that socket now. */
int block_forget(const char* name);

/* Called whenever the set of disks changes - one appearing, one going away.
 *
 * The block layer is the only place that knows a disk arrived, and the volume
 * table is rebuilt somewhere that cannot be called from a driver without
 * dragging the filesystem into it. So the shell leaves a note here and the
 * drivers ring it. Without this a stick plugged in after boot is a block
 * device with no partitions looked at and no drive letter - present in `mem`,
 * unreadable in `disk`, which is exactly how it looked. */
void block_on_change(void (*handler)(void));

/* Ring it. Safe to call when nobody is listening. */
void block_changed(void);

boot_uint32_t block_device_count(void);
BLOCK_DEVICE* block_device(boot_uint32_t index);

/* Both return 1 on success. A device with no write handler fails the write. */
int block_read(BLOCK_DEVICE* device, boot_uint64_t lba, boot_uint32_t count,
               void* buffer);
int block_write(BLOCK_DEVICE* device, boot_uint64_t lba, boot_uint32_t count,
                const void* buffer);

#endif
