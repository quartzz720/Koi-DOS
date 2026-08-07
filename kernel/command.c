#include "command.h"
#include "console.h"
#include "keyboard.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "heap.h"
#include "paging.h"
#include "pci.h"
#include "block.h"
#include "xhci.h"
#include "graphics.h"
#include "fat32.h"
#include "partition.h"
#include "rtc.h"
#include "program.h"
#include "syscall.h"
#include "build.h"
#include "cpu.h"
#include "acpi.h"
#include "../include/syscall.h"

/* The command semantics follow legacy/shell.c and legacy/fs.c, which worked
   these out against UEFI Boot Services. Only the layer underneath changed. */

#define INPUT_MAX 256
#define PATH_MAX 256

static VOLUME* current_volume;
static char current_path[PATH_MAX] = "\\";

static void put(char character) {
    console_putchar(character);
    serial_putchar(character);
}

static void print(const char* text) {
    console_write(text);
    serial_write(text);
}

static void print_line(const char* text) {
    print(text);
    print("\n");
}

static void print_dec(boot_uint64_t value) {
    console_write_dec(value);
    serial_write_dec(value);
}

/* Right-align a number in a fixed column, the way a directory listing wants
   its sizes to line up. */
static void print_dec_padded(boot_uint64_t value, int width) {
    char buffer[24];
    int length = 0;

    do {
        buffer[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value);
    for (int pad = length; pad < width; pad++) print(" ");
    while (length--) put(buffer[length]);
}

static void print_two_digits(boot_uint32_t value) {
    put((char)('0' + (value / 10U) % 10U));
    put((char)('0' + value % 10U));
}

static int is_space(char character) {
    return character == ' ' || character == '\t';
}

static const char* skip_spaces(const char* text) {
    while (is_space(*text)) text++;
    return text;
}

/* Compare a command word case-insensitively, stopping at the first space. */
static int word_is(const char* input, const char* name) {
    while (*name) {
        char a = *input >= 'a' && *input <= 'z' ? (char)(*input - 32) : *input;
        if (a != *name) return 0;
        input++;
        name++;
    }
    return !*input || is_space(*input);
}

/* Command line broken into operands and switches.
 *
 * Until now every command simply took the rest of the line as one operand,
 * which worked only because no switch existed to be confused with a name and
 * no command took two of them. Both change with `copy a b.txt c d.txt` and
 * `dir /w`, so the line is tokenised properly before either arrives:
 *
 *   - a token beginning with / is a switch, DOS style: dir /w, del /y
 *   - a token in double quotes is one operand however many spaces it holds
 *   - anything else runs to the next space
 *
 * An unquoted name may still contain spaces when it is the only operand a
 * command expects, so "type a long name.txt" keeps working without quotes. */
#define ARGUMENT_MAX 4
#define SWITCH_MAX 8

typedef struct {
    char operand[ARGUMENT_MAX][PATH_MAX];
    int operand_count;
    char switches[SWITCH_MAX];
    int switch_count;
    const char* tail;   /* everything after the command word, verbatim */
} ARGUMENTS;

static char upper(char character) {
    return character >= 'a' && character <= 'z' ? (char)(character - 32) : character;
}

static void parse_arguments(const char* input, ARGUMENTS* arguments) {
    memset(arguments, 0, sizeof(*arguments));

    while (*input && !is_space(*input)) input++;
    input = skip_spaces(input);
    arguments->tail = input;

    while (*input) {
        if (*input == '/') {
            input++;
            if (*input && arguments->switch_count < SWITCH_MAX)
                arguments->switches[arguments->switch_count++] = upper(*input);
            while (*input && !is_space(*input)) input++;
        } else if (*input == '"') {
            boot_uint64_t length = 0;
            char* target = arguments->operand_count < ARGUMENT_MAX
                         ? arguments->operand[arguments->operand_count] : (char*)0;
            input++;
            while (*input && *input != '"') {
                if (target && length + 1 < PATH_MAX) target[length++] = *input;
                input++;
            }
            if (*input == '"') input++;
            if (target) { target[length] = 0; arguments->operand_count++; }
        } else {
            boot_uint64_t length = 0;
            char* target = arguments->operand_count < ARGUMENT_MAX
                         ? arguments->operand[arguments->operand_count] : (char*)0;
            while (*input && !is_space(*input) && *input != '/') {
                if (target && length + 1 < PATH_MAX) target[length++] = *input;
                input++;
            }
            if (target) { target[length] = 0; arguments->operand_count++; }
        }
        input = skip_spaces(input);
    }
}

static int has_switch(const ARGUMENTS* arguments, char letter) {
    for (int index = 0; index < arguments->switch_count; index++)
        if (arguments->switches[index] == letter) return 1;
    return 0;
}

/* The single-operand commands accept an unquoted name with spaces, because
   that is friendlier than demanding quotes for the common case. Anything
   after a switch is not part of it. */
static void single_operand(const ARGUMENTS* arguments, char* result) {
    boot_uint64_t length = 0;
    const char* cursor = arguments->tail;

    if (arguments->operand_count == 1) {
        boot_uint64_t given = strlen(arguments->operand[0]);
        memcpy(result, arguments->operand[0], given + 1);
        return;
    }
    while (cursor[length] && cursor[length] != '/' && length + 1 < PATH_MAX)
        length++;
    while (length && is_space(cursor[length - 1])) length--;
    memcpy(result, cursor, length);
    result[length] = 0;
}

static int has_wildcard(const char* text) {
    for (; *text; text++)
        if (*text == '*' || *text == '?') return 1;
    return 0;
}

/* Split an absolute path into its directory and its final component. */
static void split_leaf(const char* path, char* directory, char* leaf) {
    const char* last = path;
    boot_uint64_t length;

    for (const char* cursor = path; *cursor; cursor++)
        if (*cursor == '\\') last = cursor + 1;

    length = strlen(last);
    if (length >= PATH_MAX) length = PATH_MAX - 1;
    memcpy(leaf, last, length);
    leaf[length] = 0;

    length = (boot_uint64_t)(last - path);
    while (length > 1 && path[length - 1] == '\\') length--;
    if (!length) length = 1;
    memcpy(directory, path, length);
    directory[length] = 0;
}

/* Resolve a name written by the user into a volume and an absolute path.
 *
 * All three DOS forms are accepted, and they are genuinely different things:
 *   EFI\BOOT        relative to the current directory
 *   \EFI\BOOT       from the root of the current drive
 *   Z:\EFI\BOOT     from the root of a named drive
 *   Z:EFI\BOOT      relative to that drive's current directory - which we do
 *                   not track per drive, so it is treated as its root
 *
 * Returns 0 when a drive letter names a volume that does not exist. */
static int resolve_path(const char* name, VOLUME** volume, char* result) {
    boot_uint64_t length;

    *volume = current_volume;

    /* A drive letter binds tighter than anything else on the line. */
    if (name[0] && name[1] == ':') {
        VOLUME* named = volume_by_letter(name[0]);
        if (!named) return 0;
        *volume = named;
        name += 2;
        if (name[0] != '\\') {
            /* Z:EFI - relative to that drive. With no per-drive current
               directory kept, the only honest reading is from its root. */
            result[0] = '\\';
            length = 1;
            for (boot_uint64_t index = 0; name[index] && length + 1 < PATH_MAX; index++)
                result[length++] = name[index];
            result[length] = 0;
            return 1;
        }
    }

    if (name[0] == '\\') {
        boot_uint64_t given = strlen(name);
        if (given >= PATH_MAX) given = PATH_MAX - 1;
        memcpy(result, name, given);
        result[given] = 0;
        return 1;
    }

    /* Relative names only make sense against the current drive. */
    if (*volume != current_volume) {
        result[0] = '\\';
        result[1] = 0;
    } else {
        length = strlen(current_path);
        memcpy(result, current_path, length);
        if (length && result[length - 1] != '\\') result[length++] = '\\';
        for (boot_uint64_t index = 0; name[index] && length + 1 < PATH_MAX; index++)
            result[length++] = name[index];
        result[length] = 0;
        return 1;
    }
    return 1;
}

/* Strip the last component, for "cd ..". */
static void path_up(void) {
    boot_uint64_t length = strlen(current_path);
    while (length > 1 && current_path[length - 1] != '\\') length--;
    if (length > 1) length--;          /* drop the separator too */
    if (!length) length = 1;
    current_path[length] = 0;
}

/* Where a program is looked for, in order.
 *
 * The current directory first, so a program sitting next to your files wins;
 * then the root; then \BIN, which is where the system's own utilities live.
 * Giving them a directory of their own is not cosmetic - the root of the
 * system volume is where the user's files go, and a dozen .EXE files sitting
 * in it turns `dir` into a search. */
#define PROGRAM_SEARCH_CURRENT 0
#define PROGRAM_SEARCH_ROOT 1
#define PROGRAM_SEARCH_BIN 2
#define PROGRAM_SEARCH_PLACES 3

static void build_program_path(const char* name, char* result, int place) {
    boot_uint64_t length;

    if (place == PROGRAM_SEARCH_BIN) {
        const char* bin = "\\BIN\\";
        length = 0;
        while (bin[length]) { result[length] = bin[length]; length++; }
    } else if (place == PROGRAM_SEARCH_ROOT) {
        result[0] = '\\';
        length = 1;
    } else {
        length = strlen(current_path);
        memcpy(result, current_path, length);
        if (length && result[length - 1] != '\\') result[length++] = '\\';
    }
    for (boot_uint64_t index = 0; name[index] && length + 1 < PATH_MAX; index++)
        result[length++] = name[index];
    result[length] = 0;
}

static void print_prompt(void) {
    console_set_color(console_theme()->prompt, console_theme()->background);
    if (current_volume) put(current_volume->letter);
    else put('?');
    put(':');
    print(current_path);
    print("> ");
    console_use_theme();
}

/* The build stamp is worth more than the version during development: two
   kernels claiming 0.5 differ by whatever happened between them, and the
   number is the only way to tell from the screen which one is running. It is
   the commit count, so it only moves when history does; a trailing `+` on the
   hash means the tree had uncommitted changes when this was built. */
static void command_ver(void) {
    print("Koi-DOS ");
    print_dec(KOI_DOS_VERSION >> 8);
    put('.');
    print_dec(KOI_DOS_VERSION & 0xFF);
    print_line(" Alpha");

    print("Kernel ");
    print_dec(KOI_DOS_VERSION >> 8);
    put('.');
    print_dec(KOI_DOS_VERSION & 0xFF);
    put('.');
    print_dec(KOI_BUILD_NUMBER);
    print(", built " KOI_BUILD_DATE " (" KOI_BUILD_COMMIT ")");
    print("\n");

    print_line("A DOS-like operating system for UEFI machines.");
}

static void command_help(void) {
    print_line("cls            clear the screen");
    print_line("dir [path] [/w]  list a directory, /w for names only");
    print_line("cd [path]      change directory, cd .. to go up");
    print_line("type file      print a file");
    print_line("copy a b       copy a file");
    print_line("del file       delete a file");
    print_line("ren old new    rename a file");
    print_line("md name        create a directory");
    print_line("rd name        remove an empty directory");
    print_line("more file      view a file a screen at a time");
    print_line("tree [path]    show the directory tree");
    print_line("attrib [+-RHSA] file   show or change attributes");
    print_line("vol            show the volume label");
    print_line("mem            memory, devices and volumes");
    print_line("pci            every function on the PCI bus");
    print_line("disk           disks and partitions, letters or not");
    print_line("format <part>  make a new filesystem - destroys everything on it");
    print_line("part <disk>    replace the partition table - destroys the whole disk");
    print_line("setup          install Koi-DOS onto a disk");
    print_line("shutdown       turn the machine off");
    print_line("reboot         restart the machine");
    print_line("date           show the date");
    print_line("time           show the time");
    print_line("echo [text]    print text");
    print_line("ver            show the version");
    print_line("help           this list");
    print_line("");
    print_line("Anything else is looked up as a program: NAME or NAME.EXE,");
    print_line("in the current directory and then at the root of the drive.");
}

static void command_date(void) {
    RTC_TIME now;
    rtc_read(&now);
    print("Current date is ");
    print_dec(now.year);
    put('-');
    print_two_digits(now.month);
    put('-');
    print_two_digits(now.day);
    print("\n");
}

static void command_time(void) {
    RTC_TIME now;
    rtc_read(&now);
    print("Current time is ");
    print_two_digits(now.hour);
    put(':');
    print_two_digits(now.minute);
    put(':');
    print_two_digits(now.second);
    print("\n");
}

static void command_vol(void) {
    if (!current_volume) { print_line("No volume."); return; }
    print(" Volume in drive ");
    put(current_volume->letter);
    if (current_volume->label[0]) {
        print(" is ");
        print(current_volume->label);
    } else {
        print(" has no label");
    }
    print("\n");
}

/* `mem` is the one place the system describes itself, so it reports what was
   found as well as what is free. A line that reads `USB : keyboard` is the
   fastest way to answer "did the driver actually come up", which otherwise
   means rebooting with a serial cable attached. */
static void print_field(const char* label, boot_uint64_t value,
                        const char* unit) {
    print(label);
    print_dec(value);
    print_line(unit);
}

static void command_mem(void) {
    print_field("Physical memory : ", memory_physical_pages() * PAGE_SIZE / 1024U / 1024U, " MB");
    print_field("Available       : ", memory_free_pages() * PAGE_SIZE / 1024U / 1024U, " MB");
    print_line("");

    print_field("Kernel image    : ", memory_kernel_bytes() / 1024U, " KB");
    print_field("Page tables     : ", paging_table_bytes() / 1024U, " KB");
    print_field("Identity map    : ", paging_mapped_bytes() / 1024U / 1024U,
                " MB (RAM and device windows)");
    print_field("Heap            : ", heap_total() / 1024U, " KB");
    print_field("Heap free       : ", (heap_total() - heap_used()) / 1024U, " KB");
    print_line("");

    print_field("PCI devices     : ", pci_device_count(), "");

    print("Disks           : ");
    print_dec(block_device_count());
    for (boot_uint32_t index = 0; index < block_device_count(); index++) {
        BLOCK_DEVICE* device = block_device(index);
        print(index ? ", " : "  ");
        print(device ? device->name : "?");
    }
    print_line("");

    print("Volumes         : ");
    print_dec(volume_count());
    for (boot_uint32_t index = 0; index < volume_count(); index++) {
        VOLUME* volume = volume_at(index);
        print(index ? ", " : "  ");
        put(volume ? volume->letter : '?');
        put(':');
    }
    print_line("");

    print("USB             : ");
    if (!xhci_port_count()) {
        print_line("no controller");
    } else {
        if (xhci_has_keyboard()) print("keyboard");
        if (xhci_has_keyboard() && xhci_has_storage()) print(", ");
        if (xhci_has_storage()) print("storage");
        if (!xhci_has_keyboard() && !xhci_has_storage()) print("nothing claimed");
        print(" (");
        print_dec(xhci_ports_connected());
        print(" of ");
        print_dec(xhci_port_count());
        print_line(" ports in use)");
    }
}

static void print_hex(boot_uint64_t value, int digits) {
    const char* digit = "0123456789ABCDEF";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        put(digit[(value >> shift) & 0xF]);
}

/* Enough of the class list to name what a DOS-like system might ever drive,
   and honest about the rest. USB is broken out by programming interface
   because that is the field that separates xHCI from the older controllers. */
static const char* class_name(const PCI_DEVICE* device) {
    switch (device->class_code) {
    case 0x01:
        switch (device->subclass) {
        case 0x01: return "IDE";
        case PCI_SUBCLASS_SATA: return "SATA";
        case PCI_SUBCLASS_NVM: return "NVMe";
        default: return "storage";
        }
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x06: return "bridge";
    case 0x08: return "system";
    case 0x09: return "input";
    case 0x0C:
        if (device->subclass != PCI_SUBCLASS_USB) return "serial bus";
        switch (device->programming_interface) {
        case 0x00: return "USB UHCI";
        case 0x10: return "USB OHCI";
        case 0x20: return "USB EHCI";
        case PCI_PROGIF_XHCI: return "USB xHCI";
        default: return "USB";
        }
    default: return "other";
    }
}

/* Sizes a person can compare at a glance, which means the largest unit that
   still leaves a whole number rather than everything in sectors. */
static void print_size(boot_uint64_t sectors, boot_uint32_t sector_size) {
    boot_uint64_t kib = sectors / (1024U / sector_size ? 1024U / sector_size : 1);

    if (!sector_size) { print("?"); return; }
    if (kib >= 1024U * 1024U) {
        print_dec(kib / (1024U * 1024U));
        print(" GB");
    } else if (kib >= 1024U) {
        print_dec(kib / 1024U);
        print(" MB");
    } else {
        print_dec(kib);
        print(" KB");
    }
}

/* What is on the disks, as opposed to what has a drive letter.
 *
 * The two are deliberately different views. `mem` and `dir` show the shell's
 * world: volumes it can read. This shows the disk's world, including regions
 * with no filesystem we understand - because anything that formats or
 * repartitions has to address those, and because a partition nobody mentions
 * is exactly the one somebody erases by accident. */
static void command_disk(void) {
    print_line("DEVICE  SIZE        PARTITIONS");

    for (boot_uint32_t index = 0; index < block_device_count(); index++) {
        BLOCK_DEVICE* device = block_device(index);
        int listed = 0;

        if (!device) continue;
        print(device->name);
        for (boot_uint64_t pad = strlen(device->name); pad < 8; pad++) put(' ');
        print_size(device->sector_count, device->sector_size);
        print_line("");

        for (boot_uint32_t p = 0; p < partition_count(); p++) {
            PARTITION* partition = partition_at(p);
            if (!partition || partition->device != device) continue;
            listed++;

            print("  ");
            print(device->name);
            put('p');
            print_dec(partition->number);
            print("  ");
            print_size(partition->sector_count, device->sector_size);
            print("  at sector ");
            print_dec(partition->first_sector);
            print("  ");

            if (partition->letter) {
                put(partition->letter);
                print(":");
            } else {
                print("--");
            }
            if (partition->is_efi_system) print("  EFI System");
            else if (partition->is_fat) print("  FAT");
            else if (partition->scheme == PARTITION_SCHEME_MBR) {
                print("  type ");
                print_dec(partition->type);
            } else {
                print("  unknown");
            }
            print_line("");
        }
        if (!listed) print_line("  no partition table, and nothing we can read");
    }
    print_line("");
    print_line("A partition with no drive letter has no filesystem this system");
    print_line("understands. That does not mean it is empty.");
}

/* Find a partition by the name `disk` prints for it, e.g. "nvme0p2". */
static PARTITION* partition_by_name(const char* name) {
    for (boot_uint32_t index = 0; index < partition_count(); index++) {
        PARTITION* partition = partition_at(index);
        const char* cursor = name;
        const char* device_name;
        boot_uint32_t number = 0;

        if (!partition || !partition->device) continue;
        device_name = partition->device->name;

        while (*device_name && upper(*cursor) == upper(*device_name)) {
            cursor++;
            device_name++;
        }
        if (*device_name) continue;
        if (upper(*cursor) != 'P') continue;
        cursor++;
        if (!*cursor) continue;
        while (*cursor >= '0' && *cursor <= '9')
            number = number * 10U + (boot_uint32_t)(*cursor++ - '0');
        if (*cursor) continue;
        if (number == partition->number) return partition;
    }
    return (PARTITION*)0;
}

/* Rebuild the volume table after the disks changed underneath it, and put the
   shell somewhere that certainly still exists. Every VOLUME pointer taken
   before this - including the one the user was standing on - is stale. */
static void remount_everything(void) {
    boot_uint32_t volumes;

    /* Before the rescan, not after: the volume table is a static array, so the
       rescan refills the same addresses and any mount record still pointing at
       one of them would carry the old filesystem's geometry into the new
       filesystem's device. */
    fat32_unmount_all();
    volumes = partition_rescan();

    for (boot_uint32_t index = 0; index < volumes; index++) {
        VOLUME* volume = volume_at(index);
        if (volume) (void)fat32_mount(volume);
    }
    current_volume = volume_boot();
    current_path[0] = '\\';
    current_path[1] = 0;
}

/* ---- The installer ------------------------------------------------------- */

/* Setup paints its own headings rather than using the console theme, so that
   it looks like a thing you are running rather than a command that scrolled
   past. The text-mode installers this is modelled on did the same. */
#define KOI_SETUP_TITLE_FOREGROUND COLOR_WHITE
#define KEY_F8 (KEY_F1 + 7)
#define KEY_F3 (KEY_F1 + 2)

static int copy_one(VOLUME* from_volume, const char* from,
                    VOLUME* to_volume, const char* to, boot_uint32_t* copied);

/* When a file operation fails and the device has something to say about why,
   say it. The layer that knows is the driver, and by the time the failure
   reaches here it has been reduced to a zero - so this asks. */
static void print_device_reason(void) {
    const char* reason = xhci_storage_error();

    if (!reason) return;
    print("The USB device reported: ");
    print_line(reason);
}

/* Which volume on the boot device holds a given file. The installation media
   may be a single volume or a two-partition layout where the loader's
   partition has no drive letter at all, so it cannot be named - only found. */
static VOLUME* volume_holding(const char* path) {
    VOLUME* boot = volume_boot();
    FAT_ENTRY entry;

    if (boot && fat32_stat(boot, path, &entry)) return boot;
    for (boot_uint32_t index = 0; index < volume_count(); index++) {
        VOLUME* volume = volume_at(index);
        if (!volume || !boot || volume->device != boot->device) continue;
        if (fat32_stat(volume, path, &entry)) return volume;
    }
    return (VOLUME*)0;
}

static void setup_banner(const char* title) {
    console_clear();
    console_set_color(KOI_SETUP_TITLE_FOREGROUND, console_theme()->background);
    print_line("");
    print("  Koi-DOS Setup - ");
    print_line(title);
    console_use_theme();
    print_line("");
}

/* Show a file a screenful at a time, the way the licence has to be read
   before it can be agreed to. Returns 1 when the reader accepted. */
static int setup_show_licence(VOLUME* volume) {
    FAT_ENTRY entry;
    char* buffer;
    boot_uint32_t offset = 0;
    boot_uint32_t lines = 0;
    boot_uint32_t rows = console_rows() > 6 ? console_rows() - 6 : 10;
    int accepted = 0;

    setup_banner("Licence");
    if (!fat32_stat(volume, "\\LICENSE", &entry)) {
        print_line("The licence file is missing from this media.");
        print_line("Setup will not continue without it.");
        print_line("");
        print("Press a key. ");
        (void)keyboard_getchar();
        return 0;
    }

    buffer = (char*)kmalloc(1024);
    if (!buffer) return 0;

    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset, buffer, 1023);
        if (!got) break;
        buffer[got] = 0;
        for (boot_uint32_t index = 0; index < got; index++) {
            if (buffer[index] == '\r') continue;
            put(buffer[index]);
            if (buffer[index] != '\n') continue;
            if (++lines < rows) continue;
            lines = 0;
            console_set_color(console_theme()->prompt, console_theme()->background);
            print("  -- more -- ");
            console_use_theme();
            (void)keyboard_getchar();
            print_line("");
        }
        offset += got;
    }
    kfree(buffer);

    print_line("");
    console_set_color(KOI_SETUP_TITLE_FOREGROUND, console_theme()->background);
    print_line("  F8 to accept and continue, any other key to stop.");
    console_use_theme();
    accepted = keyboard_getchar() == KEY_F8;
    return accepted;
}

