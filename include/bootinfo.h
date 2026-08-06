#ifndef BOOTINFO_H
#define BOOTINFO_H

/* This is the bootloader-to-kernel ABI. It deliberately has no UEFI types. */
typedef unsigned char boot_uint8_t;
typedef unsigned short boot_uint16_t;
typedef unsigned int boot_uint32_t;
typedef unsigned long long boot_uint64_t;

#define BOOT_MEMORY_USABLE 7U

/* The EFI memory types that describe RAM the board actually has, whether or
   not we are allowed to allocate from it.
 *
 * The list is an allowlist on purpose. The obvious alternative - count
 * everything except the memory-mapped I/O types - is wrong, because firmware
 * uses type 0 (reserved) both for RAM held back and for address space that is
 * not memory at all. On QEMU that is a 12 GiB reserved region at 1012 GiB,
 * which is how a 2 GiB machine comes to report fourteen gigabytes. */
#define BOOT_MEMORY_LOADER_CODE 1U
#define BOOT_MEMORY_LOADER_DATA 2U
#define BOOT_MEMORY_BOOT_SERVICES_CODE 3U
#define BOOT_MEMORY_BOOT_SERVICES_DATA 4U
#define BOOT_MEMORY_RUNTIME_CODE 5U
#define BOOT_MEMORY_RUNTIME_DATA 6U
#define BOOT_MEMORY_ACPI_RECLAIM 9U
#define BOOT_MEMORY_ACPI_NVS 10U
#define BOOT_MEMORY_PERSISTENT 14U

typedef struct {
    boot_uint32_t type;
    boot_uint32_t reserved;
    boot_uint64_t physical_start;
    boot_uint64_t virtual_start;
    boot_uint64_t page_count;
    boot_uint64_t attributes;
} BOOT_MEMORY_DESCRIPTOR;

typedef struct {
    boot_uint64_t framebuffer_base;
    boot_uint64_t framebuffer_size;
    boot_uint32_t framebuffer_width;
    boot_uint32_t framebuffer_height;
    boot_uint32_t framebuffer_pixels_per_scan_line;
    boot_uint32_t framebuffer_pixel_format;

    boot_uint64_t memory_map;
    boot_uint64_t memory_map_size;
    boot_uint64_t memory_map_descriptor_size;
    boot_uint32_t memory_map_descriptor_version;

    boot_uint64_t acpi_rsdp;

    /* The full span the kernel occupies in physical memory: lowest p_paddr of
       any PT_LOAD segment through the highest p_paddr + p_memsz. This covers
       .bss, which is not present in the file at all. The allocator must
       reserve this range, not the file size - see memory_init(). */
    boot_uint64_t kernel_image_base;
    boot_uint64_t kernel_image_size;

    boot_uint64_t kernel_stack_base;
    boot_uint64_t kernel_stack_size;

    /* Identifies the volume the bootloader was loaded from.
     *
     * The kernel cannot work this out for itself: it sees block devices
     * through its own drivers, which have no idea which of them the firmware
     * used. Guessing "the first FAT volume found" is right in a one-disk
     * virtual machine and badly wrong on a real one - booted from a USB stick,
     * the first FAT volume the AHCI driver can see is the internal drive's EFI
     * System Partition, and the shell would be pointed at the real system's
     * boot files.
     *
     * The FAT volume serial number is the identifier because it is written at
     * format time, is already on the volume, and needs no agreement between
     * the two sides beyond where it sits in the BPB. `boot_volume_known` is
     * zero when the bootloader could not read it, in which case the kernel
     * must not fall back to guessing. */
    boot_uint32_t boot_volume_serial;
    boot_uint32_t boot_volume_known;
} BOOT_INFO;

#endif
