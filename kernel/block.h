#ifndef KERNEL_BLOCK_H
#define KERNEL_BLOCK_H

#include "../include/bootinfo.h"

#define BLOCK_MAX_DEVICES 8
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

boot_uint32_t block_device_count(void);
BLOCK_DEVICE* block_device(boot_uint32_t index);

/* Both return 1 on success. A device with no write handler fails the write. */
int block_read(BLOCK_DEVICE* device, boot_uint64_t lba, boot_uint32_t count,
               void* buffer);
int block_write(BLOCK_DEVICE* device, boot_uint64_t lba, boot_uint32_t count,
                const void* buffer);

#endif