/* Copy one file, saying so, and stop the whole install if it fails. A missing
   file here means the installed system would not start. */
static int setup_copy(VOLUME* from, const char* source,
                      VOLUME* to, const char* target) {
    boot_uint32_t copied = 0;

    print("    ");
    print(target);
    if (!copy_one(from, source, to, target, &copied)) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("  FAILED");
        console_use_theme();
        print_device_reason();
        return 0;
    }
    print("  ");
    print_dec(copied);
    print_line(" bytes");
    return 1;
}

/* Find a partition on `device` by its number. */
static PARTITION* setup_partition(BLOCK_DEVICE* device, boot_uint32_t number) {
    for (boot_uint32_t index = 0; index < partition_count(); index++) {
        PARTITION* partition = partition_at(index);
        if (partition && partition->device == device &&
            partition->number == number) return partition;
    }
    return (PARTITION*)0;
}

/* Find a block device by the name `disk` prints for it. */
static BLOCK_DEVICE* device_by_name(const char* name) {
    for (boot_uint32_t index = 0; index < block_device_count(); index++) {
        BLOCK_DEVICE* device = block_device(index);
        const char* a;
        const char* b;

        if (!device) continue;
        a = name;
        b = device->name;
        while (*a && *b && upper(*a) == upper(*b)) { a++; b++; }
        if (!*a && !*b) return device;
    }
    return (BLOCK_DEVICE*)0;
}

