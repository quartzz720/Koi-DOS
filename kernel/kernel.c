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
#include "nvme.h"
#include "hpet.h"
#include "apic.h"
#include "build.h"
#include "xhci.h"
#include "graphics.h"
#include "timer.h"

/* Everything the kernel reports goes to both the framebuffer and COM1. If a
   later step faults before drawing anything, the serial log is the only
   record of how far the boot got. */
static void report(const char* text) {
    console_write(text);
    serial_write(text);
}

/* The millisecond tick, now arriving on its own rather than being counted by
   whoever remembers to look. */
static void timer_interrupt(INTERRUPT_FRAME* frame) {
    (void)frame;
    timer_tick();
}

static void report_hex(boot_uint64_t value) {
    console_write_hex(value);
    serial_write_hex(value);
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
    graphics_init(info);
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

    /* The interrupt hardware, described rather than used yet. Reporting what
       ACPI says before anything depends on it means a machine that turns out
       to be unusual says so in the log instead of failing later for reasons
       that look unrelated. */
    if (acpi_madt_present()) {
        report("APIC: LOCAL AT ");
        report_hex(acpi_local_apic_address());
        report(", IO AT ");
        report_hex(acpi_io_apic_address());
        report("\n");
    } else {
        report("APIC: NO MADT\n");
    }
    if (hpet_init()) {
        report("HPET: ");
        report_dec(hpet_frequency() / 1000U);
        report(" KHZ\n");
        /* Two independent clocks should agree. Measuring one against the other
           is the cheapest possible proof that both are running and that the
           frequency was read correctly - a period misread by a factor of a
           thousand looks perfectly plausible until it is compared.
           Measured in this direction on purpose: `timer_wait` polls the PIT
           throughout, and the PIT counter only means anything if it is read
           more often than it wraps. */
        {
            boot_uint32_t start = hpet_counter();
            boot_uint64_t elapsed;
            timer_wait(100);
            elapsed = ((boot_uint64_t)(hpet_counter() - start) * 1000ULL) /
                      (boot_uint64_t)hpet_frequency();
            report("HPET: 100 PIT MS MEASURED AS ");
            report_dec(elapsed);
            report(" MS\n");
        }
    } else {
        report("HPET: NOT AVAILABLE\n");
    }

    /* The Local APIC last, because calibrating its timer needs a clock that
       already works - the HPET when there is one, the PIT when there is not. */
    if (apic_init()) {
        report("APIC: TIMER ");
        report_dec(apic_timer_frequency() / 1000U);
        report(" KHZ, CALIBRATED\n");

        /* The timer takes the vector IRQ0 would have used. Nothing is wired to
           IRQ0 through the I/O APIC once the 8259 is out of the picture, so
           the number is free and the dispatcher already knows it. */
        if (apic_start_timer(TIMER_HZ, IRQ_BASE)) {
            irq_register(0, timer_interrupt);
            timer_use_interrupt();
            report("TIMER: LOCAL APIC, INTERRUPT DRIVEN\n");

            /* The measurement the polled PIT could not make. Sitting inside
               `hpet_delay_us` without touching the clock is exactly the case
               that used to lose time entirely: 100 ms read back as 0. If the
               tick is genuinely arriving on its own, it now reads as 100. */
            if (hpet_available()) {
                boot_uint64_t start = timer_ticks();
                hpet_delay_us(100000);
                report("TIMER: 100 HPET MS COUNTED AS ");
                report_dec(timer_ticks() - start);
                report(" MS WITHOUT POLLING\n");
            }

            /* Everything else has to move across in the same breath. An IRQ
               still expecting the 8259 would simply stop arriving, because the
               dispatcher now acknowledges the APIC instead. */
            if (keyboard_present_ps2() && apic_route_irq(1, IRQ_BASE + 1)) {
                report("KEYBOARD: MOVED TO THE IO APIC\n");
            } else if (keyboard_present_ps2()) {
                report("KEYBOARD: COULD NOT BE ROUTED - NO IO APIC\n");
            }
            /* And the old chip is masked for good rather than left half-live:
               a spurious 8259 interrupt now arrives on a vector whose handler
               would acknowledge the wrong controller. */
            for (boot_uint8_t irq = 0; irq < 16; irq++) pic_mask_irq(irq);
        }
    } else {
        report("APIC: NOT USABLE, STAYING ON THE PIT\n");
    }
    {
        const PCI_DEVICE* controller;
        pci_scan();
        report("PCI: ");
        report_dec(pci_device_count());
        report(" DEVICES\n");
        if (pci_devices_seen() > pci_device_count()) {
            /* Loud, because the symptom otherwise is a driver reporting that
               its device is absent, with nothing to suggest the scan simply
               ran out of room. */
            report("PCI: ");
            report_dec(pci_devices_seen() - pci_device_count());
            report(" DEVICES DROPPED - TABLE FULL\n");
        }

        controller = pci_find(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA,
                              PCI_PROGIF_AHCI, 0);
        if (controller && ahci_init(controller)) {
            report("AHCI: DISK ");
            report_dec(ahci_sector_count() / 2048U);
            report(" MB\n");
        } else {
            report("AHCI: NOT FOUND\n");
        }

        controller = pci_find(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVM,
                              PCI_PROGIF_NVME, 0);
        if (controller && nvme_init(controller)) {
            report("NVME: DISK ");
            report_dec(nvme_sector_count() * nvme_sector_size() / 1048576U);
            report(" MB\n");
        } else {
            report("NVME: NOT FOUND\n");
        }

        /* Every xHCI controller on the bus, not the first one. A board
           routinely has two - one in the chipset, one in the processor - and
           which one a device lands on is decided by the socket it was plugged
           into, which the OS has no say in. */
        {
            boot_uint32_t found = 0;
            for (boot_uint32_t index = 0; index < pci_device_count(); index++) {
                const PCI_DEVICE* entry = pci_device(index);
                if (!entry || entry->class_code != PCI_CLASS_SERIAL_BUS ||
                    entry->subclass != PCI_SUBCLASS_USB ||
                    entry->programming_interface != PCI_PROGIF_XHCI) continue;
                found++;
                (void)xhci_init(entry);
            }
            report("XHCI: ");
            report_dec(xhci_controller_count());
            report(" OF ");
            report_dec(found);
            report(" CONTROLLERS\n");
        }

        if (xhci_controller_count()) {
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

        /* Now that the volumes can be read, see whether one of them says the
           system lives there. Checked after mounting because it is a file, and
           only on the device we booted from - a marker on some other disk
           describes some other installation, not this one. */
        {
            VOLUME* loader = volume_boot();
            for (boot_uint32_t index = 0; loader && index < volumes; index++) {
                VOLUME* candidate = volume_at(index);
                FAT_ENTRY entry;

                if (!candidate || candidate == loader) continue;
                if (candidate->device != loader->device) continue;
                if (!fat32_stat(candidate, SYSTEM_VOLUME_MARKER, &entry)) continue;

                partition_set_system_volume(candidate);
                report("SYSTEM VOLUME: ");
                report(candidate->label[0] ? candidate->label : "unlabelled");
                report(" - the loader's partition has no drive letter\n");
                break;
            }
        }
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
