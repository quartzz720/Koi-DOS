#include "../include/bootinfo.h"
#include "console.h"
#include "serial.h"
#include "memory.h"
#include "cpu.h"
#include "idt.h"
#include "pic.h"
#include "acpi.h"
#include "keyboard.h"
#include "heap.h"
#include "paging.h"
#include "block.h"
#include "partition.h"
#include "fat32.h"
#include "command.h"
#include "config.h"
#include "pci.h"
#include "ahci.h"
#include "build.h"
#include "xhci.h"
#include "timer.h"

/* Everything the kernel reports goes to both the framebuffer and COM1. If a
   later step faults before drawing anything, the serial log is the only
   record of how far the boot got. */
static void report(const char* text) {
    console_write(text);
    serial_write(text);
}

static void report_dec(boot_uint64_t value) {
    console_write_dec(value);
    serial_write_dec(value);
}

__attribute__((noreturn)) void kernel_main(BOOT_INFO* info) {
    cpu_disable_interrupts();
    serial_init();
    /* The build stamp goes out before anything else can fail, so a log that
       ends in a panic still says which kernel produced it. */
    serial_write("\n=== KOI DOS build " KOI_BUILD_DATE
                 " (" KOI_BUILD_COMMIT ") ===\n");
    /* memory_init first: console_init allocates its back buffer. */
    memory_init(info);
    console_init(info);
    console_use_theme();
    console_clear();
    console_set_color(COLOR_WHITE, console_theme()->background);
    report("KOI DOS KERNEL\n");
    console_use_theme();
    report(memory_self_test() ? "MEMORY ALLOC FREE: OK\n" : "MEMORY ALLOC FREE: FAIL\n");
    report("MEMORY FREE: ");
    report_dec(memory_free_pages() * PAGE_SIZE / 1024U / 1024U);
    report(" MB\n");

    /* GDT before IDT: every gate stores a code selector, and it has to be one
       of ours rather than one the firmware happened to leave behind. */
    gdt_init();
    tss_init();
    idt_init();
    pic_init();
    cpu_enable_interrupts();
    report("CPU: GDT IDT PIC READY\n");

    /* Own the page tables before any driver maps device memory. */
    if (paging_init(info)) {
        report("PAGING: IDENTITY MAP ");
        report_dec(paging_mapped_bytes() / 1024U / 1024U);
        report(" MB\n");
    } else {
        report("PAGING: FAILED, USING FIRMWARE TABLES\n");
    }

    heap_init();
    report("HEAP: ");
    report_dec(heap_total() / 1024U);
    report(heap_self_test() ? " KB, SELF TEST OK\n" : " KB, SELF TEST FAILED\n");

    acpi_init(info->acpi_rsdp);
    switch (keyboard_init()) {
    case KEYBOARD_READY:
        report("KEYBOARD: PS/2 READY\n");
        break;
    case KEYBOARD_NO_DEVICE:
        /* The controller is real but nothing answered it. Common on desktop
           boards that carry an 8042 in the chipset and no socket for it. */
        report("KEYBOARD: PS/2 CONTROLLER, NO KEYBOARD ATTACHED\n");
        break;
    default:
        report("KEYBOARD: NO PS/2 CONTROLLER\n");
        break;
    }

    timer_init();
    report("TIMER: PIT POLLING 1000 HZ\n");
    {
        const PCI_DEVICE* controller;
        pci_scan();
        report("PCI: ");
        report_dec(pci_device_count());
        report(" DEVICES\n");

        controller = pci_find(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA,
                              PCI_PROGIF_AHCI, 0);
        if (controller && ahci_init(controller)) {
            report("AHCI: DISK ");
            report_dec(ahci_sector_count() / 2048U);
            report(" MB\n");
        } else {
            report("AHCI: NOT FOUND\n");
        }

        controller = pci_find(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                              PCI_PROGIF_XHCI, 0);
        if (controller && xhci_init(controller)) {
            report("XHCI: ");
            report_dec(xhci_ports_connected());
            report(" OF ");
            report_dec(xhci_port_count());
            report(" PORTS IN USE");
            if (xhci_has_keyboard()) report(", KEYBOARD");
            if (xhci_has_storage()) report(", STORAGE");
            report("\n");
        } else {
            report("XHCI: NOT FOUND\n");
        }
    }

    {
        boot_uint32_t volumes = partition_scan(info->boot_volume_serial,
                                              info->boot_volume_known);
        boot_uint32_t mounted = 0;
        for (boot_uint32_t index = 0; index < volumes; index++) {
            VOLUME* volume = volume_at(index);
            if (volume && fat32_mount(volume)) mounted++;
        }
        report("VOLUMES: ");
        report_dec(mounted);
        report(" FAT32 OF ");
        report_dec(volumes);
        report("\n");
        /* Applied before the shell paints anything, so the first prompt is
           already in the user's colours rather than flashing the default. */
        config_load(volume_boot());
        if (!volume_boot()) {
            /* Loud, because every command afterwards has no drive to work on
               and the reason is not obvious from the prompt. */
            report("BOOT VOLUME: NOT FOUND - no drive assigned\n");
        }
    }

    command_run();
}