/* Lay a fresh partition table over a whole disk.
 *
 * The layout is fixed rather than asked about, and that is the DOS answer to
 * the question: a boot partition the firmware can find, and a system partition
 * for everything else. Keeping the loader on a partition of its own is not
 * security - any other operating system sees an ordinary partition - but it
 * does mean a stray `del` cannot reach the files the machine needs to start,
 * which is the accident worth preventing. */
static void command_part(const ARGUMENTS* arguments) {
    BLOCK_DEVICE* device;
    char name[PATH_MAX];
    char answer[PATH_MAX];
    boot_uint64_t boot_sectors;
    GPT_REQUEST layout[2];

    if (!arguments->operand_count) {
        print_line("part <device>");
        print_line("");
        print_line("Replaces the partition table on a whole disk with a boot");
        print_line("partition and a system partition. Device names are the");
        print_line("ones `disk` prints, like nvme0.");
        return;
    }
    memcpy(name, arguments->operand[0], strlen(arguments->operand[0]) + 1);

    device = device_by_name(name);
    if (!device) {
        console_set_color(console_theme()->error, console_theme()->background);
        print("No disk called ");
        print_line(name);
        console_use_theme();
        return;
    }

    /* Absolutely not the disk we are running from. Rewriting its table while
       the system reads its own files off it is not recoverable. */
    {
        VOLUME* boot = volume_boot();
        if (boot && boot->device == device) {
            console_set_color(console_theme()->error, console_theme()->background);
            print_line("That is the disk this system booted from. Refusing.");
            console_use_theme();
            return;
        }
    }

    /* Big enough for the loader with room to grow, without eating a small
       disk alive. */
    boot_sectors = device->sector_count >= 2048ULL * 1024ULL
                 ? 256ULL * 2048ULL : 64ULL * 2048ULL;

    console_set_color(console_theme()->error, console_theme()->background);
    print_line("");
    print("EVERY PARTITION ON ");
    print(name);
    print_line(" WILL BE DESTROYED.");
    console_use_theme();

    print("  size       ");
    print_size(device->sector_count, device->sector_size);
    print_line("");
    print_line("  currently:");
    {
        int any = 0;
        for (boot_uint32_t index = 0; index < partition_count(); index++) {
            PARTITION* partition = partition_at(index);
            if (!partition || partition->device != device) continue;
            any = 1;
            print("    ");
            print(device->name);
            put('p');
            print_dec(partition->number);
            print("  ");
            print_size(partition->sector_count, device->sector_size);
            if (partition->letter) {
                print("  drive ");
                put(partition->letter);
                print(":");
            }
            print_line("");
        }
        if (!any) print_line("    nothing this system recognises");
    }
    print_line("");
    print_line("  afterwards:");
    print("    ");
    print(name);
    print("p1  ");
    print_size(boot_sectors, device->sector_size);
    print_line("  EFI System - the loader and the kernel");
    print("    ");
    print(name);
    print_line("p2  the rest    the system volume");
    print_line("");
    print_line("This replaces the table only. The sectors themselves are not");
    print_line("touched, so a partition that happens to start where an old one");
    print_line("did will still have the old filesystem on it. Format them.");

    print_line("");
    print("Type the disk name to confirm, anything else to stop: ");
    keyboard_read_line(answer, sizeof(answer));
    serial_write(answer);
    serial_write("\n");
    {
        const char* a = answer;
        const char* b = name;
        while (*a && *b && upper(*a) == upper(*b)) { a++; b++; }
        if (*a || *b) {
            print_line("Stopped. Nothing was written.");
            return;
        }
    }

    memset(layout, 0, sizeof(layout));
    layout[0].sector_count = boot_sectors;
    layout[0].is_efi_system = 1;
    layout[0].name = "KOI-BOOT";
    layout[1].sector_count = 0;              /* the rest */
    layout[1].name = "KOI-SYSTEM";

    print_line("Writing...");
    if (!partition_write_gpt(device, layout, 2)) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("Could not write the partition table. The disk may be too");
        print_line("small, or it rejected a write.");
        console_use_theme();
        return;
    }
    remount_everything();
    print_line("Done. Run `disk` to see it, then `format` each partition.");
}

