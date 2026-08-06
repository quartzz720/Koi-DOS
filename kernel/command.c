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
#include "fat32.h"
#include "partition.h"
#include "rtc.h"
#include "program.h"
#include "syscall.h"
#include "build.h"
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

/* Build a path to a program: either in the current directory or at the root
   of the current drive. */
static void build_program_path(const char* name, char* result, int at_root) {
    boot_uint64_t length;

    if (at_root) {
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
    char* buffer;
    boot_uint32_t offset = 0;
    int ok = 1;

    if (!fat32_stat(from_volume, from, &source)) return 0;
    if (source.attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;
    if (!fat32_create(to_volume, to, 0, &destination)) return 0;

    buffer = (char*)kmalloc(4096);
    if (!buffer) return 0;

    while (offset < source.size) {
        boot_uint32_t got = fat32_read(from_volume, &source, offset, buffer, 4096);
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

    /* Current directory first, then the root. */
    build_program_path(name, path, 0);
    if (!fat32_stat(current_volume, path, &entry)) {
        build_program_path(name, path, 1);
        if (!fat32_stat(current_volume, path, &entry)) {
            /* Nothing by that name as a program; try it as a batch file. */
            if (has_extension) return 0;
            name[length - 4] = 0;
            {
                const char* batch = ".BAT";
                for (int index = 0; index < 4; index++) name[length - 4 + index] = batch[index];
            }
            build_program_path(name, path, 0);
            if (!fat32_stat(current_volume, path, &entry)) {
                build_program_path(name, path, 1);
                if (!fat32_stat(current_volume, path, &entry)) return 0;
            }
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

    syscall_set_volume(current_volume);
    code = program_run(current_volume, path, arguments->tail);
    syscall_close_all();

    if (code < 0) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("Not a valid Koi-DOS program.");
        console_use_theme();
    } else if (code != 0) {
        /* DOS reported a non-zero exit only through ERRORLEVEL in batch files;
           printing it is more use at an interactive prompt. */
        print("Exit code ");
        print_dec((boot_uint64_t)code);
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

    /* AUTOEXEC.BAT at the root of the boot drive, if there is one. */
    if (current_volume) {
        FAT_ENTRY entry;
        if (fat32_stat(current_volume, "\\AUTOEXEC.BAT", &entry))
            run_batch(current_volume, "\\AUTOEXEC.BAT");
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