/* A percentage that overwrites itself, so a long format says something
   without scrolling the screen away. */
static void format_progress(boot_uint64_t done, boot_uint64_t total) {
    static boot_uint64_t last = 200;
    boot_uint64_t percent = total ? done * 100U / total : 100;

    if (percent == last) return;
    last = percent >= 100 ? 200 : percent;

    put('\r');
    print("    clearing the allocation tables: ");
    print_dec(percent);
    print("%   ");
    if (percent >= 100) print_line("");
}

/* Turn the machine off, or restart it.
 *
 * Neither existed in MS-DOS, and for a good reason: a machine of that era had
 * a switch that physically cut the power, so there was nothing for software to
 * do. On anything since, the button is a request the firmware interprets, and
 * ACPI is how software makes the same request. Reaching behind a desk to hold
 * a power button is not a design decision anyone made on purpose.
 *
 * Both flush nothing, because nothing is buffered: every write in this system
 * has already reached the device by the time the command returns. If that ever
 * stops being true, this is where the flush belongs. */
static void command_shutdown(void) {
    print_line("Shutting down.");
    console_show_cursor(0);

    if (acpi_power_off()) return;    /* it does not return on success */

    console_set_color(console_theme()->error, console_theme()->background);
    print_line("");
    print_line("This machine did not turn off. Its firmware did not describe a");
    print_line("way to, or refused. Use the power button.");
    console_use_theme();
}

static void command_reboot(void) {
    print_line("Restarting.");
    console_show_cursor(0);

    /* ACPI first because it is the machine's own answer. The fallback needs
       nothing at all: an empty interrupt table and one interrupt, so the fault
       about the fault about the fault resets the processor. Crude, universal,
       and the only thing left when the tables say nothing. */
    (void)acpi_reset();
    cpu_reset();
}

/* Install Koi-DOS onto a disk.
 *
 * The shape follows the text-mode installers this is descended from, and the
 * order is the point: say what this is, show the licence, let a disk be chosen,
 * warn plainly, and only then touch anything. Nothing is written until the last
 * confirmation, and the disk the system is running from is never offered.
 *
 * It lays down two partitions. The loader gets one of its own so that a
 * mistaken `del` in the system volume cannot take away the machine's ability
 * to start - not protection against anything deliberate, but the accident
 * worth preventing. */
static void command_setup(void) {
    BLOCK_DEVICE* targets[BLOCK_MAX_DEVICES];
    boot_uint32_t target_count = 0;
    BLOCK_DEVICE* target;
    VOLUME* loader_source;
    VOLUME* system_source;
    VOLUME* boot_target = 0;
    VOLUME* system_target = 0;
    char answer[PATH_MAX];
    boot_uint64_t boot_sectors;
    GPT_REQUEST layout[2];
    int key;

    setup_banner("Welcome");
    print_line("  This installs Koi-DOS onto a disk in this machine.");
    print_line("");
    print_line("  The disk you choose will be erased completely. Everything on");
    print_line("  it - every partition, every file, any other operating system");
    print_line("  - will be gone and will not be recoverable.");
    print_line("");
    print_line("  The disk this system is running from is never offered.");
    print_line("");
    console_set_color(KOI_SETUP_TITLE_FOREGROUND, console_theme()->background);
    print_line("  ENTER to continue, any other key to quit.");
    console_use_theme();
    if (keyboard_getchar() != '\n') return;

    /* The licence, from whichever volume of the media carries it. */
    {
        VOLUME* licence = volume_holding("\\LICENSE");
        if (!licence || !setup_show_licence(licence)) {
            setup_banner("Stopped");
            print_line("  The licence was not accepted. Nothing has been changed.");
            print_line("");
            return;
        }
    }

    /* Where the pieces are copied from. On single-volume media both are the
       same volume; on a two-partition one they are not. */
    loader_source = volume_holding("\\EFI\\BOOT\\BOOTX64.EFI");
    system_source = volume_holding("\\BIN");
    if (!system_source) system_source = volume_boot();
    if (!loader_source || !system_source) {
        setup_banner("Stopped");
        print_line("  This media is missing the loader or the utilities.");
        print_line("  Setup cannot continue.");
        print_line("");
        return;
    }

    /* Every disk except the one we are running from. */
    setup_banner("Choose a disk");
    {
        VOLUME* boot = volume_boot();
        for (boot_uint32_t index = 0; index < block_device_count(); index++) {
            BLOCK_DEVICE* device = block_device(index);
            if (!device) continue;
            if (boot && device == boot->device) continue;
            if (target_count >= BLOCK_MAX_DEVICES) break;
            targets[target_count] = device;

            print("   ");
            print_dec(target_count + 1);
            print(". ");
            print(device->name);
            print("   ");
            print_size(device->sector_count, device->sector_size);
            print_line("");
            for (boot_uint32_t p = 0; p < partition_count(); p++) {
                PARTITION* partition = partition_at(p);
                if (!partition || partition->device != device) continue;
                print("        contains ");
                print_size(partition->sector_count, device->sector_size);
                if (partition->letter) {
                    print(" as drive ");
                    put(partition->letter);
                    print(":");
                } else if (partition->is_efi_system) {
                    print(" EFI System");
                } else {
                    print(" of a kind this system does not read");
                }
                print_line("");
            }
            target_count++;
        }
    }
    if (!target_count) {
        print_line("  There is no disk to install to - only the one this");
        print_line("  system is running from.");
        print_line("");
        print("  Press a key. ");
        (void)keyboard_getchar();
        return;
    }

    print_line("");
    console_set_color(KOI_SETUP_TITLE_FOREGROUND, console_theme()->background);
    print("  Press the number of a disk, or any other key to quit: ");
    console_use_theme();
    key = keyboard_getchar();
    print_line("");
    if (key < '1' || key >= '1' + (int)target_count) return;
    target = targets[key - '1'];

    /* The last warning, and the only place a mistake still costs nothing. */
    setup_banner("Last chance");
    console_set_color(console_theme()->error, console_theme()->background);
    print("  EVERYTHING ON ");
    print(target->name);
    print_line(" IS ABOUT TO BE DESTROYED.");
    console_use_theme();
    print_line("");
    print("  disk    ");
    print(target->name);
    print("   ");
    print_size(target->sector_count, target->sector_size);
    print_line("");
    print_line("  layout  a boot partition for the loader, and the rest as");
    print_line("          the system volume");
    print_line("");
    print_line("  After this, remove the installation media and restart.");
    print_line("");
    print("  Type the disk name to begin, anything else to stop: ");
    keyboard_read_line(answer, sizeof(answer));
    serial_write(answer);
    serial_write("\n");
    {
        const char* a = answer;
        const char* b = target->name;
        while (*a && *b && upper(*a) == upper(*b)) { a++; b++; }
        if (*a || *b) {
            print_line("  Stopped. Nothing was written.");
            return;
        }
    }

    setup_banner("Installing");
    boot_sectors = target->sector_count >= 2048ULL * 1024ULL
                 ? 256ULL * 2048ULL : 64ULL * 2048ULL;

    print_line("  Writing the partition table");
    memset(layout, 0, sizeof(layout));
    layout[0].sector_count = boot_sectors;
    layout[0].is_efi_system = 1;
    layout[0].name = "KOI-BOOT";
    layout[1].sector_count = 0;
    layout[1].name = "KOI-SYSTEM";
    if (!partition_write_gpt(target, layout, 2)) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("  Failed. The disk may be too small.");
        console_use_theme();
        return;
    }
    remount_everything();

    print_line("  Making the filesystems");
    {
        RTC_TIME now;
        boot_uint32_t serial;
        PARTITION* first = setup_partition(target, 1);
        PARTITION* second = setup_partition(target, 2);

        rtc_read(&now);
        serial = ((boot_uint32_t)rtc_fat_date(&now) << 16) | rtc_fat_time(&now);

        if (!first || !second ||
            !fat32_format(target, first->first_sector, first->sector_count,
                          "KOI-BOOT", serial ^ 0x4B4F4901U,
                          format_progress) ||
            !fat32_format(target, second->first_sector, second->sector_count,
                          "KOI-DOS", serial ^ 0x4B4F4902U,
                          format_progress)) {
            console_set_color(console_theme()->error, console_theme()->background);
            print_line("  Failed.");
            console_use_theme();
            remount_everything();
            return;
        }
    }
    remount_everything();

    /* The volumes have letters now - or in the loader partition's case, do
       not - so they are found by which partition they sit on rather than by
       name. */
    {
        PARTITION* first = setup_partition(target, 1);
        PARTITION* second = setup_partition(target, 2);
        for (boot_uint32_t index = 0; index < volume_count(); index++) {
            VOLUME* volume = volume_at(index);
            if (!volume || volume->device != target) continue;
            if (first && volume->first_sector == first->first_sector)
                boot_target = volume;
            if (second && volume->first_sector == second->first_sector)
                system_target = volume;
        }
    }
    if (!boot_target || !system_target) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("  The new filesystems could not be mounted.");
        console_use_theme();
        return;
    }

    print_line("  Copying the loader");
    if (!fat32_create(boot_target, "\\EFI", 1, &(FAT_ENTRY){0}) ||
        !fat32_create(boot_target, "\\EFI\\BOOT", 1, &(FAT_ENTRY){0}) ||
        !fat32_create(boot_target, "\\BOOT", 1, &(FAT_ENTRY){0})) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("  Could not create the directories.");
        console_use_theme();
        return;
    }
    if (!setup_copy(loader_source, "\\EFI\\BOOT\\BOOTX64.EFI",
                    boot_target, "\\EFI\\BOOT\\BOOTX64.EFI")) return;
    if (!setup_copy(loader_source, "\\BOOT\\KERNEL.ELF",
                    boot_target, "\\BOOT\\KERNEL.ELF")) return;

    print_line("  Copying the system");
    (void)fat32_create(system_target, "\\BIN", 1, &(FAT_ENTRY){0});
    (void)fat32_create(system_target, "\\BOOT", 1, &(FAT_ENTRY){0});
    {
        FAT_DIRECTORY directory;
        FAT_ENTRY entry;
        if (fat32_opendir(system_source, "\\BIN", &directory)) {
            while (fat32_readdir(&directory, &entry)) {
                char from[PATH_MAX];
                char to[PATH_MAX];
                boot_uint64_t length;

                if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) continue;
                length = strlen(entry.name);
                if (length + 6 >= PATH_MAX) continue;
                memcpy(from, "\\BIN\\", 5);
                memcpy(from + 5, entry.name, length + 1);
                memcpy(to, from, length + 6);
                if (!setup_copy(system_source, from, system_target, to)) return;
            }
        }
    }
    (void)setup_copy(system_source, "\\LICENSE", system_target, "\\LICENSE");

    /* The marker last, because it is what makes the installation real: it is
       how the kernel decides which volume is the system one, and until it
       exists the install is not one. */
    print_line("  Marking the system volume");
    {
        FAT_ENTRY marker;
        const char* text = "Koi-DOS system volume.\r\n";
        if (!fat32_create(system_target, SYSTEM_VOLUME_MARKER, 0, &marker) ||
            fat32_write(system_target, &marker, 0, text,
                        (boot_uint32_t)strlen(text)) == 0) {
            console_set_color(console_theme()->error, console_theme()->background);
            print_line("  Failed. The installed system would not find itself.");
            console_use_theme();
            return;
        }
    }

    setup_banner("Done");
    print_line("  Koi-DOS is installed.");
    print_line("");
    print_line("  Remove the installation media before restarting, or the");
    print_line("  machine will simply start it again.");
    print_line("");
    console_set_color(KOI_SETUP_TITLE_FOREGROUND, console_theme()->background);
    print_line("  ENTER to restart, any other key to return to the prompt.");
    console_use_theme();
    if (keyboard_getchar() == '\n') cpu_reset();
    console_clear();
}

/* Make a filesystem, having first made very sure of what is about to be lost.
 *
 * The confirmation asks for the partition's name rather than a yes, because a
 * yes is what someone types without reading. Writing out "nvme0p2" requires
 * having looked at which partition this is. */
static void command_format(const ARGUMENTS* arguments) {
    PARTITION* partition;
    char name[PATH_MAX];
    char answer[PATH_MAX];
    const char* label;

    if (!arguments->operand_count) {
        print_line("format <partition> [label]");
        print_line("");
        print_line("Partition names are the ones `disk` prints, like nvme0p2.");
        return;
    }
    memcpy(name, arguments->operand[0], strlen(arguments->operand[0]) + 1);
    label = arguments->operand_count > 1 ? arguments->operand[1] : "";

    partition = partition_by_name(name);
    if (!partition) {
        console_set_color(console_theme()->error, console_theme()->background);
        print("No partition called ");
        print_line(name);
        console_use_theme();
        print_line("Run `disk` to see what there is.");
        return;
    }

    /* The one refusal with no override. Formatting the volume we are running
       from destroys the running system mid-write, and there is no version of
       that which ends well. */
    {
        VOLUME* boot = volume_boot();
        if (boot && boot->device == partition->device &&
            boot->first_sector == partition->first_sector) {
            console_set_color(console_theme()->error, console_theme()->background);
            print_line("That is the volume this system booted from. Refusing.");
            console_use_theme();
            return;
        }
    }

    console_set_color(console_theme()->error, console_theme()->background);
    print_line("");
    print("EVERYTHING ON ");
    print(name);
    print_line(" WILL BE DESTROYED.");
    console_use_theme();

    print("  device     ");
    print_line(partition->device->name);
    print("  size       ");
    print_size(partition->sector_count, partition->device->sector_size);
    print_line("");
    print("  at sector  ");
    print_dec(partition->first_sector);
    print_line("");
    print("  currently  ");
    if (partition->letter) {
        print("drive ");
        put(partition->letter);
        print_line(": - a filesystem with files on it");
    } else if (partition->is_fat) {
        print_line("a FAT filesystem this system did not mount");
    } else {
        print_line("not a filesystem this system understands - which does");
        print_line("             not mean it is empty");
    }

    print_line("");
    print("Type the partition name to confirm, anything else to stop: ");
    keyboard_read_line(answer, sizeof(answer));
    serial_write(answer);
    serial_write("\n");

    {
        const char* a = answer;
        const char* b = name;
        while (*a && *b && upper(*a) == upper(*b)) { a++; b++; }
        if (*a || *b) {
            print_line("Stopped. Nothing was written.");
            return;
        }
    }

    print_line("Writing...");
    {
        RTC_TIME now;
        boot_uint32_t serial;

        rtc_read(&now);
        /* A serial that differs from every other volume, because the boot
           volume is identified by exactly this number. Date and time together
           are enough - two volumes formatted in the same second on the same
           machine is not a case worth engineering for. */
        serial = ((boot_uint32_t)rtc_fat_date(&now) << 16) | rtc_fat_time(&now);
        serial ^= (boot_uint32_t)partition->first_sector;

        if (!fat32_format(partition->device, partition->first_sector,
                          partition->sector_count, label, serial,
                          format_progress)) {
            console_set_color(console_theme()->error, console_theme()->background);
            print_line("Format failed. The partition may be too small for FAT32,");
            print_line("or the device rejected a write.");
            console_use_theme();
            remount_everything();
            return;
        }
    }

    remount_everything();
    print_line("Done.");
    print_line("Run `disk` to see the result; the drive letters may have moved.");
}

/* Every function on the bus, as the kernel sees it.
 *
 * This exists because a driver that reports "not found" says nothing about
 * why: the device may be absent, behind a bridge nobody walked, past the end
 * of a table that silently filled up, or simply of a kind we do not drive. On
 * real hardware, with no serial cable attached, this is the only way to tell
 * those apart. */
static void command_pci(void) {
    print_line("BUS:DEV.F  VENDOR:DEVICE  CLASS");
    for (boot_uint32_t index = 0; index < pci_device_count(); index++) {
        const PCI_DEVICE* device = pci_device(index);
        if (!device) continue;
        print_hex(device->bus, 2);
        put(':');
        print_hex(device->device, 2);
        put('.');
        print_hex(device->function, 1);
        print("     ");
        print_hex(device->vendor_id, 4);
        put(':');
        print_hex(device->device_id, 4);
        print("     ");
        print(class_name(device));
        /* The raw triple, for anything the table above does not name. */
        print("  [");
        print_hex(device->class_code, 2);
        put('/');
        print_hex(device->subclass, 2);
        put('/');
        print_hex(device->programming_interface, 2);
        print_line("]");
    }
    print("");
    print_dec(pci_device_count());
    print(" device(s)");
    if (pci_devices_seen() > pci_device_count()) {
        print(" - ");
        print_dec(pci_devices_seen() - pci_device_count());
        print(" MORE WERE DROPPED, the table is full");
    }
    print_line("");
}

static void print_entry_line(const FAT_ENTRY* entry) {
    boot_uint32_t year = 1980U + (entry->modified_date >> 9);
    boot_uint32_t month = (entry->modified_date >> 5) & 0x0FU;
    boot_uint32_t day = entry->modified_date & 0x1FU;
    boot_uint32_t hour = (entry->modified_time >> 11) & 0x1FU;
    boot_uint32_t minute = (entry->modified_time >> 5) & 0x3FU;

    print_dec(year);
    put('-');
    print_two_digits(month);
    put('-');
    print_two_digits(day);
    print("  ");
    print_two_digits(hour);
    put(':');
    print_two_digits(minute);

    if (entry->attributes & FAT_ATTRIBUTE_DIRECTORY) {
        print("        <DIR>  ");
    } else {
        print_dec_padded(entry->size, 13);
        print("  ");
    }
    print_line(entry->name);
}

static void command_dir(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    char directory[PATH_MAX];
    char pattern[PATH_MAX];
    VOLUME* volume;
    FAT_DIRECTORY listing;
    FAT_ENTRY entry;
    boot_uint32_t files = 0;
    boot_uint32_t directories = 0;
    boot_uint64_t bytes = 0;
    int wide = has_switch(arguments, 'W');
    int column = 0;

    if (!current_volume) { print_line("No volume."); return; }

    single_operand(arguments, name);
    if (name[0]) {
        if (!resolve_path(name, &volume, path)) {
            print_line("Invalid drive.");
            return;
        }
    } else {
        volume = current_volume;
        memcpy(path, current_path, strlen(current_path) + 1);
    }

    if (has_wildcard(path)) {
        split_leaf(path, directory, pattern);
    } else if (fat32_stat(volume, path, &entry) &&
               (entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) {
        memcpy(directory, path, strlen(path) + 1);
        pattern[0] = '*';
        pattern[1] = 0;
    } else {
        /* A plain file name, or something that is not there at all. Listing
           the parent filtered by the name is what DOS did, and it produces
           the right "File not found" without a separate check. */
        split_leaf(path, directory, pattern);
    }

    if (!fat32_opendir(volume, directory, &listing)) {
        print_line("Directory not found.");
        return;
    }

    print(" Volume in drive ");
    put(volume->letter);
    if (volume->label[0]) { print(" is "); print(volume->label); }
    print("\n Directory of ");
    put(volume->letter);
    put(':');
    print_line(directory);
    print("\n");

    while (fat32_readdir(&listing, &entry)) {
        if (!glob_match(pattern, entry.name)) continue;
        if (wide) {
            /* Names only, five to a line - the classic /W listing. A name too
               wide for its column takes a line of its own rather than pushing
               everything after it out of alignment. */
            boot_uint64_t length = strlen(entry.name);
            if (length >= 24) {
                if (column) { print("\n"); column = 0; }
                print_line(entry.name);
            } else {
                print(entry.name);
                for (boot_uint64_t pad = length; pad < 24; pad++) print(" ");
                if (++column == 5) { print("\n"); column = 0; }
            }
        } else {
            print_entry_line(&entry);
        }
        if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) directories++;
        else { files++; bytes += entry.size; }
    }
    if (wide && column) print("\n");

    if (!files && !directories) {
        print_line("File not found");
        return;
    }

    print("\n");
    print_dec_padded(files, 8);
    print(" file(s)");
    print_dec_padded(bytes, 16);
    print_line(" bytes");
    print_dec_padded(directories, 8);
    print(" dir(s) ");
    print_dec_padded(fat32_free_bytes(volume), 15);
    print_line(" bytes free");
}

static void command_cd(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    VOLUME* volume;
    FAT_ENTRY entry;

    if (!current_volume) { print_line("No volume."); return; }
    single_operand(arguments, name);
    /* Bare `cd` prints the current directory, as it always has. */
    if (!name[0]) { print_line(current_path); return; }

    if (name[0] == '.' && name[1] == '.' && !name[2]) { path_up(); return; }
    if (name[0] == '.' && !name[1]) return;

    if (!resolve_path(name, &volume, path)) {
        print_line("Invalid drive.");
        return;
    }
    if (!fat32_stat(volume, path, &entry)) {
        print_line("Path not found.");
        return;
    }
    if (!(entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) {
        print_line("Not a directory.");
        return;
    }
    current_volume = volume;
    memcpy(current_path, path, strlen(path) + 1);
}

static void command_type(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    VOLUME* volume;
    FAT_ENTRY entry;
    char* buffer;
    boot_uint32_t offset = 0;

    if (!current_volume) { print_line("No volume."); return; }
    single_operand(arguments, name);
    if (!name[0]) { print_line("Usage: type <file>"); return; }

    if (!resolve_path(name, &volume, path)) {
        print_line("Invalid drive.");
        return;
    }
    if (!fat32_stat(volume, path, &entry)) {
        print_line("File not found.");
        return;
    }
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) {
        print_line("That is a directory.");
        return;
    }

    buffer = (char*)kmalloc(512);
    if (!buffer) { print_line("Out of memory."); return; }

    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset, buffer, 512);
        if (!got) break;
        for (boot_uint32_t index = 0; index < got; index++) {
            char character = buffer[index];
            /* Files arrive with CRLF; the console supplies its own carriage
               return, so passing both through would double-space everything. */
            if (character == '\r') continue;
            put(character);
        }
        offset += got;
    }
    if (entry.size) print("\n");
    kfree(buffer);
}

/* Both operands of a two-operand command, resolved. Returns 0 with a message
   already printed when the line does not carry two of them. */
static int two_operands(const ARGUMENTS* arguments, const char* usage,
                        VOLUME** from_volume, char* from_path,
                        VOLUME** to_volume, char* to_path) {
    if (arguments->operand_count != 2) {
        print("Usage: ");
        print_line(usage);
        return 0;
    }
    if (!resolve_path(arguments->operand[0], from_volume, from_path) ||
        !resolve_path(arguments->operand[1], to_volume, to_path)) {
        print_line("Invalid drive.");
        return 0;
    }
    return 1;
}

static void command_mkdir(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    VOLUME* volume;
    FAT_ENTRY entry;

    if (!current_volume) { print_line("No volume."); return; }
    single_operand(arguments, name);
    if (!name[0]) { print_line("Usage: md <directory>"); return; }
    if (!resolve_path(name, &volume, path)) { print_line("Invalid drive."); return; }
    if (!fat32_create(volume, path, 1, &entry))
        print_line("Unable to create directory.");
}

static void command_rmdir(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    VOLUME* volume;
    FAT_ENTRY entry;

    if (!current_volume) { print_line("No volume."); return; }
    single_operand(arguments, name);
    if (!name[0]) { print_line("Usage: rd <directory>"); return; }
    if (!resolve_path(name, &volume, path)) { print_line("Invalid drive."); return; }
    if (!fat32_stat(volume, path, &entry)) { print_line("Path not found."); return; }
    if (!(entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) {
        print_line("Not a directory.");
        return;
    }
    if (!fat32_remove(volume, path))
        print_line("The directory is not empty.");
}

/* Ask before doing something sweeping. `del *.*` was the classic way to lose
   a directory by accident, and DOS prompted for exactly this. */
static int confirmed(const char* question) {
    int key;
    print(question);
    print(" (Y/N) ");
    for (;;) {
        key = keyboard_getchar();
        if (key == 'y' || key == 'Y') { print_line("Y"); return 1; }
        if (key == 'n' || key == 'N' || key == 27) { print_line("N"); return 0; }
    }
}

static void command_del(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    char directory[PATH_MAX];
    char pattern[PATH_MAX];
    char target[PATH_MAX];
    VOLUME* volume;
    FAT_ENTRY entry;
    boot_uint32_t deleted = 0;

    if (!current_volume) { print_line("No volume."); return; }
    single_operand(arguments, name);
    if (!name[0]) { print_line("Usage: del <file>"); return; }
    if (!resolve_path(name, &volume, path)) { print_line("Invalid drive."); return; }

    if (!has_wildcard(path)) {
        if (!fat32_stat(volume, path, &entry)) { print_line("File not found."); return; }
        if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) {
            /* DOS made this mistake impossible by refusing; so do we. */
            print_line("That is a directory. Use rd.");
            return;
        }
        if (!fat32_remove(volume, path)) print_line("Unable to delete.");
        return;
    }

    split_leaf(path, directory, pattern);
    if (!confirmed("Delete matching files?")) return;

    /* Find one match, delete it, start again.
       Deleting while iterating would mean reasoning about what the directory
       walk does to entries that vanish underneath it; restarting costs a pass
       per file and is obviously correct. Directories here hold tens of files,
       not thousands. */
    for (;;) {
        FAT_DIRECTORY listing;
        int found = 0;

        if (!fat32_opendir(volume, directory, &listing)) break;
        while (fat32_readdir(&listing, &entry)) {
            if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) continue;
            if (!glob_match(pattern, entry.name)) continue;
            found = 1;
            break;
        }
        if (!found) break;

        {
            boot_uint64_t length = strlen(directory);
            memcpy(target, directory, length);
            if (length && target[length - 1] != '\\') target[length++] = '\\';
            memcpy(target + length, entry.name, strlen(entry.name) + 1);
        }
        if (!fat32_remove(volume, target)) {
            print("Unable to delete ");
            print_line(entry.name);
            break;
        }
        print("Deleted ");
        print_line(entry.name);
        deleted++;
    }

    if (!deleted) print_line("File not found.");
}

static void command_ren(const ARGUMENTS* arguments) {
    char from[PATH_MAX];
    char to[PATH_MAX];
    VOLUME* from_volume;
    VOLUME* to_volume;

    if (!current_volume) { print_line("No volume."); return; }
    if (!two_operands(arguments, "ren <old> <new>",
                      &from_volume, from, &to_volume, to)) return;
    if (from_volume != to_volume) {
        print_line("Cannot rename across drives.");
        return;
    }
    if (!fat32_rename(from_volume, from, to))
        print_line("Unable to rename.");
}

/* Copy one file. Shared by the plain and the wildcard forms. */
static int copy_one(VOLUME* from_volume, const char* from,
                    VOLUME* to_volume, const char* to, boot_uint32_t* copied) {
    FAT_ENTRY source;
    FAT_ENTRY destination;
    FAT_ENTRY existing;
    char* buffer;
    /* Bigger is straightforwardly faster: the filesystem turns one call into
       one disk command per run of sectors, so a small buffer means the drive
       spends its time answering rather than transferring. */
    boot_uint32_t size = 65536;
    boot_uint32_t offset = 0;
    int ok = 1;

    if (!fat32_stat(from_volume, from, &source)) return 0;
    if (source.attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;

    /* Copying over a file that is already there replaces it - that is what
       copy has always meant. It has to be removed first: fat32_create refuses
       a name that is taken, and without this the second copy of anything
       failed with nothing to say why. */
    if (fat32_stat(to_volume, to, &existing)) {
        if (existing.attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;
        /* Onto itself is not a copy, it is a way to lose the file: the remove
           below would take the only copy and there would be nothing left to
           read from. Two names are the same file when they share a directory
           entry on the same volume. */
        if (from_volume == to_volume &&
            source.entry_sector == existing.entry_sector &&
            source.entry_offset == existing.entry_offset) return 0;
        if (!fat32_remove(to_volume, to)) return 0;
    }
    if (!fat32_create(to_volume, to, 0, &destination)) return 0;

    while (!(buffer = (char*)kmalloc(size)) && size > 4096) size /= 2;
    if (!buffer) return 0;

    while (offset < source.size) {
        boot_uint32_t got = fat32_read(from_volume, &source, offset, buffer, size);
        if (!got) { ok = 0; break; }
        if (fat32_write(to_volume, &destination, offset, buffer, got) != got) {
            ok = 0;
            break;
        }
        offset += got;
    }
    kfree(buffer);
    if (copied) *copied = offset;
    return ok;
}

/* Append a leaf to a directory path. */
static void join(const char* directory, const char* leaf, char* result) {
    boot_uint64_t length = strlen(directory);
    if (length >= PATH_MAX) length = PATH_MAX - 1;
    memcpy(result, directory, length);
    if (length && result[length - 1] != '\\' && length + 1 < PATH_MAX)
        result[length++] = '\\';
    for (boot_uint64_t index = 0; leaf[index] && length + 1 < PATH_MAX; index++)
        result[length++] = leaf[index];
    result[length] = 0;
}

static void command_copy(const ARGUMENTS* arguments) {
    char from[PATH_MAX];
    char to[PATH_MAX];
    VOLUME* from_volume;
    VOLUME* to_volume;
    FAT_ENTRY existing;
    boot_uint32_t copied = 0;
    boot_uint32_t count = 0;

    if (!current_volume) { print_line("No volume."); return; }
    if (!two_operands(arguments, "copy <source> <destination>",
                      &from_volume, from, &to_volume, to)) return;

    if (has_wildcard(from)) {
        char directory[PATH_MAX];
        char pattern[PATH_MAX];
        char source[PATH_MAX];
        char target[PATH_MAX];
        FAT_DIRECTORY listing;
        FAT_ENTRY entry;

        /* Several sources need somewhere to put them, so the destination has
           to be a directory that already exists. */
        if (!fat32_stat(to_volume, to, &existing) ||
            !(existing.attributes & FAT_ATTRIBUTE_DIRECTORY)) {
            print_line("The destination must be an existing directory.");
            return;
        }

        split_leaf(from, directory, pattern);
        if (!fat32_opendir(from_volume, directory, &listing)) {
            print_line("Directory not found.");
            return;
        }
        while (fat32_readdir(&listing, &entry)) {
            if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) continue;
            if (!glob_match(pattern, entry.name)) continue;
            join(directory, entry.name, source);
            join(to, entry.name, target);
            if (!copy_one(from_volume, source, to_volume, target, &copied)) {
                print("Failed to copy ");
                print_line(entry.name);
                continue;
            }
            print(entry.name);
            print("\n");
            count++;
        }
        if (!count) { print_line("File not found."); return; }
        print_dec_padded(count, 9);
        print_line(" file(s) copied");
        return;
    }

    /* Copying onto an existing directory means "into it", which needs the
       leaf name appended. Anything else would silently replace the folder. */
    if (fat32_stat(to_volume, to, &existing) &&
        (existing.attributes & FAT_ATTRIBUTE_DIRECTORY)) {
        char leaf[PATH_MAX];
        char directory[PATH_MAX];
        char joined[PATH_MAX];
        split_leaf(from, directory, leaf);
        join(to, leaf, joined);
        memcpy(to, joined, strlen(joined) + 1);
    }

    if (!copy_one(from_volume, from, to_volume, to, &copied)) {
        print_line("Copy failed.");
        print_device_reason();
        return;
    }
    print("        1 file(s) copied, ");
    print_dec(copied);
    print_line(" bytes");
}

static void run_batch(VOLUME* volume, const char* path);

/* Page a file a screenful at a time. */
static void command_more(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    VOLUME* volume;
    FAT_ENTRY entry;
    char* buffer;
    boot_uint32_t offset = 0;
    boot_uint32_t lines = 0;
    boot_uint32_t page = console_rows() - 1;

    if (!current_volume) { print_line("No volume."); return; }
    single_operand(arguments, name);
    if (!name[0]) { print_line("Usage: more <file>"); return; }
    if (!resolve_path(name, &volume, path)) { print_line("Invalid drive."); return; }
    if (!fat32_stat(volume, path, &entry)) { print_line("File not found."); return; }
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) {
        print_line("That is a directory.");
        return;
    }

    buffer = (char*)kmalloc(512);
    if (!buffer) { print_line("Out of memory."); return; }

    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset, buffer, 512);
        if (!got) break;
        for (boot_uint32_t index = 0; index < got; index++) {
            char character = buffer[index];
            if (character == '\r') continue;
            put(character);
            if (character != '\n') continue;
            if (++lines < page) continue;
            /* -- More -- and wait, the way the DOS filter did. */
            console_set_color(console_theme()->background, console_theme()->foreground);
            print("-- More --");
            console_use_theme();
            if (keyboard_getchar() == 27) { offset = entry.size; break; }
            print("\n");
            lines = 0;
        }
        offset += got;
    }
    print("\n");
    kfree(buffer);
}

/* Walk a directory tree. Depth is capped rather than trusted: a directory
   whose .. has been corrupted into a loop would otherwise recurse until the
   kernel stack ran out. */
#define TREE_DEPTH_MAX 16

static void tree_walk(VOLUME* volume, const char* path, int depth) {
    FAT_DIRECTORY listing;
    FAT_ENTRY entry;
    char child[PATH_MAX];

    if (depth >= TREE_DEPTH_MAX) return;
    if (!fat32_opendir(volume, path, &listing)) return;

    while (fat32_readdir(&listing, &entry)) {
        if (!(entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) continue;
        if (entry.name[0] == '.' && !entry.name[1]) continue;
        if (entry.name[0] == '.' && entry.name[1] == '.' && !entry.name[2]) continue;

        for (int level = 0; level < depth; level++) print("   ");
        put((char)0xC3);          /* right tee */
        put((char)0xC4);
        put((char)0xC4);
        put(' ');
        print_line(entry.name);

        join(path, entry.name, child);
        tree_walk(volume, child, depth + 1);
    }
}

static void command_tree(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    char name[PATH_MAX];
    VOLUME* volume;

    if (!current_volume) { print_line("No volume."); return; }
    single_operand(arguments, name);
    if (name[0]) {
        if (!resolve_path(name, &volume, path)) {
            print_line("Invalid drive.");
            return;
        }
    } else {
        volume = current_volume;
        memcpy(path, current_path, strlen(current_path) + 1);
    }

    put(volume->letter);
    put(':');
    print_line(path);
    tree_walk(volume, path, 0);
}

/* attrib with no letters reports; +R -H and so on change. */
static void command_attrib(const ARGUMENTS* arguments) {
    char path[PATH_MAX];
    VOLUME* volume;
    FAT_ENTRY entry;
    boot_uint8_t attributes;
    int changing = 0;

    if (!current_volume) { print_line("No volume."); return; }
    if (!arguments->operand_count) { print_line("Usage: attrib [+-RHSA] <file>"); return; }

    /* The last operand is the file; anything before it that starts with + or -
       is a change to make. */
    if (!resolve_path(arguments->operand[arguments->operand_count - 1],
                      &volume, path)) {
        print_line("Invalid drive.");
        return;
    }
    if (!fat32_stat(volume, path, &entry)) { print_line("File not found."); return; }
    attributes = entry.attributes;

    for (int index = 0; index < arguments->operand_count - 1; index++) {
        const char* token = arguments->operand[index];
        int adding = token[0] == '+';
        boot_uint8_t bit = 0;

        if (token[0] != '+' && token[0] != '-') continue;
        switch (upper(token[1])) {
        case 'R': bit = FAT_ATTRIBUTE_READ_ONLY; break;
        case 'H': bit = FAT_ATTRIBUTE_HIDDEN; break;
        case 'S': bit = FAT_ATTRIBUTE_SYSTEM; break;
        case 'A': bit = FAT_ATTRIBUTE_ARCHIVE; break;
        default: continue;
        }
        attributes = (boot_uint8_t)(adding ? (attributes | bit)
                                           : (attributes & ~bit));
        changing = 1;
    }

    if (changing && !fat32_set_attributes(volume, &entry, attributes)) {
        print_line("Unable to change attributes.");
        return;
    }

    put(attributes & FAT_ATTRIBUTE_ARCHIVE ? 'A' : ' ');
    put(attributes & FAT_ATTRIBUTE_SYSTEM ? 'S' : ' ');
    put(attributes & FAT_ATTRIBUTE_HIDDEN ? 'H' : ' ');
    put(attributes & FAT_ATTRIBUTE_READ_ONLY ? 'R' : ' ');
    print("   ");
    print_line(path);
}

/* An unrecognised word is looked up as a program, the way COMMAND.COM always
   did: the built-in commands are tried first, and anything left over is a file
   to load. `.EXE` is appended when the user did not write it.
   Search order is the current directory then the root of the current drive -
   a two-entry PATH, which is all a system without one needs. */
static int try_program(const char* input, const ARGUMENTS* arguments) {
    char name[PATH_MAX];
    char path[PATH_MAX];
    boot_uint64_t length = 0;
    int has_extension = 0;
    FAT_ENTRY entry;
    int code;
    int exit_code = 0;

    if (!current_volume) return 0;

    while (input[length] && !is_space(input[length]) && length + 5 < PATH_MAX) {
        if (input[length] == '.') has_extension = 1;
        name[length] = input[length];
        length++;
    }
    if (!length) return 0;
    if (!has_extension) {
        const char* extension = ".EXE";
        for (int index = 0; index < 4; index++) name[length++] = extension[index];
    }
    name[length] = 0;

    /* The current directory, then the root, then \BIN. */
    {
        int found = 0;
        for (int place = 0; place < PROGRAM_SEARCH_PLACES && !found; place++) {
            build_program_path(name, path, place);
            found = fat32_stat(current_volume, path, &entry);
        }
        if (!found) {
            /* Nothing by that name as a program; try it as a batch file. */
            if (has_extension) return 0;
            name[length - 4] = 0;
            {
                const char* batch = ".BAT";
                for (int index = 0; index < 4; index++) name[length - 4 + index] = batch[index];
            }
            for (int place = 0; place < PROGRAM_SEARCH_PLACES && !found; place++) {
                build_program_path(name, path, place);
                found = fat32_stat(current_volume, path, &entry);
            }
            if (!found) return 0;
            run_batch(current_volume, path);
            return 1;
        }
    }
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;

    /* An explicit .BAT is a batch file, not a program image. */
    if (length > 4 && (name[length - 3] == 'B' || name[length - 3] == 'b') &&
        (name[length - 2] == 'A' || name[length - 2] == 'a') &&
        (name[length - 1] == 'T' || name[length - 1] == 't')) {
        run_batch(current_volume, path);
        return 1;
    }

    syscall_set_location(current_volume, current_path);
    code = program_run(current_volume, path, arguments->tail, &exit_code);
    syscall_close_all();
    /* And take the screen back, whether or not the program gave it up. A
       program that returns while still holding it would otherwise leave the
       shell invisible with no way to ask for it back - which is precisely the
       failure this mode was shaped to avoid. */
    graphics_leave();

    if (code == PROGRAM_NOT_LOADABLE) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("Not a valid Koi-DOS program.");
        console_use_theme();
    } else if (code == PROGRAM_REFUSED) {
        /* program_run() has already said why, in more detail than this could. */
    } else if (exit_code != 0) {
        /* DOS reported a non-zero exit only through ERRORLEVEL in batch files;
           printing it is more use at an interactive prompt. A negative one is
           printed as itself rather than as a very large unsigned number: -1 is
           what a program returns when it failed, and it should read that way. */
        print("Exit code ");
        if (exit_code < 0) {
            put('-');
            print_dec((boot_uint64_t)(-(long)exit_code));
        } else {
            print_dec((boot_uint64_t)exit_code);
        }
        print("\n");
    }
    return 1;
}

/* "Z:" on its own switches drives, exactly as it always did. */
static int try_drive_change(const char* input) {
    VOLUME* volume;

    if (!input[0] || input[1] != ':' || (input[2] && !is_space(input[2])))
        return 0;
    volume = volume_by_letter(input[0]);
    if (!volume) {
        print_line("Invalid drive.");
        return 1;
    }
    current_volume = volume;
    current_path[0] = '\\';
    current_path[1] = 0;
    return 1;
}

static void execute(const char* input);

/* Run a batch file: one line, one command.
 *
 * Nesting is refused rather than supported. A batch file that calls itself
 * would recurse on the kernel stack with nothing to stop it, and DOS needed
 * CALL for the same reason.
 *
 * `@` at the start of a line suppresses the echo, `rem` and `:` are comments
 * and labels - the three pieces of syntax worth having before variables and
 * control flow exist. */
static int batch_depth;

static void run_batch(VOLUME* volume, const char* path) {
    FAT_ENTRY entry;
    char* contents;
    char line[INPUT_MAX];
    boot_uint32_t offset = 0;

    if (batch_depth) { print_line("Batch files cannot be nested."); return; }
    if (!fat32_stat(volume, path, &entry)) return;
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return;
    if (!entry.size) return;

    contents = (char*)kmalloc(entry.size + 1);
    if (!contents) { print_line("Out of memory."); return; }
    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset,
                                       contents + offset, entry.size - offset);
        if (!got) break;
        offset += got;
    }
    contents[offset] = 0;

    batch_depth++;
    for (boot_uint32_t index = 0; index < offset;) {
        boot_uint64_t length = 0;
        int quiet = 0;
        const char* command;

        while (index < offset && contents[index] != '\n' &&
               length + 1 < INPUT_MAX) {
            char character = contents[index++];
            if (character != '\r') line[length++] = character;
        }
        while (index < offset && contents[index] != '\n') index++;
        if (index < offset) index++;
        line[length] = 0;

        command = skip_spaces(line);
        if (*command == '@') { quiet = 1; command = skip_spaces(command + 1); }
        if (!*command || *command == ':') continue;
        if (word_is(command, "REM")) continue;

        if (!quiet) {
            print_prompt();
            print_line(command);
        }
        execute(command);
    }
    batch_depth--;
    kfree(contents);
}

static void execute(const char* input) {
    ARGUMENTS arguments;

    input = skip_spaces(input);
    if (try_drive_change(input)) return;
    parse_arguments(input, &arguments);

    if (word_is(input, "CLS")) { console_clear(); return; }
    if (word_is(input, "DIR")) { command_dir(&arguments); return; }
    if (word_is(input, "CD") || word_is(input, "CHDIR")) {
        command_cd(&arguments);
        return;
    }
    if (word_is(input, "TYPE")) { command_type(&arguments); return; }
    if (word_is(input, "VOL")) { command_vol(); return; }
    if (word_is(input, "MEM")) { command_mem(); return; }
    if (word_is(input, "PCI")) { command_pci(); return; }
    if (word_is(input, "DISK")) { command_disk(); return; }
    if (word_is(input, "FORMAT")) { command_format(&arguments); return; }
    if (word_is(input, "PART")) { command_part(&arguments); return; }
    if (word_is(input, "SETUP")) { command_setup(); return; }
    if (word_is(input, "SHUTDOWN")) { command_shutdown(); return; }
    if (word_is(input, "REBOOT")) { command_reboot(); return; }
    if (word_is(input, "DATE")) { command_date(); return; }
    if (word_is(input, "TIME")) { command_time(); return; }
    if (word_is(input, "VER")) { command_ver(); return; }
    if (word_is(input, "HELP")) { command_help(); return; }
    /* `echo.` prints a blank line - the DOS idiom for one, since a bare
       `echo` prints the on/off state instead. */
    if (word_is(input, "ECHO.")) { print_line(""); return; }
    if (word_is(input, "ECHO")) { print_line(arguments.tail); return; }
    if (word_is(input, "MD") || word_is(input, "MKDIR")) {
        command_mkdir(&arguments);
        return;
    }
    if (word_is(input, "RD") || word_is(input, "RMDIR")) {
        command_rmdir(&arguments);
        return;
    }
    if (word_is(input, "DEL") || word_is(input, "ERASE")) {
        command_del(&arguments);
        return;
    }
    if (word_is(input, "REN") || word_is(input, "RENAME")) {
        command_ren(&arguments);
        return;
    }
    if (word_is(input, "COPY")) { command_copy(&arguments); return; }
    if (word_is(input, "MORE")) { command_more(&arguments); return; }
    if (word_is(input, "TREE")) { command_tree(&arguments); return; }
    if (word_is(input, "ATTRIB")) { command_attrib(&arguments); return; }

    if (try_program(input, &arguments)) return;

    console_set_color(console_theme()->error, console_theme()->background);
    print("Bad command or file name: ");
    print_line(input);
    console_use_theme();
}

__attribute__((noreturn)) void command_run(void) {
    static char input[INPUT_MAX];

    current_volume = volume_boot();
    console_use_theme();
    command_ver();
    print("\n");

    /* AUTOEXEC.BAT at the root of the boot drive, if there is one. It runs
       before the check below, because on a machine with no keyboard whatever
       it prints is the only thing the system will ever say. */
    if (current_volume) {
        FAT_ENTRY entry;
        if (fat32_stat(current_volume, "\\AUTOEXEC.BAT", &entry))
            run_batch(current_volume, "\\AUTOEXEC.BAT");
    }

    /* Refusing to start the loop is the whole point.
     *
     * `keyboard_read_line` cannot distinguish "the user pressed Enter on an
     * empty line" from "there is no way to read a key", so with no input
     * device it returns an empty line immediately and the loop below prints
     * the prompt again - forever, as fast as the console can draw. Seen on
     * real hardware, and it presents as nonsense rather than as a failure.
     * Same principle as a missing boot volume: say it once, loudly. */
    if (!keyboard_available()) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("");
        print_line("No keyboard found - neither PS/2 nor USB.");
        print_line("Nothing can be typed, so the shell is not starting.");
        console_use_theme();
        for (;;) __asm__ volatile ("hlt");
    }

    for (;;) {
        print_prompt();
        keyboard_read_line(input, sizeof(input));
        /* keyboard_read_line echoes to the screen only, so the line is
           repeated here for the serial log - without the prompt, which
           print_prompt() has already written to both. */
        serial_write(input);
        serial_write("\n");
        if (input[0]) execute(input);
    }
}
