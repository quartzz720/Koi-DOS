#include "command.h"
#include "console.h"
#include "layout.h"
#include "keyboard.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "heap.h"
#include "paging.h"
#include "pci.h"
#include "block.h"
#include "xhci.h"
#include "audio.h"
#include "hda.h"
#include "graphics.h"
#include "net.h"
#include "e1000.h"
#include "tftp.h"
#include "mouse.h"

#include "config.h"
#include "environment.h"
#include "timer.h"
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

    /* A name that starts at the root is already a path, and searching for it
       three times over would produce `\BIN\\MIZU\MIZU.EXE`. This is how an
       installed package is run: dosget gives every package a directory of its
       own rather than emptying it into \BIN, so `\MIZU\MIZU` is the ordinary
       way to start one and has to work from anywhere. */
    if (name[0] == '\\') {
        length = 0;
        while (name[length] && length + 1 < PATH_MAX) {
            result[length] = name[length];
            length++;
        }
        result[length] = 0;
        return;
    }

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

static int build_program_path_from_prefix(const char* prefix, const char* name,
                                          VOLUME** volume, char* result) {
    boot_uint64_t length = 0;
    const char* cursor = prefix;

    *volume = volume_boot();
    if (!*volume) return 0;

    if (cursor[0] && cursor[1] == ':') {
        VOLUME* named = volume_by_letter(cursor[0]);
        if (!named) return 0;
        *volume = named;
        cursor += 2;
    }

    if (cursor[0] != '\\') result[length++] = '\\';
    while (cursor[0] && length + 1 < PATH_MAX) result[length++] = *cursor++;
    if (length && result[length - 1] != '\\') result[length++] = '\\';
    for (boot_uint64_t index = 0; name[index] && length + 1 < PATH_MAX; index++)
        result[length++] = name[index];
    result[length] = 0;
    return 1;
}

static int search_configured_program_path(const char* name, VOLUME** volume,
                                          char* path, FAT_ENTRY* entry) {
    const char* configured = config_program_path();
    char prefix[PATH_MAX];

    if (!configured || !configured[0]) return 0;

    for (const char* cursor = configured; *cursor; ) {
        boot_uint64_t length = 0;

        while (*cursor == ';' || is_space(*cursor)) cursor++;
        while (*cursor && *cursor != ';' && length + 1 < PATH_MAX)
            prefix[length++] = *cursor++;
        while (length && is_space(prefix[length - 1])) length--;
        prefix[length] = 0;
        if (prefix[0] && build_program_path_from_prefix(prefix, name, volume, path) &&
            fat32_stat(*volume, path, entry)) {
            return 1;
        }
        while (*cursor && *cursor != ';') cursor++;
        if (*cursor == ';') cursor++;
    }
    return 0;
}

static int find_program(const char* name, VOLUME** volume, char* path,
                        FAT_ENTRY* entry) {
    int found = 0;

    if (!current_volume) return 0;
    *volume = current_volume;

    /* A name that says which drive it is on is a location, not something to
     * search for.
     *
     * `\MIZU\MIZU` already worked, because a leading backslash is handled
     * below; `Z:\MIZU\MIZU` did not, and neither did anything else with a
     * drive letter in front of it. Nothing complained in an obvious way - the
     * search simply looked for a file whose name contained a colon in four
     * directories that could not have one, and reported that there was no such
     * command. CALL found it: writing out the full path to another batch file
     * is the ordinary way to write one. */
    for (boot_uint64_t index = 0; name[index]; index++) {
        if (name[index] != ':') continue;
        if (!resolve_path(name, volume, path)) return 0;
        return fat32_stat(*volume, path, entry);
    }

    /* The current directory first, then the root of the current drive. */
    for (int place = PROGRAM_SEARCH_CURRENT; place <= PROGRAM_SEARCH_ROOT && !found;
         place++) {
        build_program_path(name, path, place);
        found = fat32_stat(current_volume, path, entry);
    }

    /* Then the user-configured search path, which defaults to the package dirs
       that carry Commander and Mizu. */
    if (!found) found = search_configured_program_path(name, volume, path, entry);

    /* Then \\BIN on the system volume, where the built-in tools live. */
    if (!found) {
        VOLUME* system = volume_boot();
        if (system) {
            build_program_path(name, path, PROGRAM_SEARCH_BIN);
            if (fat32_stat(system, path, entry)) {
                found = 1;
                *volume = system;
            }
        }
    }

    return found;
}

/* Just the numbers, for the prompt. The commands below say "Current time is"
   around them, which is right there and wrong in a prompt. */
static void print_time_of_day(void) {
    RTC_TIME now;
    rtc_read(&now);
    print_two_digits(now.hour);
    put(':');
    print_two_digits(now.minute);
}

static void print_date_of_today(void) {
    RTC_TIME now;
    rtc_read(&now);
    print_dec(now.year);
    put('-');
    print_two_digits(now.month);
    put('-');
    print_two_digits(now.day);
}

/* The prompt, which is `$P$G` unless somebody has said otherwise.
 *
 * The codes are DOS's, and the reason to use theirs rather than invent any is
 * that people already have `SET PROMPT=$P$G` in their fingers. Anything after
 * a `$` that is not a code prints as itself, so a prompt with a stray dollar
 * in it is odd-looking rather than swallowed. */
static void print_prompt(void) {
    const char* format = environment_get("PROMPT");

    console_set_color(console_theme()->prompt, console_theme()->background);
    if (!format || !format[0]) format = "$P$G";

    while (*format) {
        char code;

        if (*format != '$') { put(*format++); continue; }
        if (!format[1]) { put('$'); break; }
        code = format[1];
        format += 2;
        if (code >= 'a' && code <= 'z') code = (char)(code - 32);
        switch (code) {
        case 'P':                                   /* drive and directory */
            if (current_volume) put(current_volume->letter);
            else put('?');
            put(':');
            print(current_path);
            break;
        case 'N':                                   /* the drive alone */
            if (current_volume) put(current_volume->letter);
            else put('?');
            break;
        case 'G': put('>'); break;
        case 'L': put('<'); break;
        case 'B': put('|'); break;
        case 'Q': put('='); break;
        case 'S': put(' '); break;
        case '$': put('$'); break;
        case '_': put('\n'); break;
        case 'T': print_time_of_day(); break;
        case 'D': print_date_of_today(); break;
        default:
            /* Not a code. Both characters, as they were typed. */
            put('$');
            put(format[-1]);
            break;
        }
    }
    print(" ");
    console_use_theme();
}

/* The build stamp is worth more than the version during development: two
   kernels claiming 0.51 differ by whatever happened between them, and the
   number is the only way to tell from the screen which one is running. It is
   the commit count, so it only moves when history does; a trailing `+` on the
   hash means the tree had uncommitted changes when this was built. */
/* Who and what this is made of.
 *
 * An operating system that cannot say where it came from is a strange object.
 * The licences are here because they have to be somewhere a person can reach
 * without a browser, and the last line is here because it is the truest thing
 * on the list.
 *
 * The name at the end is a person, not a joke. Typing it on its own prints the
 * dedication by itself - which is the only kind of memorial a program can
 * really offer: it waits, and it is there when somebody looks. */
static void dedication(void) {
    console_set_color(console_theme()->prompt, console_theme()->background);
    print_line("");
    print_line("  Built through intense pain and heartbreak.");
    print_line("  I miss you so much... Rest in peace, @nonconformie.");
    print_line("");
    console_use_theme();
}

static void command_credits(void) {
    print_line("");
    print("Koi-DOS ");
    print(KOI_VERSION_NAME);
    print_line(" - a DOS-like operating system for UEFI machines.");
    print_line("");
    print_line("Written by Koi Ayame (quartzz720), in freestanding C, with no");
    print_line("standard library and nothing borrowed but the font.");
    print_line("");
    print_line("  Koi-DOS        MIT");
    print_line("  Terminus Font  SIL Open Font License 1.1");
    print_line("  K-DOOM         GPL-2.0, from id Software's release of DOOM");
    print_line("");
    print_line("The full text is in \\LICENSE on this volume.");
    dedication();
}

static void command_ver(void) {
    print("Koi-DOS ");
    print_line(KOI_VERSION_NAME);

    print("Kernel ");
    print_dec(KOI_DOS_VERSION >> 8);
    put('.');
    print_dec(KOI_DOS_VERSION & 0xFF);
    put('.');
    print_dec(KOI_BUILD_NUMBER);
    /* The build id, not just the commit: work between two commits all carries
       the same commit, so without this a machine running an hour-old kernel
       says exactly what a machine running the newest one says - and "did the
       update land" stops being answerable. */
    print(", built " KOI_BUILD_DATE " (" KOI_BUILD_COMMIT ", build "
          KOI_BUILD_ID ")");
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
    print_line("set [n=v]      environment variables; PATH and PROMPT live here");
    print_line("credits        what this is made of, and who it is for");
    print_line("find sort      filter text; both read a pipe when given no file");
    print_line("xcopy          a directory and everything under it");
    print_line("label mode     what a volume is called, and how the console is set");
    print_line("");
    print_line("  cmd > file   send the output there      cmd >> file  add to it");
    print_line("  cmd < file   read from there            a | b        b reads a's output");
    print_line("pci            every function on the PCI bus");
    print_line("disk           disks and partitions, letters or not");
    print_line("chkdsk [d:] [/F]  check a volume, /F to repair what it finds");
    print_line("format <part>  make a new filesystem - destroys everything on it");
    print_line("part <disk>    replace the partition table - destroys the whole disk");
    print_line("setup          install Koi-DOS onto a disk");
    print_line("shutdown       turn the machine off");
    print_line("reboot         restart the machine");
    print_line("date           show the date");
    print_line("time           show the time");
    print_line("echo [text]    print text");
    print_line("beep [hz] [ms] a tone, if there is a sound device");
    print_line("sound [node]   the sound device; a node sends output there by hand");
    print_line("sound volume <n>  loudness, 0 to 100");
    print_line("layout [en|ru|uk|el]  keyboard layout; Alt+Shift switches");
    print_line("net [start]    the network: what it is, or ask for an address");
    print_line("net set <..>   set an address by hand, for a wire with no server");
    print_line("net usb        test the USB network device on its own");
    print_line("ping <host>    is it there, and how far away");
    print_line("dosget <..>    packages: list, install, update, remove");
    print_line("pointer        move the pointer about; hold the button to draw");
    print_line("log [file]     the kernel log: on screen, or written to a file");
    print_line("log net <addr> send the kernel log to another machine over UDP");
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
    {
        /* Counted by walking rather than from block_device_count(), which
           still includes the slots of devices that have been unplugged. */
        boot_uint32_t present = 0;
        for (boot_uint32_t index = 0; index < block_device_count(); index++)
            if (block_device(index)) present++;
        print_dec(present);
        {
            int first = 1;
            for (boot_uint32_t index = 0; index < block_device_count(); index++) {
                BLOCK_DEVICE* device = block_device(index);
                if (!device) continue;
                print(first ? "  " : ", ");
                print(device->name);
                first = 0;
            }
        }
    }
    print_line("");

    /* Only the ones with a letter, and a count of the ones without.
     *
     * This printed every volume's letter without asking whether it had one.
     * The loader's partition deliberately has none, so its letter is a zero
     * byte - and a zero byte was invisible in the font this was written
     * against and is a hollow rectangle in Terminus. The bug did not arrive
     * with the new font; it arrived with the old one and hid there. */
    {
        boot_uint32_t lettered = 0;
        boot_uint32_t hidden = 0;

        for (boot_uint32_t index = 0; index < volume_count(); index++) {
            VOLUME* volume = volume_at(index);
            if (volume && volume->letter) lettered++;
            else hidden++;
        }

        print("Volumes         : ");
        print_dec(lettered);
        for (boot_uint32_t index = 0; index < volume_count(); index++) {
            VOLUME* volume = volume_at(index);
            if (!volume || !volume->letter) continue;
            print("  ");
            put(volume->letter);
            put(':');
        }
        if (hidden) {
            print("  (");
            print_dec(hidden);
            print(hidden == 1 ? " without a letter)" : " without letters)");
        }
        print_line("");
    }

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

    print("Audio           : ");
    if (audio_ready()) {
        print(audio_device_name());
        print_line(" codec, 48 kHz stereo");
    } else {
        print_line("no device");
    }
}

/* A plain decimal number, or `fallback` when the text is not one. Small and
   local because the shell has no general number parsing and one command
   asking for two integers does not justify inventing it. */
static boot_uint32_t decimal(const char* text, boot_uint32_t fallback) {
    boot_uint32_t value = 0;
    int digits = 0;

    if (!text) return fallback;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (boot_uint32_t)(*text++ - '0');
        if (value > 1000000U) return fallback;
        digits++;
    }
    if (!digits || *text) return fallback;
    return value;
}

static void command_beep(const ARGUMENTS* arguments) {
    boot_uint32_t hertz = 880;
    boot_uint32_t milliseconds = 200;

    if (!audio_ready()) {
        print_line("No sound device.");
        return;
    }
    /* 880 Hz for a fifth of a second by default - close enough to the note a
       PC speaker made that it is recognisable, and short enough not to be
       tiresome when it is the thing being tested. */
    if (arguments->operand_count > 0)
        hertz = decimal(arguments->operand[0], 880);
    if (arguments->operand_count > 1)
        milliseconds = decimal(arguments->operand[1], 200);

    if (hertz < 20 || hertz > 20000) {
        print_line("Usage: beep [hertz 20-20000] [milliseconds]");
        return;
    }
    if (milliseconds > 10000) milliseconds = 10000;

    if (audio_tone(hertz, milliseconds, 180) < 0)
        print_line("Every voice is busy.");
}

static void print_hex(boot_uint64_t value, int digits) {
    const char* digit = "0123456789ABCDEF";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        put(digit[(value >> shift) & 0xF]);
}

/* What the sound hardware is, and what it decided.
 *
 * "There is no sound" has several completely different causes that are
 * identical from a chair: no controller, a controller with no codec, a codec
 * whose only outputs are digital, a headphone socket with nothing plugged into
 * it, or the right socket picked and muted further along. Printing the codec's
 * own description of its outputs is the difference between guessing and
 * knowing which of those it is. */
/* The kernel's own log, which on most machines has nowhere else to go.
 *
 * Every driver says what it found and why it gave up over COM1, and no machine
 * made this century has a COM1. The text is kept in memory as well, so this
 * prints it - or writes it to a file, which is the only way to get a boot log
 * off a laptop and onto something that can read it. */
/* Ship the log to another machine, in pieces small enough to cross an
 * Ethernet frame.
 *
 * No sequence numbers and no acknowledgements: this is UDP on a local network,
 * where a lost datagram is rare, and the alternative is TCP, which this system
 * does not have and does not need in order to hand over a text file. What it
 * does have is a receiver as simple as `nc -lu -p 5555 > koi.log`, and that is
 * the whole point - the machine being debugged should not require software on
 * the other end.
 *
 * A pause between pieces because there is none of the pacing a real stack has,
 * and sixty datagrams arriving back to back is how a receiver drops some. */
#define LOG_CHUNK 1400

static void send_log(const char* address_text, const char* port_text) {
    const char* text = boot_log();
    boot_uint32_t length = boot_log_length();
    boot_uint32_t address = 0;
    boot_uint32_t port = 5555;
    boot_uint32_t at = 0;
    boot_uint32_t pieces = 0;

    if (!net_configured()) {
        print_line("No address yet. Run `net start` first.");
        return;
    }
    if (!net_parse_address(address_text, &address)) {
        print("Not an address: ");
        print_line(address_text);
        return;
    }
    if (port_text && port_text[0]) {
        port = 0;
        for (const char* digit = port_text; *digit; digit++) {
            if (*digit < '0' || *digit > '9') { port = 0; break; }
            port = port * 10 + (boot_uint32_t)(*digit - '0');
        }
        if (!port || port > 65535) {
            print_line("Not a port.");
            return;
        }
    }

    while (at < length) {
        boot_uint32_t chunk = length - at;

        if (chunk > LOG_CHUNK) chunk = LOG_CHUNK;
        if (!net_send_to(address, (boot_uint16_t)port, text + at, chunk)) {
            print_line("The log could not be sent.");
            return;
        }
        at += chunk;
        pieces++;
        timer_wait(5);
    }

    print_dec(length);
    print(" bytes sent to ");
    print(address_text);
    print(" in ");
    print_dec(pieces);
    print_line(pieces == 1 ? " datagram" : " datagrams");
    print_line("Receive it with: nc -lu -p 5555 > koi.log");
}

static void command_log(const ARGUMENTS* arguments) {
    const char* text = boot_log();
    boot_uint32_t length = boot_log_length();
    char name[PATH_MAX];

    /* `log net <address> [port]` before anything else, because otherwise the
       word "net" is a perfectly good filename. */
    if (word_is(arguments->operand[0], "NET")) {
        send_log(arguments->operand[1], arguments->operand[2]);
        return;
    }

    single_operand(arguments, name);

    if (name[0]) {
        char path[PATH_MAX];
        VOLUME* volume;
        FAT_ENTRY entry;
        boot_uint32_t written;

        if (!current_volume) { print_line("No volume."); return; }
        if (!resolve_path(name, &volume, path)) {
            print_line("Invalid drive.");
            return;
        }
        /* Replacing an older log rather than refusing: this gets written
           again after every change, and a command that fails the second time
           is a command nobody uses. */
        if (fat32_stat(volume, path, &entry)) fat32_remove(volume, path);
        if (!fat32_create(volume, path, 0, &entry)) {
            print_line("Unable to create the file.");
            return;
        }
        written = fat32_write(volume, &entry, 0, text, length);
        print_dec(written);
        print(" bytes written to ");
        print_line(name);
        if (written != length) print_line("The file is short - the disk is full.");
        return;
    }

    {
        boot_uint32_t lines = 0;
        boot_uint32_t page = console_rows() - 1;

        for (boot_uint32_t index = 0; index < length; index++) {
            char character = text[index];
            if (character == '\r') continue;
            put(character);
            if (character != '\n') continue;
            if (++lines < page) continue;
            console_set_color(console_theme()->background,
                              console_theme()->foreground);
            print("-- More --");
            console_use_theme();
            if (keyboard_getchar() == 27) return;
            print("\n");
            lines = 0;
        }
    }
    if (boot_log_truncated())
        print_line("(the log filled up; everything after this was dropped)");
}

/* Print one address as a dotted quad, or a dash when there is not one. */
static void print_address(boot_uint32_t address) {
    char text[16];

    if (!address) { print("-"); return; }
    net_format_address(address, text);
    print(text);
}

static void print_traffic(void);

/* Hexadecimal, for the register dump below: these are bit fields, and a
   decimal bit field is a puzzle. */
static void print_card_number(boot_uint64_t value) {
    print_hex((boot_uint32_t)value, 8);
}

static void command_net(void) {
    if (!net_link_ready()) {
        print_line("No network device.");
        print_line("");
        print_line("Koi-DOS carries frames over a USB adapter speaking RNDIS, which");
        print_line("is what a phone offers when it shares its connection. Plug one");
        print_line("in and turn USB tethering on; `log` says what was found and why");
        print_line("it was not taken.");
        return;
    }

    print("Carried over     : ");
    print_line(net_link_name());
    print("Hardware address: ");
    for (int index = 0; index < 6; index++) {
        if (index) print(":");
        print_hex(net_hardware_address()[index], 2);
    }
    print_line("");

    if (!net_configured()) {
        print_line("Address         : none - nobody has handed one out yet");
        print_line("");
        print_line("`net start` asks for one.");
        return;
    }

    print("Address         : "); print_address(net_address()); print_line("");
    print("Netmask         : "); print_address(net_netmask()); print_line("");
    print("Gateway         : "); print_address(net_gateway()); print_line("");
    print("Name server     : "); print_address(net_dns()); print_line("");
    print_traffic();
    if (e1000_ready()) {
        print("Card registers  : ");
        e1000_diagnose(print, print_card_number);
        print_line("");
    }
}

/* What has actually moved. Printed whenever the network is discussed, because
   the interesting case is the one where the answer is zero. */
static void print_traffic(void) {
    boot_uint32_t sent = 0;
    boot_uint32_t received = 0;
    boot_uint32_t failed = 0;

    net_traffic(&sent, &received, &failed);
    print("Frames sent     : ");
    print_dec(sent);
    if (failed) {
        print(" (");
        print_dec(failed);
        print(" lost)");
    }
    print_line("");
    print("Frames received : ");
    print_dec(received);
    print_line("");
}

static void command_net_start(void) {
    if (!net_link_ready()) { print_line("No network device."); return; }
    print_line("Asking for an address...");
    if (!net_start()) {
        print_line("Nobody answered.");
        print_traffic();
        print_line("");
        /* Which of these is true is the whole diagnosis, and the counters
           above say which without another boot. */
        print_line("Nothing sent    : the adapter is not taking frames from us.");
        print_line("Sent, none back : the phone is not bridging - turn tethering");
        print_line("                  off and on again, and check it stayed on.");
        print_line("Frames received : something answered, but not a DHCP server.");
        /* And what the hardware says, which is not the same as what the
           driver believes. Written to the log rather than the screen: it is
           for reading afterwards, next to everything else. */
        usb_net_diagnose();
        print_line("");
        print_line("The controller's own view of the endpoints is in the log.");
        return;
    }
    print("Address ");
    print_address(net_address());
    print(", gateway ");
    print_address(net_gateway());
    print_line("");
}

/* An address chosen by hand: `net set <address> <netmask> [gateway] [dns]`.
 *
 * For a network with nobody to ask - two machines and one cable between them,
 * which is the arrangement you reach for precisely when the ordinary one has
 * stopped working. */
static void command_net_set(const ARGUMENTS* arguments) {
    boot_uint32_t address = 0;
    boot_uint32_t netmask = 0;
    boot_uint32_t gateway = 0;
    boot_uint32_t dns = 0;

    if (!net_link_ready()) { print_line("No network device."); return; }
    if (!arguments->operand[1][0] || !arguments->operand[2][0]) {
        print_line("Usage: net set <address> <netmask> [gateway] [dns]");
        print_line("");
        print_line("For a cable straight between two machines:");
        print_line("  here      net set 192.168.50.2 255.255.255.0");
        print_line("  the other ip addr add 192.168.50.1/24 dev <interface>");
        return;
    }
    if (!net_parse_address(arguments->operand[1], &address) ||
        !net_parse_address(arguments->operand[2], &netmask)) {
        print_line("Not an address.");
        return;
    }
    if (arguments->operand[3][0] &&
        !net_parse_address(arguments->operand[3], &gateway)) {
        print_line("Not a gateway address.");
        return;
    }

    if (!net_configure(address, netmask, gateway, dns)) {
        print_line("The address could not be set.");
        return;
    }
    print("Address ");
    print_address(net_address());
    print_line(", set by hand");
}

/* Four echo requests, the way every ping since 1983 has done it. */
static void command_ping(const ARGUMENTS* arguments) {
    boot_uint32_t address;
    int replies = 0;

    if (!arguments->tail[0]) {
        print_line("Usage: ping <address or name>");
        return;
    }
    if (!net_configured()) {
        print_line("No address yet. Run `net start` first.");
        return;
    }
    if (!net_resolve(arguments->tail, &address)) {
        print("Could not find ");
        print_line(arguments->tail);
        return;
    }

    print("Pinging ");
    print(arguments->tail);
    print(" [");
    print_address(address);
    print_line("] with 32 bytes of data:");

    for (int attempt = 0; attempt < 4; attempt++) {
        int milliseconds = net_ping(address, 2000);

        if (milliseconds < 0) {
            print_line("Request timed out.");
        } else {
            replies++;
            print("Reply from ");
            print_address(address);
            print(": time=");
            print_dec((boot_uint64_t)milliseconds);
            print_line("ms");
        }
        if (attempt < 3) timer_wait(500);
    }

    print("");
    print_dec((boot_uint64_t)replies);
    print(" of 4 replied");
    print_line(replies ? "." : " - nothing came back.");
}

static void command_sound(const ARGUMENTS* arguments) {
    if (!audio_ready()) {
        print("No sound device: ");
        print_line(audio_failure());
        print_line("");
        print_line("Koi-DOS drives Intel HD Audio, which is the sound hardware in");
        print_line("every machine since about 2005 - not the PC speaker, which this");
        print_line("does not use at all. `pci` will show whether a controller is");
        print_line("there: class 04:03 is HD Audio.");
        return;
    }

    /* `sound <node>` sends the output somewhere else by hand.
     *
     * A codec that cannot tell whether anything is in its headphone socket
     * leaves one question that nothing else here can answer: is the jack not
     * being sensed, or not being driven? Choosing it deliberately and
     * listening separates those two, and there is no other way to. */
    /* `sound volume <0-100>`. A percentage rather than the 0-255 the mixer
       works in, because nobody thinks about loudness in eighth-bits - and the
       first thing anybody wants after hearing this machine through headphones
       is to turn it down. */
    if (arguments->operand_count && word_is(arguments->operand[0], "VOLUME")) {
        const char* text = arguments->operand[1];
        boot_uint64_t percent = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            percent = percent * 10 + (boot_uint64_t)(*text++ - '0');
            digits++;
        }
        if (!digits || *text || percent > 100) {
            print("Volume is ");
            print_dec((boot_uint64_t)((audio_volume() * 100 + 127) / 255));
            print_line(" percent.");
            print_line("`sound volume <0-100>` changes it.");
            return;
        }
        audio_set_volume((int)((percent * 255 + 50) / 100));
        print("Volume ");
        print_dec(percent);
        print_line(" percent.");
        return;
    }

    if (arguments->operand_count) {
        boot_uint64_t node = 0;
        const char* text = arguments->operand[0];
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            node = node * 10 + (boot_uint64_t)(*text++ - '0');
            digits++;
        }
        if (!digits || *text || node > 255) {
            print_line("sound [node]");
            print_line("sound volume [0-100]");
            print_line("");
            print_line("With no argument, what the sound hardware is doing.");
            print_line("With a node number from the list below, sends the");
            print_line("sound to that output instead.");
            return;
        }
        if (hda_select((boot_uint8_t)node)) {
            print("Output moved to node ");
            print_dec(node);
            print_line(". Play something to hear whether it worked.");
        } else {
            print("There is no output at node ");
            print_dec(node);
            print_line(", or no converter reaches it.");
        }
        print_line("");
    }

    /* Measured now rather than remembered from startup, so a socket can be
       tested by plugging something into it rather than by rebooting. */
    hda_rescan();

    print("Codec           : ");
    print(hda_codec_name());
    print(" (");
    print_hex(hda_codec_id(), 8);
    print_line(")");
    print("Output          : ");
    print(hda_output_name());
    print(", converter at node ");
    print_dec(hda_converter());
    print_line("");
    print_field("Rate            : ", HDA_RATE, " Hz, stereo, 16-bit");
    print("Volume          : ");
    print_dec((boot_uint64_t)((audio_volume() * 100 + 127) / 255));
    print_line(" percent  (`sound volume <0-100>` changes it)");
    print("Voices          : ");
    print_dec(audio_voices_playing());
    print(" of ");
    print_dec(AUDIO_VOICES);
    print_line(" playing");
    print_line("");

    print_line("Analogue outputs the codec describes:");
    for (boot_uint32_t index = 0; index < hda_pin_count(); index++) {
        const HDA_PIN* pin = hda_pin(index);
        if (!pin) continue;

        print("  node ");
        print_dec(pin->node);
        print("  ");
        switch (pin->device) {
        case HDA_DEVICE_LINE_OUT: print("line out   "); break;
        case HDA_DEVICE_SPEAKER: print("speaker    "); break;
        case HDA_DEVICE_HEADPHONE: print("headphones "); break;
        default: print("other      "); break;
        }
        /* A pin the board never wired to a socket is not an empty socket, and
           calling it one sends somebody looking for a jack that is not on the
           machine. Two "headphones" in a list where the laptop has one is
           exactly that. */
        if (pin->connectivity == 1) {
            print("not connected          ");
        } else switch (pin->sense) {
        case HDA_SENSE_PRESENT: print("something is plugged in"); break;
        case HDA_SENSE_EMPTY: print("nothing plugged in     "); break;
        case HDA_SENSE_FIXED: print("built in               "); break;
        default: print("cannot tell            "); break;
        }
        if (pin->chosen) print("  <- in use");
        print_line("");
    }
    if (!hda_pin_count()) {
        print_line("  none - every output on this codec is digital");
        return;
    }
    /* Something is plugged into a socket that is not the one playing.
     *
     * Said rather than acted on: moving the output because a register changed
     * would be this command doing something to the machine when it was asked
     * to describe it. The offer is one line and the decision stays with the
     * person wearing the headphones. */
    for (boot_uint32_t index = 0; index < hda_pin_count(); index++) {
        const HDA_PIN* pin = hda_pin(index);
        if (!pin || pin->chosen) continue;
        if (pin->sense != HDA_SENSE_PRESENT) continue;
        print_line("");
        print("Something is plugged into node ");
        print_dec(pin->node);
        print(" and the sound is not going there.");
        print_line("");
        print("Run `sound ");
        print_dec(pin->node);
        print_line("` to move it.");
        break;
    }

    print_line("");
    print_line("`sound <node>` sends the sound to one of these by hand - which is");
    print_line("how to tell a socket that is not sensed from one that is not");
    print_line("driven. The raw answers the codec gave are in the log.");
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
    boot_uint64_t kib;

    /* The guard comes first, and used to come one line later - after a
       division by the very thing it was guarding against. Nothing had ever
       reported a sector size of zero until a device could be unplugged. */
    if (!sector_size) { print("?"); return; }
    kib = sectors / (1024U / sector_size ? 1024U / sector_size : 1);
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
static void remount_everything(void);

/* The note the block layer rings when a disk appears or goes away. Registered
   once, at startup. */
static void disks_changed(void) {
    remount_everything();
}

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

    /* And which of them the system is installed on, which the scan cannot know
     * on its own: it is a file, so it can only be looked for once the volumes
     * are mounted.
     *
     * This was done at boot and nowhere else, because a rescan only ever
     * happened after `format` - where losing your place was expected. Wiring
     * it to a stick being plugged in made the omission visible immediately and
     * absurdly: plugging in a USB stick moved the system to Y: and handed Z:
     * to the loader's partition, which is the one volume that is meant to have
     * no letter at all. */
    {
        VOLUME* loader = volume_boot();
        for (boot_uint32_t index = 0; loader && index < volumes; index++) {
            VOLUME* candidate = volume_at(index);
            FAT_ENTRY entry;

            if (!candidate || candidate == loader) continue;
            if (candidate->device != loader->device) continue;
            if (!fat32_stat(candidate, SYSTEM_VOLUME_MARKER, &entry)) continue;
            partition_set_system_volume(candidate);
            break;
        }
    }

    current_volume = volume_boot();
    current_path[0] = '\\';
    current_path[1] = 0;
}



/* `pointer` - a screen, a cursor, and somewhere to move it.
 *
 * The smallest thing that proves a pointer works, and the first program in
 * this system that is driven by something other than a keyboard. It draws
 * where the pointer is, leaves a trail where the button was held, and says how
 * many packets have arrived - because a device that was found and reports
 * nothing looks exactly like a working one that nobody is touching.
 *
 * The bar down the right-hand side is the wheel, and on a laptop it is two
 * fingers on the touchpad. It is here for the same reason as the packet count:
 * a scroll that arrives and a scroll that does not are the same silence. */
static void command_pointer(void) {
    GRAPHICS_SCREEN screen;
    boot_uint32_t background;
    boot_uint32_t ink;
    boot_uint32_t trail;
    boot_uint32_t wheel_ink;
    int last_x;
    int last_y;
    int last_scroll;
    int started_at;
    int bar;
    int last_bar;
    int bar_x;

    if (!mouse_present()) {
        print_line("No pointer. This machine has no PS/2 mouse or touchpad,");
        print_line("or the firmware left its port switched off.");
        return;
    }
    if (!graphics_enter(&screen)) {
        print_line("The screen could not be taken.");
        return;
    }

    background = graphics_color(16, 24, 40);
    ink = graphics_color(230, 240, 255);
    trail = graphics_color(80, 160, 220);
    wheel_ink = graphics_color(240, 200, 90);
    graphics_clear(background);
    /* And shown. Clearing draws into the back buffer, and everything after
       this presents only the small rectangle around the pointer - so without
       this line the screen keeps the text that was there and the pointer
       arrives as grey squares appearing over it, which reads as a machine
       that has hung rather than one that is working. */
    graphics_present();
    mouse_place((int)screen.width / 2, (int)screen.height / 2);
    last_x = mouse_x();
    last_y = mouse_y();

    started_at = mouse_scroll();
    last_scroll = started_at;
    bar = (int)screen.height / 2;
    last_bar = bar;
    bar_x = (int)screen.width - 24;
    graphics_fill(bar_x, bar - 12, 12, 24, wheel_ink);
    graphics_present_rect(bar_x, 0, 12, (int)screen.height);

    for (;;) {
        int x = mouse_x();
        int y = mouse_y();
        int scroll = mouse_scroll();

        if (keyboard_pending() && keyboard_getchar() == 27) break;

        if (scroll != last_scroll) {
            /* Up on the wheel moves the marker up, which is the only mapping
               that will not be argued with. */
            bar -= (scroll - last_scroll) * 16;
            if (bar < 12) bar = 12;
            if (bar > (int)screen.height - 12) bar = (int)screen.height - 12;
            last_scroll = scroll;

            graphics_fill(bar_x, last_bar - 12, 12, 24, background);
            graphics_fill(bar_x, bar - 12, 12, 24, wheel_ink);
            graphics_present_rect(bar_x, 0, 12, (int)screen.height);
            last_bar = bar;
        }

        if (mouse_buttons() & 1) {
            /* Held down: draw. A line rather than a dot, because the pointer
               moves further between two looks than one pixel. */
            graphics_line(last_x, last_y, x, y, trail);
            graphics_present();
        } else if (x != last_x || y != last_y) {
            /* Erase the old cursor by redrawing the background under it, which
               is cheap and does not disturb anything drawn. */
            graphics_fill(last_x - 6, last_y - 6, 13, 13, background);
            graphics_present_rect(last_x - 7, last_y - 7, 15, 15);
        }

        if (x != last_x || y != last_y || (mouse_buttons() & 1)) {
            graphics_line(x - 6, y, x + 6, y, ink);
            graphics_line(x, y - 6, x, y + 6, ink);
            graphics_present_rect(x - 7, y - 7, 15, 15);
            last_x = x;
            last_y = y;
        }
    }

    graphics_leave();
    console_use_theme();
    print("Pointer at ");
    print_dec((boot_uint64_t)mouse_x());
    print(",");
    print_dec((boot_uint64_t)mouse_y());
    print(" after ");
    print_dec(mouse_movements());
    print_line(" packet(s).");

    if (!mouse_has_wheel()) {
        print_line("No wheel: this device answered the IntelliMouse sequence as");
        print_line("a plain three-button mouse, so there is no scrolling to have.");
    } else {
        int turned = mouse_scroll() - started_at;
        print("Wheel ");
        print_dec((boot_uint64_t)(turned < 0 ? -turned : turned));
        print(" notch(es) ");
        print_line(turned > 0 ? "up." : (turned < 0 ? "down." : "- not turned."));
    }
}

/* ---- dosget --------------------------------------------------------------
 *
 * A package manager, named the way winget is named and behaving the way dnf
 * behaves: install fetches and unpacks, update brings everything already here
 * up to date, list says what there is.
 *
 * The transport is deliberately the weakest part. It is TFTP today because
 * this system has UDP and not TCP, and the whole arrangement is built so that
 * changing it costs one line in a file: the source is read from
 * \BOOT\dosget.cfg on every invocation, never cached, never read at boot. An
 * installed system can be pointed somewhere else and use it immediately -
 * which is the difference between a package manager and a hardcoded address.
 *
 * A package goes into a directory of its own at the root, beside DOOM: a
 * program with data files should not scatter them into a shared BIN.
 */

/* Does a line begin with this key? Case-insensitive, because a configuration
   file is written by a person and people do not agree about capitals. */
/* Append a separator and a word to a path, and never past the end. Paths here
   are built out of three or four pieces and a separate length check at each
   join is how one of them eventually gets forgotten. */
static void string_join(char* into, boot_uint32_t size, const char* separator,
                        const char* word) {
    boot_uint32_t at = 0;

    while (into[at]) at++;
    while (*separator && at + 1 < size) into[at++] = *separator++;
    while (*word && at + 1 < size) into[at++] = *word++;
    into[at] = 0;
}

static int prefix_matches(const char* text, const char* key) {
    while (*key) {
        if (upper(*text) != upper(*key)) return 0;
        text++;
        key++;
    }
    return 1;
}

#define DOSGET_CONFIG "\\BOOT\\dosget.cfg"
/* Where packages come from when nobody has said otherwise.
 *
 * It used to be the cable this is developed on, which is the right answer for
 * exactly one machine and means everybody else has to create a file before
 * `dosget` does anything at all. The public server is the answer that works
 * without being told, and \BOOT\dosget.cfg still overrides it - read fresh on
 * every invocation, so somebody running their own server changes one line and
 * needs no reboot. */
#define DOSGET_DEFAULT_SOURCE "195.133.195.44"
#define DOSGET_BUFFER (4 * 1024 * 1024)
/* What is installed and at which version. One `NAME VERSION` per line, on the
   boot volume beside the loader, because a record of what the system is made
   of belongs with the system rather than inside one of its packages. */
#define DOSGET_DATABASE "\\BOOT\\DOSGET.DB"

/* And what each one put where: one file per package, named after it.
 *
 * The database above says a package is installed and at which version, which
 * is everything `update` needs and nothing `remove` does. Removing something
 * means knowing what was written, and the only moment anybody knows that is
 * while it is being written - guessing afterwards means either deleting a
 * directory whole, along with the WAD somebody put in it, or deleting nothing.
 *
 * Beside the system rather than inside the package, for the same reason as the
 * database: a record of what the system is made of should not disappear
 * together with the thing it describes. */
#define DOSGET_RECORDS "\\BOOT\\DOSGET"

/* Where packages come from, asked afresh every time.
 *
 * Read rather than remembered on purpose. A setting that is loaded at boot is
 * a setting that needs a reboot to change, and the machine this runs on is one
 * whose network arrangements change several times an afternoon. */
static int dosget_source(boot_uint32_t* address) {
    char text[64];
    FAT_ENTRY entry;
    boot_uint32_t got = 0;
    boot_uint32_t at = 0;

    if (current_volume && fat32_stat(current_volume, DOSGET_CONFIG, &entry)) {
        boot_uint8_t page[512];
        got = fat32_read(current_volume, &entry, 0, page, sizeof(page) - 1);
        if (got) {
            page[got] = 0;
            /* One `key = value` per line, the same dull format the rest of the
               system uses, and only one key understood so far. */
            for (boot_uint32_t index = 0; index + 6 < got; index++) {
                if (page[index] != 's' && page[index] != 'S') continue;
                if (!prefix_matches((const char*)(page + index), "source")) continue;
                index += 6;
                while (index < got && (page[index] == ' ' || page[index] == '=' ||
                                       page[index] == '\t')) index++;
                while (index < got && at + 1 < sizeof(text) &&
                       page[index] != '\r' && page[index] != '\n' &&
                       page[index] != ' ')
                    text[at++] = (char)page[index++];
                text[at] = 0;
                break;
            }
        }
    }

    if (!at) {
        /* No file, or nothing usable in it. The default is the cable, which is
           where this is developed and the one address that is always true of a
           machine being worked on. */
        const char* fallback = DOSGET_DEFAULT_SOURCE;
        while (*fallback) text[at++] = *fallback++;
        text[at] = 0;
    }
    return net_parse_address(text, address);
}

/* How a download looks while it is happening.
 *
 * A third of a megabyte over a phone tethered to a router is slow enough that
 * a still screen and a hung machine are the same picture, and the person
 * watching has no way to tell them apart. So: a bar, a percentage when the
 * server said how big the file is, and the rate - which answers the other
 * question, "is it slow or is it stuck".
 *
 * One line, rewritten in place with a carriage return, because a progress
 * report that scrolls is a progress report that buries what came before it. */
static boot_uint32_t fetch_total;
static boot_uint32_t fetch_started;
static boot_uint32_t fetch_painted;

static void fetch_total_is(boot_uint32_t total) { fetch_total = total; }

static void fetch_progress(boot_uint32_t received) {
    boot_uint32_t elapsed = (boot_uint32_t)(timer_ticks() - fetch_started);
    boot_uint32_t width = 32;
    boot_uint32_t filled;

    /* Twenty times a second is plenty for something a person is watching, and
       repainting per block would spend more time drawing than downloading. */
    if (elapsed - fetch_painted < 50 && received != fetch_total) return;
    fetch_painted = elapsed;

    put('\r');
    print("  [");
    filled = fetch_total ? (received * width) / fetch_total : 0;
    for (boot_uint32_t index = 0; index < width; index++)
        put(index < filled ? '=' : (fetch_total ? ' ' : '.'));
    print("] ");

    if (fetch_total) {
        print_dec((boot_uint64_t)received * 100 / fetch_total);
        print("%  ");
    }
    print_dec(received / 1024);
    print(" KiB");

    if (elapsed > 200) {
        print("  ");
        print_dec((boot_uint64_t)received * 1000 / elapsed / 1024);
        print(" KiB/s");
    }
    print("     ");
}

static void fetch_progress_begin(void) {
    fetch_total = 0;
    fetch_started = (boot_uint32_t)timer_ticks();
    fetch_painted = 0;
    tftp_progress(fetch_total_is, fetch_progress);
}

static void fetch_progress_end(void) {
    tftp_progress((void (*)(boot_uint32_t))0, (void (*)(boot_uint32_t))0);
    /* The bar is left on screen only when it means something: a file small
       enough to arrive in one breath does not need a line saying so. */
    if (fetch_painted) print_line("");
    else { put('\r'); print("                                                        "); put('\r'); }
}

/* Fetch one file from the source into `buffer`. */
static int dosget_fetch(boot_uint32_t source, const char* name, void* buffer,
                        boot_uint32_t size) {
    const char* why = (const char*)0;
    int got;

    fetch_progress_begin();
    got = tftp_fetch(source, name, buffer, size, &why);
    fetch_progress_end();

    if (got < 0) {
        print("  ");
        print(name);
        print(": ");
        print_line(why ? why : "could not be fetched");
        return -1;
    }
    return got;
}

/* Read one `key = value` line out of a manifest already in memory. Returns
   where the value starts, or NULL. */
static const char* manifest_value(const char* text, boot_uint32_t length,
                                  const char* key, boot_uint32_t* from) {
    boot_uint32_t at = from ? *from : 0;

    while (at < length) {
        boot_uint32_t start = at;

        while (at < length && text[at] != '\n') at++;
        if (prefix_matches(text + start, key)) {
            boot_uint32_t value = start + (boot_uint32_t)strlen(key);
            while (value < at && (text[value] == ' ' || text[value] == '=' ||
                                  text[value] == '\t')) value++;
            if (from) *from = at + 1;
            return text + value;
        }
        at++;
    }
    if (from) *from = length;
    return (const char*)0;
}

/* Copy a line's worth of text out, stopping at the end of the line. */
static void take_line(const char* from, char* into, boot_uint32_t size) {
    boot_uint32_t at = 0;

    while (from && from[at] && from[at] != '\r' && from[at] != '\n' &&
           at + 1 < size) {
        into[at] = from[at];
        at++;
    }
    into[at] = 0;
}



/* The version recorded for a package, or an empty string. */
static void dosget_installed(const char* package, char* into,
                             boot_uint32_t size) {
    FAT_ENTRY entry;
    char text[1024];
    boot_uint32_t got;
    boot_uint32_t at = 0;

    into[0] = 0;
    if (!current_volume) return;
    if (!fat32_stat(current_volume, DOSGET_DATABASE, &entry)) return;
    got = fat32_read(current_volume, &entry, 0, text, sizeof(text) - 1);
    text[got] = 0;

    while (at < got) {
        boot_uint32_t start = at;

        while (at < got && text[at] != '\n') at++;
        if (prefix_matches(text + start, package)) {
            boot_uint32_t value = start + (boot_uint32_t)strlen(package);

            if (text[value] == ' ' || text[value] == '\t') {
                while (text[value] == ' ' || text[value] == '\t') value++;
                take_line(text + value, into, size);
                return;
            }
        }
        at++;
    }
}

/* Write one down, replacing whatever was there for that name. A null version
   drops the line instead, which is what removing a package needs. */
static void dosget_record(const char* package, const char* version) {
    FAT_ENTRY entry;
    char text[1024];
    char rebuilt[1024];
    boot_uint32_t got = 0;
    boot_uint32_t at = 0;
    boot_uint32_t out = 0;

    if (!current_volume) return;
    if (fat32_stat(current_volume, DOSGET_DATABASE, &entry)) {
        got = fat32_read(current_volume, &entry, 0, text, sizeof(text) - 1);
        fat32_remove(current_volume, DOSGET_DATABASE);
    }
    text[got] = 0;

    /* Every line but this package's, then this package's, fresh. Rewritten
       whole rather than edited in place: the file is a kilobyte and the
       alternative is offsets. */
    while (at < got) {
        boot_uint32_t start = at;

        while (at < got && text[at] != '\n') at++;
        if (!prefix_matches(text + start, package)) {
            for (boot_uint32_t index = start; index < at && out + 2 < sizeof(rebuilt); index++)
                if (text[index] != '\r') rebuilt[out++] = text[index];
            if (out + 2 < sizeof(rebuilt)) rebuilt[out++] = '\r', rebuilt[out++] = '\n';
        }
        at++;
    }

    /* Terminated before each join, because that is where the join starts
       looking. Without it the first one scans uninitialised memory for a zero
       byte and appends past whatever it finds - which put three bytes of
       rubbish in front of every name in this file, and made a package that was
       plainly installed look like one that was not. */
    rebuilt[out] = 0;
    if (version) {
        string_join(rebuilt + out, sizeof(rebuilt) - out, "", package);
        while (rebuilt[out]) out++;
        rebuilt[out] = 0;
        string_join(rebuilt + out, sizeof(rebuilt) - out, " ", version);
        while (rebuilt[out]) out++;
        if (out + 2 < sizeof(rebuilt)) rebuilt[out++] = '\r', rebuilt[out++] = '\n';
    }

    if (fat32_create(current_volume, DOSGET_DATABASE, 0, &entry))
        (void)fat32_write(current_volume, &entry, 0, rebuilt, out);
}

/* \BOOT\DOSGET\<PACKAGE>.PKG, where what a package installed is written. */
static void dosget_record_path(const char* package, char* into,
                               boot_uint32_t size) {
    into[0] = 0;
    string_join(into, size, "", DOSGET_RECORDS "\\");
    string_join(into, size, "", package);
    string_join(into, size, "", ".PKG");
}

/* Write down what was just installed, and where.
 *
 * The same dull format as everything else here, one `key = value` per line:
 *
 *     directory = \GAMES
 *     file = GAMES.EXE
 */
static void dosget_record_files(const char* package, const char* directory,
                                const char* manifest,
                                boot_uint32_t manifest_length) {
    char path[PATH_MAX];
    char text[1024];
    char name[64];
    FAT_ENTRY entry;
    boot_uint32_t out = 0;
    boot_uint32_t at = 0;

    if (!current_volume) return;
    if (!fat32_stat(current_volume, DOSGET_RECORDS, &entry))
        fat32_create(current_volume, DOSGET_RECORDS, 1, &entry);

    text[0] = 0;
    string_join(text, sizeof(text), "", "directory = ");
    string_join(text, sizeof(text), "", directory);
    string_join(text, sizeof(text), "", "\r\n");
    while (text[out]) out++;

    for (;;) {
        const char* value = manifest_value(manifest, manifest_length, "file",
                                           &at);
        if (!value) break;
        take_line(value, name, sizeof(name));
        if (!name[0]) continue;
        text[out] = 0;
        string_join(text + out, sizeof(text) - out, "", "file = ");
        string_join(text + out, sizeof(text) - out, "", name);
        string_join(text + out, sizeof(text) - out, "", "\r\n");
        while (text[out]) out++;
    }

    dosget_record_path(package, path, sizeof(path));
    if (fat32_stat(current_volume, path, &entry))
        fat32_remove(current_volume, path);
    if (fat32_create(current_volume, path, 0, &entry))
        (void)fat32_write(current_volume, &entry, 0, text, out);
}

/* `dosget list` - what the source has. */
static void dosget_list(boot_uint32_t source, boot_uint8_t* buffer) {
    int got = dosget_fetch(source, "INDEX", buffer, DOSGET_BUFFER - 1);
    boot_uint32_t at = 0;

    if (got < 0) return;
    buffer[got] = 0;

    print_line("PACKAGE      VERSION  SUMMARY");
    while (at < (boot_uint32_t)got) {
        char line[128];
        boot_uint32_t start = at;

        while (at < (boot_uint32_t)got && buffer[at] != '\n') at++;
        take_line((const char*)(buffer + start), line, sizeof(line));
        at++;
        if (!line[0] || line[0] == '#') continue;
        print_line(line);
    }
}

/* Fetch one package's files and write them into a directory of its own. */
/* Package names are upper case, the way every other name in DOS is.
 *
 * The shell folds the command word but not its operands, so `dosget install
 * system` asked the server for `packages/system` - and a server whose files
 * live on a case-sensitive filesystem answered, correctly, that there is no
 * such thing. "The server refused the file" is a true sentence and a useless
 * one when the only difference is the shift key. */
static void upper_name(const char* from, char* into, boot_uint32_t size) {
    boot_uint32_t at = 0;
    while (from[at] && at + 1 < size) { into[at] = upper(from[at]); at++; }
    into[at] = 0;
}

static int dosget_install(boot_uint32_t source, const char* raw_package,
                          boot_uint8_t* buffer) {
    char package[64];
    char path[PATH_MAX];
    char directory[PATH_MAX];
    char manifest[1024];
    char name[64];
    char version[32];
    VOLUME* volume;
    boot_uint32_t manifest_length;
    boot_uint32_t at = 0;
    int files = 0;
    FAT_ENTRY entry;

    if (!current_volume) { print_line("No volume."); return 0; }
    volume = current_volume;
    upper_name(raw_package, package, sizeof(package));

    /* The manifest first: it names the files, and a package whose manifest is
       missing is not a package this knows how to unpack. */
    at = 0;
    path[0] = 0;
    string_join(path, sizeof(path), "packages/", package);
    string_join(path, sizeof(path), "", "/MANIFEST");
    {
        int got = dosget_fetch(source, path, manifest, sizeof(manifest) - 1);
        if (got < 0) return 0;
        manifest[got] = 0;
        manifest_length = (boot_uint32_t)got;
    }

    /* Where it goes. A package normally gets a directory of its own at the
       root, beside DOOM - a program with data files should not scatter them
       into a shared BIN. A package that replaces part of the system says so,
       and SYSTEM is the one that does. */
    {
        const char* target = manifest_value(manifest, manifest_length,
                                            "target", (boot_uint32_t*)0);
        const char* where = manifest_value(manifest, manifest_length,
                                           "volume", (boot_uint32_t*)0);

        /* Which partition, which on an installed machine is not the one the
         * shell is standing on.
         *
         * The kernel lives on the partition the firmware hands to the loader,
         * because that is the only one the loader can read before there is a
         * system. On a machine installed to a disk that is a second partition
         * with no drive letter - deliberately, so nothing deletes it by
         * accident - and writing to `Z:\BOOT` there produces a perfectly good
         * kernel on a volume nothing will ever boot from.
         *
         * Which is exactly what happened: every byte written, every message
         * correct, and the old kernel still running after the reboot. */
        if (where && prefix_matches(where, "loader")) {
            volume = volume_loader();
            if (!volume) {
                print_line("There is no loader partition to write to.");
                return 0;
            }
        }

        if (target) {
            take_line(target, path, sizeof(path));
        } else {
            path[0] = '\\';
            path[1] = 0;
            string_join(path, sizeof(path), "", package);
        }
    }
    /* Already there is not an error: installing over an older copy is the
       ordinary case, and is most of what a package manager does. */
    if (!fat32_stat(volume, path, &entry) &&
        !fat32_create(volume, path, 1, &entry)) {
        print("Could not make ");
        print_line(path);
        return 0;
    }
    {
        char here[PATH_MAX];
        boot_uint32_t at2 = 0;
        while (path[at2]) { directory[at2] = path[at2]; at2++; }
        directory[at2] = 0;
        (void)here;
    }

    at = 0;
    for (;;) {
        const char* value = manifest_value(manifest, manifest_length, "file",
                                           &at);
        char remote[PATH_MAX];
        char local[PATH_MAX];
        int got;

        if (!value) break;
        take_line(value, name, sizeof(name));
        if (!name[0]) continue;

        remote[0] = 0;
        string_join(remote, sizeof(remote), "packages/", package);
        string_join(remote, sizeof(remote), "/", name);

        got = dosget_fetch(source, remote, buffer, DOSGET_BUFFER);
        if (got < 0) return 0;

        local[0] = 0;
        string_join(local, sizeof(local), "", directory);
        string_join(local, sizeof(local), "\\", name);

        /* The old copy is kept, not deleted, when it is part of the system.
           A kernel that does not start is otherwise a machine that needs the
           USB stick this whole arrangement exists to retire. */
        if (fat32_stat(volume, local, &entry) &&
            word_is(package, "SYSTEM")) {
            char keep[PATH_MAX];
            FAT_ENTRY previous;

            keep[0] = 0;
            string_join(keep, sizeof(keep), "", directory);
            string_join(keep, sizeof(keep), "\\", "KERNEL.BAK");
            if (fat32_stat(volume, keep, &previous))
                fat32_remove(volume, keep);
            if (fat32_create(volume, keep, 0, &previous)) {
                boot_uint8_t* old = buffer + DOSGET_BUFFER / 2;
                boot_uint32_t was = fat32_read(volume, &entry, 0, old,
                                               DOSGET_BUFFER / 2);
                if (was) (void)fat32_write(volume, &previous, 0, old, was);
                print("  kept the old one as ");
                print_line(keep);
            }
        }

        if (fat32_stat(volume, local, &entry))
            fat32_remove(volume, local);
        if (!fat32_create(volume, local, 0, &entry)) {
            print("Could not write ");
            print_line(local);
            return 0;
        }
        if (fat32_write(volume, &entry, 0, buffer,
                        (boot_uint32_t)got) != (boot_uint32_t)got) {
            print("Could not write all of ");
            print_line(local);
            return 0;
        }

        print("  ");
        if (volume != current_volume) print("(loader) ");
        print(local);
        print("  ");
        print_dec((boot_uint64_t)got);
        print_line(" bytes");
        files++;
    }

    if (!files) {
        print_line("The manifest names no files.");
        return 0;
    }

    /* And make it runnable.
     *
     * A package installs into a directory of its own, which is the right place
     * for a program with data files beside it and the wrong place for the
     * shell to find it: nothing searches \GAMES. So the directory goes on the
     * program search path, and stays there across a reboot.
     *
     * Without this, installing succeeded, every file landed correctly, and
     * typing the program's name answered "Bad command or file name" - which is
     * the same sentence a machine gives for a package that was never installed
     * at all. That is how DOSFETCH came to be copied into \BIN by hand.
     *
     * `path = no` in the manifest opts out, and the two packages that are part
     * of the system say it: the kernel goes to \BOOT, which holds no programs,
     * and the utilities go to \BIN, which the shell already searches. The
     * default is to add, so a package written by somebody else is runnable
     * without their having known to ask. */
    {
        const char* wanted = manifest_value(manifest, manifest_length, "path",
                                            (boot_uint32_t*)0);
        char answer[16];

        answer[0] = 0;
        if (wanted) take_line(wanted, answer, sizeof(answer));
        if (!word_is(answer, "NO") && volume == current_volume) {
            if (config_add_program_path(current_volume, directory)) {
                print("  on the search path: ");
                print_line(directory);
            } else {
                print("  could not add ");
                print(directory);
                print_line(" to the search path");
            }
        }
    }

    {
        const char* value = manifest_value(manifest, manifest_length,
                                           "version", (boot_uint32_t*)0);
        take_line(value, version, sizeof(version));
        dosget_record(package, version[0] ? version : "0");
    }
    /* Recorded on the volume the shell is standing on even when the files went
       to the loader's, because that is where the record of the system lives
       and there is only one of it. */
    dosget_record_files(package, directory, manifest, manifest_length);
    return 1;
}


/* Is this package on the disk, whatever the database believes?
 *
 * SYSTEM is a special case and an obvious one: a machine that is running is
 * running a kernel, so the system is installed by definition. Everything else
 * has a directory of its own, and the directory being there is the evidence. */
/* `dosget remove` - take a package off this machine.
 *
 * The rule, and it is the whole design: it deletes the files a package
 * installed, inside that package's own directory, and nothing else. It is not
 * a command that can be aimed at the system.
 *
 * That is enforced by a property rather than by a list of names to protect. A
 * package's directory has to be a directory of its own - not the root, not
 * \BIN, not \BOOT - and only the files written down at install time are
 * deleted from it. Anything else in there is somebody's: DOOM's WAD is the
 * obvious one, and it is not the package manager's to throw away. The
 * directory itself goes only if it is empty afterwards, which the filesystem
 * enforces for us: fat32_remove refuses a directory with anything left in it.
 *
 * A package installed before there were records cannot be removed, and says
 * so, because the alternative is guessing what belongs to it. Reinstalling
 * writes the record, and then it can be. */
static int dosget_protected(const char* directory) {
    return !directory[0] || directory[0] != '\\' ||
           word_is(directory, "\\") || word_is(directory, "\\BIN") ||
           word_is(directory, "\\BOOT");
}

static int dosget_remove(const char* raw_package) {
    char package[64];
    char record[PATH_MAX];
    char directory[PATH_MAX];
    char text[1024];
    char name[64];
    char path[PATH_MAX];
    FAT_ENTRY entry;
    boot_uint32_t length;
    boot_uint32_t at = 0;
    int removed = 0;
    int kept = 0;

    if (!current_volume) { print_line("No volume."); return 0; }
    upper_name(raw_package, package, sizeof(package));

    if (word_is(package, "SYSTEM")) {
        print_line("SYSTEM is the kernel this machine is running. It can be "
                   "updated, not removed.");
        return 0;
    }

    dosget_record_path(package, record, sizeof(record));
    if (!fat32_stat(current_volume, record, &entry)) {
        print(package);
        print_line(" is not installed, or was installed before this system "
                   "recorded what a package contains.");
        print_line("Install it again and it can then be removed.");
        return 0;
    }
    length = fat32_read(current_volume, &entry, 0, text, sizeof(text) - 1);
    text[length] = 0;

    {
        const char* value = manifest_value(text, length, "directory",
                                           (boot_uint32_t*)0);
        directory[0] = 0;
        if (value) take_line(value, directory, sizeof(directory));
    }
    if (dosget_protected(directory)) {
        print("The record for ");
        print(package);
        print(" says its files are in ");
        print(directory[0] ? directory : "no directory at all");
        print_line(", which is the system's. Nothing was removed.");
        return 0;
    }

    for (;;) {
        const char* value = manifest_value(text, length, "file", &at);

        if (!value) break;
        take_line(value, name, sizeof(name));
        if (!name[0]) continue;

        path[0] = 0;
        string_join(path, sizeof(path), "", directory);
        string_join(path, sizeof(path), "\\", name);
        if (!fat32_stat(current_volume, path, &entry)) continue;
        if (fat32_remove(current_volume, path)) {
            print("  removed ");
            print_line(path);
            removed++;
        } else {
            print("  could not remove ");
            print_line(path);
            kept++;
        }
    }

    /* The directory goes only when nothing is left in it, and what is left
       belongs to whoever put it there. */
    if (!kept && fat32_stat(current_volume, directory, &entry)) {
        if (fat32_remove(current_volume, directory)) {
            print("  removed ");
            print_line(directory);
        } else {
            print("  ");
            print(directory);
            print_line(" was left: there is something in it that this did not "
                       "install.");
        }
    }

    config_remove_program_path(current_volume, directory);
    fat32_remove(current_volume, record);
    dosget_record(package, (const char*)0);

    print(package);
    if (!removed && !kept) {
        print_line(" was recorded but none of its files were there. The record "
                   "is gone now.");
        return 1;
    }
    print(" removed, ");
    print_dec((boot_uint64_t)removed);
    print_line(removed == 1 ? " file." : " files.");
    return 1;
}

static int dosget_present(const char* package) {
    char path[PATH_MAX];
    FAT_ENTRY entry;

    if (!current_volume) return 0;
    if (word_is(package, "SYSTEM")) return 1;

    path[0] = '\\';
    path[1] = 0;
    string_join(path, sizeof(path), "", package);
    return fat32_stat(current_volume, path, &entry);
}

/* `dosget update` - everything already here, brought up to date.
 *
 * dnf's meaning of the word rather than apt's: it refreshes and upgrades in
 * one go. A package is "already here" when the database says so, which is why
 * installing writes to it. Packages nobody asked for are not installed by
 * this, and packages that have not moved are not fetched twice. */
static void dosget_update(boot_uint32_t source, boot_uint8_t* buffer) {
    char index[2048];
    boot_uint32_t at = 0;
    boot_uint32_t got;
    int changed = 0;

    {
        int fetched = dosget_fetch(source, "INDEX", index, sizeof(index) - 1);
        if (fetched < 0) return;
        index[fetched] = 0;
        got = (boot_uint32_t)fetched;
    }

    while (at < got) {
        char line[128];
        char name[64];
        char offered[32];
        char here[32];
        boot_uint32_t start = at;
        boot_uint32_t split = 0;

        while (at < got && index[at] != '\n') at++;
        take_line(index + start, line, sizeof(line));
        at++;
        if (!line[0] || line[0] == '#') continue;

        /* NAME VERSION SUMMARY, split at the first two spaces. */
        while (line[split] && line[split] != ' ') split++;
        if (!line[split]) continue;
        line[split] = 0;
        take_line(line, name, sizeof(name));
        split++;
        {
            boot_uint32_t end = split;
            while (line[end] && line[end] != ' ') end++;
            line[end] = 0;
            take_line(line + split, offered, sizeof(offered));
        }

        dosget_installed(name, here, sizeof(here));
        if (!here[0]) {
            /* No record. That is not the same as not installed, and treating
               it as such is how a machine with a damaged database reports
               that everything is up to date while running something older.
             *
             * The database is a record; the filesystem is the truth. A package
             * whose files are here is installed, whatever the record says, and
             * a version nobody wrote down is one that cannot match - so it is
             * fetched and written down properly this time. */
            if (!dosget_present(name)) continue;
            print(name);
            print_line(": installed, but no version recorded - fetching it");
        } else if (!strcmp(here, offered)) {
            continue;                        /* already at that version */
        } else {
            print(name);
            print(": ");
            print(here);
            print(" -> ");
            print_line(offered);
        }
        if (dosget_install(source, name, buffer)) changed++;
        continue;

    }

    if (!changed) {
        print_line("Everything is already up to date.");
        return;
    }
    print_dec((boot_uint64_t)changed);
    print_line(" package(s) updated.");
    print_line("");
    print_line("A new kernel takes effect at the next boot. The old one is");
    print_line("kept as \\BOOT\\KERNEL.BAK, and the loader falls back to it");
    print_line("if the new one will not start.");
}

static void command_dosget(const ARGUMENTS* arguments) {
    boot_uint32_t source = 0;
    boot_uint8_t* buffer;
    char shown[16];

    /* Before the network is looked at, because removing something needs no
       source at all - and a machine that has been carried away from its
       network is exactly where somebody wants to free some space. */
    if (word_is(arguments->operand[0], "REMOVE")) {
        if (!arguments->operand[1][0])
            print_line("Usage: dosget remove <name>");
        else
            (void)dosget_remove(arguments->operand[1]);
        return;
    }

    if (!net_configured()) {
        print_line("No address yet. Run `net start`, or `net set` on a cable.");
        return;
    }
    if (!dosget_source(&source)) {
        print("The source in " DOSGET_CONFIG " is not an address.");
        print_line("");
        return;
    }

    net_format_address(source, shown);

    if (!arguments->operand[0][0]) {
        char version[32];

        print("Source          : ");
        print_line(shown);
        dosget_installed("SYSTEM", version, sizeof(version));
        print("System version  : ");
        /* Said out loud because its absence was confusing on its own: a
           machine that has never been updated over the network and one whose
           record was lost look identical without this line. */
        print_line(version[0] ? version : "not recorded - never updated here");
        print_line("");
        print_line("dosget list              what the source has");
        print_line("dosget install <name>    fetch it and unpack it");
        print_line("dosget update            bring everything here up to date");
        print_line("dosget remove <name>     delete what it installed");
        print_line("");
        print("The source is read from " DOSGET_CONFIG);
        print_line(" every time,");
        print_line("so it can be changed on a running system.");
        return;
    }

    /* One buffer for the largest thing a package might carry, taken and given
       back around the command rather than held: this is the only place in the
       system that needs a quarter of a megabyte at once. */
    buffer = (boot_uint8_t*)alloc_pages(DOSGET_BUFFER / PAGE_SIZE);
    if (!buffer) { print_line("Out of memory."); return; }

    print("Source: ");
    print_line(shown);

    if (word_is(arguments->operand[0], "LIST")) {
        dosget_list(source, buffer);
    } else if (word_is(arguments->operand[0], "INSTALL")) {
        /* Install pays no attention to versions on purpose: it is the way to
           put a package on regardless of what the records claim, which is what
           somebody wants when the records are what is wrong. */
        if (!arguments->operand[1][0]) {
            print_line("Usage: dosget install <name>");
        } else if (dosget_install(source, arguments->operand[1], buffer)) {
            print(arguments->operand[1]);
            print_line(" installed.");
        }
    } else if (word_is(arguments->operand[0], "UPDATE")) {
        dosget_update(source, buffer);
    } else {
        print_line("dosget: list, install, update or remove.");
    }

    free_pages(buffer, DOSGET_BUFFER / PAGE_SIZE);
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

/* A directory and everything under it, for the installer.
 *
 * xcopy_tree does this too, but it prints as it goes in a shape that belongs
 * at a prompt and reports failure by returning to one. The installer has a
 * screen it owns and a licence to abandon, so it says less per file and stops
 * the whole install when a copy fails. Sharing one of them would mean the
 * installer printing a directory listing over its own banner. */
static int setup_copy_tree(VOLUME* from, const char* source,
                           VOLUME* to, const char* target) {
    FAT_DIRECTORY directory;
    FAT_ENTRY entry;

    if (!fat32_stat(from, source, &entry)) return 1;   /* absent is not a fault */
    if (!(entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) return 1;
    if (!fat32_stat(to, target, &entry) &&
        !fat32_create(to, target, 1, &entry)) {
        print("    ");
        print(target);
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("  FAILED");
        console_use_theme();
        return 0;
    }
    if (!fat32_opendir(from, source, &directory)) return 1;

    while (fat32_readdir(&directory, &entry)) {
        char from_path[PATH_MAX];
        char to_path[PATH_MAX];

        if (entry.name[0] == '.') continue;
        from_path[0] = 0;
        string_join(from_path, sizeof(from_path), "", source);
        string_join(from_path, sizeof(from_path), "\\", entry.name);
        to_path[0] = 0;
        string_join(to_path, sizeof(to_path), "", target);
        string_join(to_path, sizeof(to_path), "\\", entry.name);

        if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) {
            if (!setup_copy_tree(from, from_path, to, to_path)) return 0;
            continue;
        }
        if (!setup_copy(from, from_path, to, to_path)) return 0;
    }
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
/* The volume to install from: the one the shell is standing on when it
   carries the file in question, and otherwise whichever one does. */
static VOLUME* setup_source_of(const char* path) {
    FAT_ENTRY entry;

    if (current_volume && fat32_stat(current_volume, path, &entry))
        return current_volume;
    return volume_holding(path);
}

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
    {
        VOLUME* from = setup_source_of("\\BIN");
        print("  Installing from ");
        if (from && from->letter) { put(from->letter); print(":"); }
        else print("this drive");
        if (from && from->label[0]) { print(" ("); print(from->label); print(")"); }
        print_line("");
    }
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
       same volume; on a two-partition one they are not.
     *
     * The drive the user is standing on is asked first, and that is not a
     * nicety. Searching every volume for the files finds the installed system
     * before it finds the media, so a machine booted from its disk with newer
     * install media plugged in would offer to copy the old system onto the new
     * stick - which is backwards, and reads as correct until you look at what
     * it says twice. Somebody who has changed to a drive and typed `setup`
     * means that drive. */
    loader_source = setup_source_of("\\EFI\\BOOT\\BOOTX64.EFI");
    system_source = setup_source_of("\\BIN");
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

    /* And the media has to be found again, because it has just moved.
     *
     * The source volumes were located before any of this, and remounting
     * rebuilds the whole volume table: a disk that had no partitions
     * contributed nothing to it and now contributes two, so every entry after
     * that point is a different volume than it was. The installer was left
     * holding a pointer to the medium that referred to the empty partition it
     * had just created, and the first copy failed with the file "not found" -
     * on a medium it had read the licence from a moment earlier.
     *
     * Found by what they contain rather than remembered, which is what makes
     * this correct rather than merely fixed: the freshly formatted partitions
     * hold neither of these files. */
    loader_source = setup_source_of("\\EFI\\BOOT\\BOOTX64.EFI");
    system_source = setup_source_of("\\BIN");
    if (!loader_source || !system_source) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("  The installation media is no longer readable.");
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

    /* The packages, and the records that say what they are.
     *
     * Without this the installed machine had the system and none of the
     * programs: \BIN went across and \COMMANDER, \MIZU and the rest stayed
     * on the stick. The obvious fix - "fetch them afterwards" - is the wrong
     * one, because the machine that most needs them is the one whose network
     * card we do not drive. What is on the medium has to arrive.
     *
     * Which directories to copy is not guessed: every package wrote down where
     * it put itself when it was installed, so the medium already knows. The
     * records go across too, so `dosget remove` still works afterwards, and so
     * does the search path in \BOOT\CONFIG - a package that arrives without
     * its place on the path is a package that cannot be run. */
    print_line("  Copying the packages");
    {
        FAT_DIRECTORY directory;
        FAT_ENTRY entry;

        (void)fat32_create(system_target, DOSGET_RECORDS, 1, &(FAT_ENTRY){0});
        (void)fat32_create(system_target, CONFIG_DIRECTORY, 1, &(FAT_ENTRY){0});

        if (fat32_opendir(system_source, DOSGET_RECORDS, &directory)) {
            while (fat32_readdir(&directory, &entry)) {
                char record[PATH_MAX];
                char text[1024];
                boot_uint32_t length;
                FAT_ENTRY found;
                char where[PATH_MAX];

                if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) continue;
                record[0] = 0;
                string_join(record, sizeof(record), "", DOSGET_RECORDS "\\");
                string_join(record, sizeof(record), "", entry.name);
                if (!setup_copy(system_source, record, system_target, record))
                    return;

                if (!fat32_stat(system_source, record, &found)) continue;
                length = fat32_read(system_source, &found, 0, text,
                                    sizeof(text) - 1);
                text[length] = 0;
                {
                    const char* value = manifest_value(text, length, "directory",
                                                       (boot_uint32_t*)0);
                    where[0] = 0;
                    if (value) take_line(value, where, sizeof(where));
                }
                if (!where[0] || where[0] != '\\') continue;
                if (!setup_copy_tree(system_source, where, system_target, where))
                    return;
            }
        }

        (void)setup_copy(system_source, CONFIG_DIRECTORY "\\SYSTEM.CFG",
                         system_target, CONFIG_DIRECTORY "\\SYSTEM.CFG");
        (void)setup_copy(system_source, DOSGET_DATABASE,
                         system_target, DOSGET_DATABASE);
        (void)setup_copy(system_source, "\\AUTOEXEC.BAT",
                         system_target, "\\AUTOEXEC.BAT");
    }

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

/* ---- chkdsk --------------------------------------------------------------
 *
 * The system had no way to say whether a volume was sound until a bug in its
 * own writer made a directory grow past one cluster and left an
 * end-of-directory marker in the middle of the chain. Every file after that
 * point was written correctly and could never be found again - by us, or by
 * any other operating system, because they all obey the same marker. The
 * writer was fixed; the volumes it had already written were not, and nothing
 * in the system could either see the damage or undo it.
 *
 * Without /F it changes nothing, which is the DOS behaviour and the right
 * default: the first thing anybody wants from a disk checker is to be told
 * what is wrong, not to have it decided for them.
 */
/* "1 sectors" is the kind of thing that makes a report look like nobody ever
   read one. */
static void print_count(boot_uint64_t number, const char* singular,
                        const char* plural) {
    print_dec(number);
    put(' ');
    print(number == 1 ? singular : plural);
}

static void chkdsk_fault(FAT_FAULT fault, const char* where,
                         boot_uint64_t number, int repaired) {
    console_set_color(repaired ? console_theme()->foreground
                               : console_theme()->error,
                      console_theme()->background);
    print("  ");
    if (where && where[0]) {
        print(where);
        print("  ");
    }

    switch (fault) {
    case FAT_FAULT_TERMINATOR:
        print("cluster ");
        print_dec(number);
        print(" ends the directory early and hides everything after it");
        break;
    case FAT_FAULT_ORPHAN_LONG_NAME:
        print_count(number, "long-name entry belongs", "long-name entries belong");
        print(" to no file");
        break;
    case FAT_FAULT_BAD_LINK:
        print("the chain leads to cluster ");
        print_dec(number);
        print(", which is not on this volume");
        break;
    case FAT_FAULT_CROSS_LINKED:
        print("cluster ");
        print_dec(number);
        print(" is claimed twice");
        break;
    case FAT_FAULT_SIZE_TOO_LARGE:
        print_count(number, "cluster", "clusters");
        print(" more than the recorded size needs");
        break;
    case FAT_FAULT_SIZE_TOO_SMALL:
        print("recorded size ");
        print_dec(number);
        print(" is more than the clusters hold");
        break;
    case FAT_FAULT_PARENT_WRONG:
        print("\"..\" points at cluster ");
        print_dec(number);
        break;
    case FAT_FAULT_LOST_CLUSTERS:
        print_count(number, "cluster is allocated and belongs",
                            "clusters are allocated and belong");
        print(" to nothing");
        break;
    case FAT_FAULT_FAT_COPIES_DIFFER:
        print("the copies of the allocation table differ in ");
        print_count(number, "sector", "sectors");
        break;
    case FAT_FAULT_FREE_COUNT_WRONG:
        print("the free-space hint says ");
        print_dec(number);
        print(" clusters, and the table says otherwise");
        break;
    case FAT_FAULT_TOO_COMPLEX:
        print("too many directories to check at once");
        break;
    }

    print_line(repaired ? "  - repaired" : "");
    console_use_theme();
}

static void command_chkdsk(const ARGUMENTS* arguments) {
    VOLUME* volume = current_volume;
    FAT_CHECK_RESULT result;
    int repair = 0;

    for (int index = 0; index < arguments->switch_count; index++)
        if (upper(arguments->switches[index]) == 'F') repair = 1;

    if (arguments->operand_count) {
        const char* name = arguments->operand[0];
        volume = name[0] && name[1] == ':' && !name[2]
               ? volume_by_letter(name[0]) : (VOLUME*)0;
        if (!volume) {
            print_line("chkdsk [drive:] [/F]");
            print_line("");
            print_line("Checks the volume and says what is wrong with it.");
            print_line("/F repairs what can be repaired; without it nothing");
            print_line("on the disk is changed.");
            return;
        }
    }
    if (!volume) { print_line("No volume."); return; }

    print("Checking ");
    put(volume->letter);
    print(":");
    if (volume->label[0]) {
        print(" (");
        print(volume->label);
        print(")");
    }
    print_line(repair ? " - repairing" : "");
    print_line("");

    if (!fat32_check(volume, repair, chkdsk_fault, &result)) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("The volume could not be checked. It may not be FAT32, or");
        print_line("there was not enough memory to hold the cluster map.");
        console_use_theme();
        return;
    }

    if (result.faults) print_line("");
    print("  ");
    print_dec(fat32_total_bytes(volume));
    print_line(" bytes total disk space");
    print("  ");
    print_dec(result.bytes_in_files);
    print(" bytes in ");
    print_count(result.files, "file", "files");
    print_line("");
    print("  ");
    print_count(result.directories, "directory", "directories");
    print_line("");
    print("  ");
    print_dec(fat32_free_bytes(volume));
    print_line(" bytes available");
    print("  ");
    print_dec(result.cluster_bytes);
    print(" bytes in each allocation unit, holding ");
    print_count(result.entries_per_cluster, "directory entry", "directory entries");
    print_line("");
    print_line("");

    if (!result.faults) {
        print_line("  No faults found.");
    } else {
        print("  ");
        print_dec(result.faults);
        print(result.faults == 1 ? " fault found, " : " faults found, ");
        print_dec(result.repaired);
        print_line(" repaired.");
        if (result.repaired < result.faults) {
            if (!repair) {
                print("  Run `chkdsk ");
                put(volume->letter);
                print_line(": /F` to repair them.");
            } else {
                print_line("  What is left needs a decision this cannot make");
                print_line("  on its own - a cross-linked cluster belongs to");
                print_line("  one file or the other, and only you know which.");
            }
        }
    }

    /* A check that gave up partway is not a clean bill of health, and saying
       so plainly matters more than the number above it. */
    if (!result.complete) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("");
        print_line("  The check did not finish. Some of the volume was not");
        print_line("  read, so this says nothing about the parts it missed.");
        console_use_theme();
    }
}

/* Every function on the bus, as the kernel sees it.
 *
 * This exists because a driver that reports "not found" says nothing about
 * why: the device may be absent, behind a bridge nobody walked, past the end
 * of a table that silently filled up, or simply of a kind we do not drive. On
 * real hardware, with no serial cable attached, this is the only way to tell
 * those apart. */
/* `layout` - what the keyboard is typing, and how to change it.
 *
 * Alt+Shift does the same thing and is what anybody will reach for. This
 * exists because a key combination nobody mentioned is not a feature: it is a
 * thing that happens by accident and confuses somebody. */
static void command_layout(const ARGUMENTS* arguments) {
    if (arguments->operand_count) {
        const char* name = arguments->operand[0];
        int chosen = -1;

        if (word_is(name, "EN") || word_is(name, "ENGLISH")) chosen = LAYOUT_EN;
        else if (word_is(name, "RU") || word_is(name, "RUSSIAN")) chosen = LAYOUT_RU;
        else if (word_is(name, "UK") || word_is(name, "UKRAINIAN")) chosen = LAYOUT_UK;
        else if (word_is(name, "EL") || word_is(name, "GREEK")) chosen = LAYOUT_GR;

        if (chosen < 0) {
            print_line("layout [en|ru|uk|el]");
            print_line("");
            print_line("With no argument, which layout is typing now.");
            return;
        }
        layout_select(chosen);
        if (chosen != LAYOUT_EN) layout_set_alternate(chosen);
    }

    print("Typing in ");
    print(layout_name(layout_current()));
    print_line(".");
    print("Alt+Shift switches between English and ");
    print(layout_name(layout_alternate()));
    print_line(".");
}

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

static void run_batch(VOLUME* volume, const char* path,
                      const char* arguments);

/* Did the last thing that ran get stopped with Ctrl+C?
 *
 * A batch file is a list of commands somebody wanted run in order, and
 * stopping one of them means stopping that. Carrying on to the next line would
 * make Ctrl+C useless against exactly the case it exists for - a long
 * AUTOEXEC.BAT, or a loop of commands - because the way out would be to
 * interrupt every command in the file one at a time. */
static int interrupted;

/* What the last thing to run returned, which is what `IF ERRORLEVEL` asks
 * about.
 *
 * Kept for the built-in commands too, and set to zero by them, because DOS
 * did: a batch file testing ERRORLEVEL after `copy` should see the result of
 * the copy and not whatever a program left behind three lines earlier. Only
 * the things that can fail set anything else. */
static int last_exit_code;

/* Batch state. All of it is about the file being read rather than about a
 * command, which is why it lives here and not inside run_batch: GOTO is a
 * command that has to tell the loop where to go next, and there is no other
 * way for it to say so.
 *
 * `batch_echo` is ECHO ON/OFF, which survives across nested files as it did in
 * DOS - a called file inherits the setting of the one that called it. */
#define BATCH_LABEL_MAX 64
#define BATCH_ARGUMENT_MAX 10

static char batch_target[BATCH_LABEL_MAX];
static int batch_jump;              /* a GOTO is waiting to be honoured */
static int batch_quit;              /* GOTO :EOF, or a fault: end this file */
static int batch_echo = 1;

/* %0..%9 for the file being run. %0 is the file's own name, as in DOS. */
static char batch_argument[BATCH_ARGUMENT_MAX][PATH_MAX];
static int batch_argument_count;

/* How many batch files are open. GOTO, IF and the loop that reads them all
   need it, and they are spread across this file, so it is declared with the
   rest of the batch state rather than beside the one function that used to
   own it. */
static int batch_depth;

static void execute(const char* input);

/* The file a command should read instead of the keyboard, or NULL. Declared
   with the other shell-wide state because the filters below use it and the
   parser that sets it comes after them. */
static const char* command_input_source(void);

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
    /* With no file, whatever arrived through a pipe. `dir | more` is the
       oldest reason pipes exist and it would be strange for this of all
       commands not to take one. */
    if (!name[0]) {
        const char* piped = command_input_source();
        boot_uint64_t at = 0;
        while (piped && piped[at] && at + 1 < PATH_MAX) { name[at] = piped[at]; at++; }
        name[at] = 0;
    }
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
    VOLUME* program_volume;
    int code;
    int exit_code = 0;

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

    if (!find_program(name, &program_volume, path, &entry)) {
        /* Nothing by that name as a program; try it as a batch file. */
        if (has_extension) return 0;
        name[length - 4] = 0;
        {
            const char* batch = ".BAT";
            for (int index = 0; index < 4; index++) name[length - 4 + index] = batch[index];
        }
        if (!find_program(name, &program_volume, path, &entry)) return 0;
        run_batch(program_volume, path, arguments->tail);
        return 1;
    }
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;

    /* An explicit .BAT is a batch file, not a program image. */
    if (length > 4 && (name[length - 3] == 'B' || name[length - 3] == 'b') &&
        (name[length - 2] == 'A' || name[length - 2] == 'a') &&
        (name[length - 1] == 'T' || name[length - 1] == 't')) {
        run_batch(current_volume, path, arguments->tail);
        return 1;
    }

    syscall_set_location(current_volume, current_path);
    code = program_run(program_volume, path, arguments->tail, &exit_code);
    syscall_close_all();
    /* The keyboard back to English, for the same reason as the screen and the
       colours below: a program that asked for Alt+Shift and exited while the
       other layout was selected would hand the prompt a keyboard typing in
       Cyrillic, and the prompt is ASCII. */
    layout_gesture_enable(0);
    /* And take the screen back, whether or not the program gave it up. A
       program that returns while still holding it would otherwise leave the
       shell invisible with no way to ask for it back - which is precisely the
       failure this mode was shaped to avoid. */
    graphics_leave();
    /* And the colours back too, for the same reason and with the same
       reasoning. A program is free to paint the console however it likes while
       it runs; a program that exits having left the shell wearing its scheme
       has changed the system, and nothing asked it to. The cursor comes back
       as well - a program that hid it and exited leaves a prompt with nothing
       blinking at the end of it. */
    console_use_theme();
    console_show_cursor(1);

    if (code == PROGRAM_NOT_LOADABLE) {
        console_set_color(console_theme()->error, console_theme()->background);
        print_line("Not a valid Koi-DOS program.");
        console_use_theme();
    } else if (code == PROGRAM_REFUSED) {
        /* program_run() has already said why, in more detail than this could. */
    }
    last_exit_code = (code == PROGRAM_OK) ? exit_code : 1;
    if (code == PROGRAM_NOT_LOADABLE || code == PROGRAM_REFUSED) {
        /* already reported above */
    } else if (exit_code == KOI_EXIT_INTERRUPTED) {
        /* Ctrl+C already printed itself, at the moment it happened. Saying
           "Exit code 130" underneath it would be the same news twice, in a
           form nobody asked for. What it does do is stop a batch file. */
        interrupted = 1;
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

/* `SET`, and the rule about what counts as the name.
 *
 * `SET NAME=VALUE` sets, `SET NAME=` removes, `SET` alone lists. The split is
 * at the first `=` and nowhere else, because a value may contain one and a
 * name may not - `SET PROMPT=$P$G` and `SET GREETING=a=b` both mean what they
 * look like.
 *
 * Spaces around the `=` are not eaten. DOS did not eat them either, and it
 * matters: `SET PATH = \BIN` sets a variable called "PATH " to " \BIN", and a
 * shell that quietly tidied that up would be a shell where the same line means
 * two different things depending on who typed it. The name is trimmed of what
 * comes before it and nothing else. */
static void command_set(const char* tail) {
    char name[ENVIRONMENT_NAME_MAX];
    const char* cursor = skip_spaces(tail);
    const char* equals = cursor;
    boot_uint64_t length = 0;

    if (!*cursor) {
        const char* variable;
        const char* value;

        for (int index = 0; environment_at(index, &variable, &value); index++) {
            print(variable);
            put('=');
            print_line(value);
        }
        return;
    }

    while (*equals && *equals != '=') equals++;
    if (!*equals) {
        /* `SET NAME` on its own shows one, which is what somebody checking
           reaches for before they find that `SET` lists everything. */
        const char* value;

        while (cursor[length] && length + 1 < sizeof(name))
            { name[length] = cursor[length]; length++; }
        name[length] = 0;
        while (length && name[length - 1] == ' ') name[--length] = 0;
        value = environment_get(name);
        if (!value) {
            print(name);
            print_line(" is not set.");
            return;
        }
        /* Printed under the name it is stored as, not the one that was just
           typed: names are matched without case, and echoing back `path` when
           the variable is `PATH` would suggest there are two of them. */
        {
            const char* stored;
            const char* ignored;

            for (int index = 0; environment_at(index, &stored, &ignored); index++)
                if (environment_get(stored) == value) { print(stored); break; }
        }
        put('=');
        print_line(value);
        return;
    }

    while (cursor + length < equals && length + 1 < sizeof(name))
        { name[length] = cursor[length]; length++; }
    name[length] = 0;

    if (!name[0]) { print_line("Usage: set NAME=value"); return; }
    if (!environment_set(name, equals + 1)) {
        print_line("No room for another variable, or the value is too long.");
        return;
    }
}

/* ---- Redirection and pipes -----------------------------------------------
 *
 * `dir > list.txt`, `type log | more`, `sort < names.txt`. The operators are
 * DOS's and so is the implementation: a pipe is a temporary file. DOS did it
 * that way because it had no processes to run side by side, which is exactly
 * true here - the left-hand command finishes before the right-hand one starts,
 * and what flowed between them has to be somewhere in the meantime.
 *
 * The output is caught in console.c, at the one function everything printing
 * passes through, so a program's output redirects exactly as a built-in's
 * does. Input is a file the reading commands are handed instead of the
 * keyboard.
 */
#define PIPE_FILE "\\PIPE.$$$"

static VOLUME* capture_volume;
static FAT_ENTRY capture_entry;
static boot_uint32_t capture_position;
static char capture_buffer[512];
static boot_uint32_t capture_pending;
static int capture_failed;

static void capture_flush(void) {
    if (!capture_pending) return;
    if (fat32_write(capture_volume, &capture_entry, capture_position,
                    capture_buffer, capture_pending) != capture_pending)
        capture_failed = 1;
    capture_position += capture_pending;
    capture_pending = 0;
}

/* Buffered, because a character at a time through the filesystem would mean a
   directory listing costing one write per letter. */
static void capture_sink(char character) {
    capture_buffer[capture_pending++] = character;
    if (capture_pending >= sizeof(capture_buffer)) capture_flush();
}

/* Send what a command prints into a file. Returns 0 when the file could not be
   made, in which case nothing is redirected and the caller says so. */
static int capture_begin(const char* name, int append) {
    char path[PATH_MAX];

    capture_failed = 0;
    capture_pending = 0;
    capture_position = 0;
    if (!resolve_path(name, &capture_volume, path)) return 0;

    if (fat32_stat(capture_volume, path, &capture_entry)) {
        if (capture_entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;
        if (append) capture_position = capture_entry.size;
        else {
            fat32_remove(capture_volume, path);
            if (!fat32_create(capture_volume, path, 0, &capture_entry)) return 0;
        }
    } else if (!fat32_create(capture_volume, path, 0, &capture_entry)) {
        return 0;
    }

    console_capture(capture_sink);
    return 1;
}

static int capture_end(void) {
    console_capture((void (*)(char))0);
    capture_flush();
    return !capture_failed;
}

/* The file a command reads instead of the keyboard, for `<` and for the right
   half of a pipe. Empty when there is none. */
static char input_source[PATH_MAX];

static const char* command_input_source(void) {
    return input_source[0] ? input_source : (const char*)0;
}

static void set_input_source(const char* name) {
    boot_uint64_t at = 0;

    while (name && name[at] && at + 1 < sizeof(input_source))
        { input_source[at] = name[at]; at++; }
    input_source[at] = 0;
}

/* ---- Text filters --------------------------------------------------------
 *
 * FIND and SORT read a file, or whatever arrived through a pipe when no file
 * is named. That is the whole of what makes `dir | sort` work, and it is why
 * they are written together: a filter that could only read a file it was told
 * about would be half a filter.
 */

/* The text to work on: the named file, or the pipe. Returns a buffer the
   caller frees, and its length, or NULL when there is nothing to read. */
static char* read_text(const char* name, boot_uint32_t* length_out) {
    VOLUME* volume;
    char path[PATH_MAX];
    FAT_ENTRY entry;
    char* contents;
    boot_uint32_t offset = 0;

    if (!name || !name[0]) name = command_input_source();
    if (!name) return (char*)0;
    if (!resolve_path(name, &volume, path)) return (char*)0;
    if (!fat32_stat(volume, path, &entry)) return (char*)0;
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return (char*)0;

    contents = (char*)kmalloc(entry.size + 1);
    if (!contents) return (char*)0;
    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset,
                                       contents + offset, entry.size - offset);
        if (!got) break;
        offset += got;
    }
    contents[offset] = 0;
    *length_out = offset;
    return contents;
}

/* Does `line` contain `wanted`? Plain substring, no patterns: DOS's FIND had
   none either, and a system with no regular expressions anywhere should not
   grow its first one inside a filter. */
static int line_holds(const char* line, boot_uint64_t length,
                      const char* wanted, int fold) {
    boot_uint64_t needle = strlen(wanted);

    if (!needle) return 1;
    if (needle > length) return 0;
    for (boot_uint64_t start = 0; start + needle <= length; start++) {
        boot_uint64_t at = 0;
        while (at < needle) {
            char a = line[start + at];
            char b = wanted[at];
            if (fold) { a = upper(a); b = upper(b); }
            if (a != b) break;
            at++;
        }
        if (at == needle) return 1;
    }
    return 0;
}

/* FIND "text" [file] - the lines that contain it.
 *
 *   /V  the lines that do not
 *   /C  how many, rather than which
 *   /N  numbered
 *   /I  ignoring case
 */
static void command_find(const ARGUMENTS* arguments) {
    char wanted[PATH_MAX];
    char* text;
    boot_uint32_t length = 0;
    boot_uint32_t at = 0;
    boot_uint32_t number = 0;
    boot_uint32_t matched = 0;
    int invert = 0, count_only = 0, numbered = 0, fold = 0;
    const char* file;

    for (int index = 0; index < arguments->switch_count; index++) {
        char option = upper(arguments->switches[index]);
        if (option == 'V') invert = 1;
        else if (option == 'C') count_only = 1;
        else if (option == 'N') numbered = 1;
        else if (option == 'I') fold = 1;
    }

    if (!arguments->operand_count) {
        print_line("Usage: find \"text\" [file] [/v] [/c] [/n] [/i]");
        return;
    }
    {
        const char* source = arguments->operand[0];
        boot_uint64_t out = 0;
        while (*source && out + 1 < sizeof(wanted)) wanted[out++] = *source++;
        wanted[out] = 0;
    }
    file = arguments->operand_count > 1 ? arguments->operand[1] : "";

    text = read_text(file, &length);
    if (!text) { print_line("File not found."); return; }

    while (at <= length) {
        boot_uint32_t start = at;
        boot_uint32_t end;

        while (at < length && text[at] != '\n') at++;
        end = at;
        while (end > start && text[end - 1] == '\r') end--;
        number++;

        if (line_holds(text + start, end - start, wanted, fold) != !invert) {
            if (at >= length) break;
            at++;
            continue;
        }
        matched++;
        if (!count_only) {
            if (numbered) { print_dec(number); print("]"); }
            for (boot_uint32_t index = start; index < end; index++) put(text[index]);
            print_line("");
        }
        if (at >= length) break;
        at++;
    }

    if (count_only) { print_dec(matched); print_line(" line(s)"); }
    kfree(text);
}

/* SORT [file] - the lines in order.
 *
 *   /R  descending
 *
 * An insertion sort over an array of offsets. The lines are not moved and
 * nothing is copied: a file of a few thousand lines is what this is for, and
 * the alternative is a quicksort nobody can check by reading. */
#define SORT_MAX 2048

static void command_sort(const ARGUMENTS* arguments) {
    static boot_uint32_t start[SORT_MAX];
    static boot_uint32_t end[SORT_MAX];
    char* text;
    boot_uint32_t length = 0;
    boot_uint32_t at = 0;
    boot_uint32_t count = 0;
    int descending = 0;

    for (int index = 0; index < arguments->switch_count; index++)
        if (upper(arguments->switches[index]) == 'R') descending = 1;

    text = read_text(arguments->operand_count ? arguments->operand[0] : "",
                     &length);
    if (!text) { print_line("File not found."); return; }

    while (at <= length && count < SORT_MAX) {
        boot_uint32_t line_end;

        start[count] = at;
        while (at < length && text[at] != '\n') at++;
        line_end = at;
        while (line_end > start[count] && text[line_end - 1] == '\r') line_end--;
        end[count] = line_end;
        count++;
        if (at >= length) break;
        at++;
    }

    for (boot_uint32_t index = 1; index < count; index++) {
        boot_uint32_t s = start[index];
        boot_uint32_t e = end[index];
        boot_uint32_t place = index;

        while (place) {
            const char* left = text + start[place - 1];
            boot_uint64_t left_length = end[place - 1] - start[place - 1];
            boot_uint64_t right_length = e - s;
            boot_uint64_t shortest = left_length < right_length ? left_length
                                                                : right_length;
            boot_uint64_t position = 0;
            int order = 0;

            while (position < shortest && !order) {
                char a = upper(left[position]);
                char b = upper(text[s + position]);
                if (a != b) order = a < b ? -1 : 1;
                position++;
            }
            if (!order && left_length != right_length)
                order = left_length < right_length ? -1 : 1;
            if (descending ? order >= 0 : order <= 0) break;
            start[place] = start[place - 1];
            end[place] = end[place - 1];
            place--;
        }
        start[place] = s;
        end[place] = e;
    }

    for (boot_uint32_t index = 0; index < count; index++) {
        for (boot_uint32_t position = start[index]; position < end[index]; position++)
            put(text[position]);
        print_line("");
    }
    kfree(text);
}

/* XCOPY - a directory and everything under it.
 *
 * `copy` takes files; this takes a tree, which is the difference that made it
 * a separate program in DOS and makes it one here. Directories are created as
 * they are met rather than in advance, so a failure halfway leaves what was
 * already copied rather than an empty skeleton.
 *
 * Recursion with a depth limit, because a filesystem with a loop in it is a
 * filesystem this should complain about rather than follow forever. */
#define XCOPY_DEPTH 8

static int xcopy_tree(VOLUME* from_volume, const char* from,
                      VOLUME* to_volume, const char* to, int depth,
                      boot_uint32_t* files) {
    FAT_DIRECTORY directory;
    FAT_ENTRY entry;

    if (depth > XCOPY_DEPTH) {
        print_line("Directories are nested too deeply.");
        return 0;
    }
    if (!fat32_stat(to_volume, to, &entry) &&
        !fat32_create(to_volume, to, 1, &entry)) {
        print("Could not make ");
        print_line(to);
        return 0;
    }
    if (!fat32_opendir(from_volume, from, &directory)) return 0;

    while (fat32_readdir(&directory, &entry)) {
        char source[PATH_MAX];
        char target[PATH_MAX];
        boot_uint32_t copied = 0;

        if (entry.name[0] == '.') continue;      /* . and .. */
        source[0] = 0;
        string_join(source, sizeof(source), "", from);
        string_join(source, sizeof(source), "\\", entry.name);
        target[0] = 0;
        string_join(target, sizeof(target), "", to);
        string_join(target, sizeof(target), "\\", entry.name);

        if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) {
            if (!xcopy_tree(from_volume, source, to_volume, target, depth + 1,
                            files))
                return 0;
            continue;
        }
        if (!copy_one(from_volume, source, to_volume, target, &copied)) {
            print("Could not copy ");
            print_line(source);
            return 0;
        }
        print("  ");
        print_line(target);
        (*files)++;
    }
    return 1;
}

static void command_xcopy(const ARGUMENTS* arguments) {
    VOLUME* from_volume;
    VOLUME* to_volume;
    char from[PATH_MAX];
    char to[PATH_MAX];
    FAT_ENTRY entry;
    boot_uint32_t files = 0;

    if (arguments->operand_count < 2) {
        print_line("Usage: xcopy <directory> <directory>");
        return;
    }
    if (!resolve_path(arguments->operand[0], &from_volume, from) ||
        !resolve_path(arguments->operand[1], &to_volume, to)) {
        print_line("Invalid drive.");
        return;
    }
    if (!fat32_stat(from_volume, from, &entry)) {
        print_line("Directory not found.");
        return;
    }
    if (!(entry.attributes & FAT_ATTRIBUTE_DIRECTORY)) {
        print_line("That is a file. Use copy.");
        return;
    }
    if (xcopy_tree(from_volume, from, to_volume, to, 0, &files)) {
        print_dec(files);
        print_line(" file(s) copied.");
    }
}

/* LABEL - what the volume calls itself.
 *
 * Reported here and not changed, and the difference is worth stating plainly
 * rather than pretending: the name lives in a directory entry with the volume
 * attribute set, and this filesystem has no way to write one yet. Saying so is
 * the point of the command existing at all - `vol` and `label` both answering
 * "no idea" would be worse. */
static void command_label(const ARGUMENTS* arguments) {
    VOLUME* volume = current_volume;

    if (arguments->operand_count && arguments->operand[0][1] == ':')
        volume = volume_by_letter(arguments->operand[0][0]);
    if (!volume) { print_line("Invalid drive."); return; }

    print(" Volume in drive ");
    if (volume->letter) put(volume->letter);
    else print("(hidden)");
    if (volume->label[0]) { print(" is "); print_line(volume->label); }
    else print_line(" has no label");

    if (arguments->operand_count > (volume == current_volume ? 0 : 1))
        print_line("Changing a label needs a filesystem write this does not have yet.");
}

/* MODE - what the console and the serial port are set to.
 *
 * DOS's MODE changed them. This reports them, because the one that matters -
 * the screen - is chosen by the loader before ExitBootServices and cannot be
 * changed afterwards: the protocol that would do it is gone with the firmware.
 * A command that told a comfortable lie about that would be worse than one
 * that says what it is. */
static void command_mode(void) {
    print("CON: ");
    print_dec(console_columns());
    print(" columns, ");
    print_dec(console_rows());
    print_line(" lines");
    print("     ");
    print_dec(console_width());
    put('x');
    print_dec(console_height());
    print_line(" pixels, fixed by the loader at boot");
    print_line("COM1: 115200 baud, 8-N-1, output only");
}

/* ---- The batch language --------------------------------------------------
 *
 * Enough of it to write something that decides. A file of commands in order is
 * a list; GOTO, IF and CALL are what make it a program, and the difference is
 * the whole reason AUTOEXEC.BAT and an installer can be written at all.
 */

/* PAUSE. The message is DOS's, and so is the detail that any key will do -
   including the ones that are not characters, which is why this reads an
   event rather than a line. */
static void command_pause(void) {
    print("Press any key to continue . . .");
    (void)keyboard_getchar();
    print_line("");
}

/* GOTO. Says where, and lets the loop reading the file do the going: only that
   loop knows where the lines are. `GOTO :EOF` ends the file, which is the
   spelling later DOS versions settled on and the one people write. */
static void command_goto(const ARGUMENTS* arguments) {
    const char* target = skip_spaces(arguments->tail);
    boot_uint64_t length = 0;

    if (!batch_depth) {
        print_line("GOTO is only meaningful in a batch file.");
        return;
    }
    if (*target == ':') target++;
    if (!*target) { print_line("Usage: goto label"); return; }
    if (word_is(target, "EOF")) { batch_quit = 1; return; }

    while (target[length] && !is_space(target[length]) &&
           length + 1 < BATCH_LABEL_MAX) {
        batch_target[length] = target[length];
        length++;
    }
    batch_target[length] = 0;
    batch_jump = 1;
}

/* Two words the same, ignoring case and nothing else.
 *
 * The quotes people write around the operands - IF "%A%"=="" - are not
 * special: they are compared like any other character, which is exactly what
 * makes the idiom work. An empty variable leaves "" on the left and "" was
 * written on the right, and the two match. */
static int same_text(const char* left, boot_uint64_t left_length,
                     const char* right, boot_uint64_t right_length) {
    if (left_length != right_length) return 0;
    for (boot_uint64_t index = 0; index < left_length; index++)
        if (upper(left[index]) != upper(right[index])) return 0;
    return 1;
}

/* IF, in the three shapes DOS had:
 *
 *     IF [NOT] ERRORLEVEL n  command
 *     IF [NOT] a==b          command
 *     IF [NOT] EXIST file    command
 *
 * ERRORLEVEL is "n or more", which is not an accident of implementation: a
 * program returning a larger number means something worse happened, and the
 * test is written to read that way. */
static void command_if(const char* tail) {
    const char* cursor = skip_spaces(tail);
    int negated = 0;
    int truth = 0;

    if (word_is(cursor, "NOT")) {
        negated = 1;
        while (*cursor && !is_space(*cursor)) cursor++;
        cursor = skip_spaces(cursor);
    }

    if (word_is(cursor, "ERRORLEVEL")) {
        int wanted = 0;
        int digits = 0;

        while (*cursor && !is_space(*cursor)) cursor++;
        cursor = skip_spaces(cursor);
        while (*cursor >= '0' && *cursor <= '9') {
            wanted = wanted * 10 + (*cursor++ - '0');
            digits++;
        }
        if (!digits) { print_line("IF ERRORLEVEL wants a number."); return; }
        truth = last_exit_code >= wanted;
    } else if (word_is(cursor, "EXIST")) {
        char name[PATH_MAX];
        char path[PATH_MAX];
        VOLUME* volume;
        FAT_ENTRY entry;
        boot_uint64_t length = 0;

        while (*cursor && !is_space(*cursor)) cursor++;
        cursor = skip_spaces(cursor);
        while (cursor[length] && !is_space(cursor[length]) &&
               length + 1 < PATH_MAX) {
            name[length] = cursor[length];
            length++;
        }
        name[length] = 0;
        cursor += length;
        truth = name[0] && resolve_path(name, &volume, path) &&
                fat32_stat(volume, path, &entry);
    } else {
        /* a==b. The left side runs to the `==`, so a value with a space in it
           still compares as one thing. */
        const char* left = cursor;
        const char* equals = cursor;
        const char* right;
        boot_uint64_t right_length = 0;

        while (*equals && !(equals[0] == '=' && equals[1] == '=')) equals++;
        if (!*equals) {
            print_line("IF wants ERRORLEVEL, EXIST, or a==b comparison.");
            return;
        }
        right = equals + 2;
        while (right[right_length] && !is_space(right[right_length]))
            right_length++;
        truth = same_text(left, (boot_uint64_t)(equals - left),
                          right, right_length);
        cursor = right + right_length;
    }

    if (negated) truth = !truth;

    cursor = skip_spaces(cursor);
    if (!*cursor) return;      /* a condition with nothing to do is not an error */
    if (truth) execute(cursor);
}

/* FOR %v IN (a b c) DO command
 *
 * The loop variable is an ordinary environment variable while the loop runs,
 * which is why the command inside can say %v and get what everything else
 * gets. It is removed afterwards, unless it had a value before - a loop that
 * quietly destroys somebody's variable is a loop nobody can nest.
 *
 * This is handed the line before %NAME% expansion, and it has to be. Every
 * other command sees an expanded line, but `%v` in the body must survive until
 * the loop sets it: expanded first, it would become nothing and the loop would
 * run its command with a hole in it. DOS solved the same problem by making it
 * `%%v` in batch files and `%v` at the prompt, which is a rule people get
 * wrong constantly. Both are accepted here and mean the same thing.
 */
static void command_for(const char* tail) {
    char name[ENVIRONMENT_NAME_MAX];
    char item[PATH_MAX];
    char body_line[INPUT_MAX];
    const char* cursor = skip_spaces(tail);
    const char* body;
    const char* set_end;
    boot_uint64_t length = 0;

    if (*cursor != '%') { print_line("Usage: for %v in (set) do command"); return; }
    cursor++;
    if (*cursor == '%') cursor++;          /* %%v, as written in a batch file */
    while (*cursor && !is_space(*cursor) && length + 1 < sizeof(name))
        name[length++] = *cursor++;
    name[length] = 0;
    if (!name[0]) { print_line("Usage: for %v in (set) do command"); return; }

    cursor = skip_spaces(cursor);
    if (!word_is(cursor, "IN")) { print_line("FOR wants IN after the variable."); return; }
    while (*cursor && !is_space(*cursor)) cursor++;
    cursor = skip_spaces(cursor);
    if (*cursor != '(') { print_line("FOR wants a (set) in brackets."); return; }
    cursor++;

    /* The body is found before the loop runs: the set is walked item by item
       and the DO command does not move. */
    body = cursor;
    while (*body && *body != ')') body++;
    if (!*body) { print_line("FOR is missing its closing bracket."); return; }
    set_end = body;
    body = skip_spaces(body + 1);
    if (!word_is(body, "DO")) { print_line("FOR wants DO after the set."); return; }
    while (*body && !is_space(*body)) body++;
    body = skip_spaces(body);
    if (!*body) { print_line("FOR has nothing to do."); return; }

    while (cursor < set_end) {
        boot_uint64_t out = 0;
        const char* scan;

        length = 0;
        while (cursor < set_end && (is_space(*cursor) || *cursor == ',')) cursor++;
        while (cursor < set_end && !is_space(*cursor) && *cursor != ',' &&
               length + 1 < sizeof(item))
            item[length++] = *cursor++;
        item[length] = 0;
        if (!item[0]) continue;

        /* The loop puts the item into the body itself rather than into a
         * variable for the expander to find later.
         *
         * That was the first attempt and it does not work: %%v is how a batch
         * file spells the variable, and %% is also how a line escapes a
         * percent sign - so the expander turned `%%v` into `%v` and printed
         * it, and the loop ran three times producing the same literal text.
         * FOR did the substituting in DOS too. Anything else on the line is
         * left alone and expanded afterwards, so %PATH% beside %%v still
         * works. */
        for (scan = body; *scan && out + 1 < sizeof(body_line); ) {
            const char* after = scan;
            boot_uint64_t at = 0;

            if (*after != '%') { body_line[out++] = *scan++; continue; }
            after++;
            if (*after == '%') after++;
            while (name[at] && after[at] && upper(after[at]) == upper(name[at]))
                at++;
            if (name[at] || (after[at] >= 'A' && after[at] <= 'Z') ||
                (after[at] >= 'a' && after[at] <= 'z') ||
                (after[at] >= '0' && after[at] <= '9')) {
                /* Some other name, or a longer one that merely starts the
                   same. Left for the expander. */
                body_line[out++] = *scan++;
                continue;
            }
            for (const char* value = item; *value && out + 1 < sizeof(body_line); value++)
                body_line[out++] = *value;
            scan = after + at;
        }
        body_line[out] = 0;

        execute(body_line);
        /* Ctrl+C stops the loop, not only the command inside it. A loop that
           had to be interrupted once per item would not be interruptible. */
        if (interrupted || batch_quit || batch_jump) break;
    }
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


/* Run a batch file: one line, one command.
 *
 * Nesting is refused rather than supported. A batch file that calls itself
 * would recurse on the kernel stack with nothing to stop it, and DOS needed
 * CALL for the same reason.
 *
 * `@` at the start of a line suppresses the echo, `rem` and `:` are comments
 * and labels - the three pieces of syntax worth having before variables and
 * control flow exist. */

/* Is this line the label GOTO is looking for?
 *
 * A label is a colon and a name. Anything after the name on the same line is
 * ignored, which is how `:done  the end` works and how DOS behaved. */
static int line_is_label(const char* line, const char* wanted) {
    boot_uint64_t length = 0;

    line = skip_spaces(line);
    if (*line != ':') return 0;
    line = skip_spaces(line + 1);
    while (line[length] && !is_space(line[length])) length++;
    return same_text(line, length, wanted, strlen(wanted));
}

/* %0..%9, replaced before anything else looks at the line.
 *
 * Done here rather than in environment_expand because these belong to the file
 * being run, not to the machine: two batch files running one after the other
 * have different %1 and the same PATH. A digit with no argument becomes
 * nothing, as it did in DOS - which is what makes `IF "%1"=="" goto usage`
 * the standard way to check. */
static void expand_batch_arguments(const char* input, char* output,
                                   boot_uint64_t size) {
    boot_uint64_t out = 0;

    while (*input && out + 1 < size) {
        int which;

        if (input[0] != '%' || input[1] < '0' || input[1] > '9') {
            output[out++] = *input++;
            continue;
        }
        which = input[1] - '0';
        input += 2;
        if (which >= batch_argument_count) continue;
        for (const char* value = batch_argument[which];
             *value && out + 1 < size; value++)
            output[out++] = *value;
    }
    output[out] = 0;
}

/* Run a batch file: one line at a time, with somewhere to go.
 *
 * The whole file is in memory, which is what makes GOTO possible at all: a
 * label is found by walking the lines from the start, and a file read a line
 * at a time off the disk could only ever go forwards.
 *
 * Nesting is allowed now, to a depth. CALL is the reason - a batch file that
 * calls another and carries on afterwards is how anything longer than a screen
 * gets written - and a limit rather than none because a file that calls itself
 * is a mistake somebody will make on their first afternoon, and the machine
 * should say so rather than run out of stack. */
#define BATCH_DEPTH_MAX 4

static void run_batch(VOLUME* volume, const char* path, const char* arguments) {
    FAT_ENTRY entry;
    char* contents;
    char raw[INPUT_MAX];
    char line[INPUT_MAX];
    boot_uint32_t offset = 0;
    char saved_arguments[BATCH_ARGUMENT_MAX][PATH_MAX];
    int saved_count;
    int saved_echo = batch_echo;

    if (batch_depth >= BATCH_DEPTH_MAX) {
        print_line("Batch files are nested too deeply.");
        return;
    }
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

    /* The caller's arguments are put back afterwards, so a called file cannot
       change what %1 means in the file that called it. */
    memcpy(saved_arguments, batch_argument, sizeof(saved_arguments));
    saved_count = batch_argument_count;
    {
        const char* cursor = arguments ? skip_spaces(arguments) : "";
        boot_uint64_t length = 0;

        while (path[length] && length + 1 < PATH_MAX) {
            batch_argument[0][length] = path[length];
            length++;
        }
        batch_argument[0][length] = 0;
        batch_argument_count = 1;

        while (*cursor && batch_argument_count < BATCH_ARGUMENT_MAX) {
            length = 0;
            while (*cursor && !is_space(*cursor) && length + 1 < PATH_MAX)
                batch_argument[batch_argument_count][length++] = *cursor++;
            batch_argument[batch_argument_count][length] = 0;
            if (length) batch_argument_count++;
            cursor = skip_spaces(cursor);
        }
    }

    batch_depth++;
    batch_quit = 0;
    for (boot_uint32_t index = 0; index < offset && !batch_quit;) {
        boot_uint64_t length = 0;
        int quiet = 0;
        const char* command;

        while (index < offset && contents[index] != '\n' &&
               length + 1 < INPUT_MAX) {
            char character = contents[index++];
            if (character != '\r') raw[length++] = character;
        }
        while (index < offset && contents[index] != '\n') index++;
        if (index < offset) index++;
        raw[length] = 0;

        expand_batch_arguments(raw, line, sizeof(line));

        command = skip_spaces(line);
        if (*command == '@') { quiet = 1; command = skip_spaces(command + 1); }
        if (!*command || *command == ':') continue;
        if (word_is(command, "REM")) continue;

        if (!quiet && batch_echo) {
            print_prompt();
            print_line(command);
        }
        interrupted = 0;
        execute(command);
        if (interrupted) {
            print_line("Batch job stopped.");
            break;
        }
        if (!batch_jump) continue;

        /* A GOTO. The label is looked for from the top, so it may be behind us
           - which is the only way to write a loop. */
        batch_jump = 0;
        {
            boot_uint32_t at = 0;
            int found = 0;

            while (at < offset) {
                boot_uint32_t start = at;

                while (at < offset && contents[at] != '\n') at++;
                {
                    char candidate[INPUT_MAX];
                    boot_uint64_t copied = 0;

                    while (start + copied < at && copied + 1 < INPUT_MAX) {
                        char character = contents[start + copied];
                        candidate[copied] = character == '\r' ? 0 : character;
                        copied++;
                    }
                    candidate[copied] = 0;
                    if (line_is_label(candidate, batch_target)) {
                        index = at < offset ? at + 1 : at;
                        found = 1;
                    }
                }
                if (at < offset) at++;
                if (found) break;
            }
            if (!found) {
                print("Label not found: ");
                print_line(batch_target);
                break;
            }
        }
    }
    batch_depth--;
    batch_quit = 0;
    batch_echo = saved_echo;
    memcpy(batch_argument, saved_arguments, sizeof(saved_arguments));
    batch_argument_count = saved_count;
    kfree(contents);
}

void command_execute_line(const char* line) {
    if (line) execute(line);
}

static void execute(const char* input) {
    char expanded[INPUT_MAX];
    ARGUMENTS arguments;

    /* %NAME% becomes its value before anything else looks at the line.
     *
     * Before, and not inside each command, because it has to work everywhere a
     * word can appear - a file name, a switch, the right-hand side of SET - and
     * a command that had to remember to expand its own arguments is a command
     * that will forget. This is also what makes `set PATH=%PATH%;\GAMES` mean
     * what it says. */
    /* FOR is looked at first, before expansion, and it is the only command
       that is: its loop variable has to survive until the loop sets it. See
       command_for. */
    {
        const char* raw = skip_spaces(input);
        if (word_is(raw, "FOR")) {
            while (*raw && !is_space(*raw)) raw++;
            last_exit_code = 0;
            command_for(raw);
            return;
        }
    }

    environment_expand(input, expanded, sizeof(expanded));
    input = skip_spaces(expanded);

    /* Pipes and redirection, taken off the line before the command sees it.
     *
     * Split here rather than inside each command for the same reason the
     * output is caught in console.c: a command that had to notice its own `>`
     * is a command that will not, and there would be forty of them. */
    {
        char left[INPUT_MAX];
        char right[INPUT_MAX];
        char target[PATH_MAX];
        const char* bar = 0;
        const char* arrow = 0;
        const char* less = 0;
        int append = 0;

        for (const char* scan = input; *scan; scan++) {
            if (*scan == '|' && !bar) bar = scan;
            else if (*scan == '>' && !arrow && !bar) {
                arrow = scan;
                append = scan[1] == '>';
            } else if (*scan == '<' && !less && !bar) less = scan;
        }

        if (bar) {
            /* The left side into a file, then the right side out of it. What
               DOS did, and for the same reason: nothing runs side by side. */
            boot_uint64_t length = (boot_uint64_t)(bar - input);
            int ok;

            if (length + 1 >= sizeof(left)) { print_line("Line too long."); return; }
            memcpy(left, input, length);
            left[length] = 0;
            {
                const char* rest = skip_spaces(bar + 1);
                boot_uint64_t at = 0;
                while (rest[at] && at + 1 < sizeof(right)) { right[at] = rest[at]; at++; }
                right[at] = 0;
            }
            if (!right[0]) { print_line("Nothing after the pipe."); return; }

            if (!capture_begin(PIPE_FILE, 0)) {
                print_line("Could not make the pipe file.");
                return;
            }
            execute(left);
            ok = capture_end();
            if (!ok) print_line("The pipe file could not be written.");
            else {
                char keep[PATH_MAX];
                boot_uint64_t at = 0;
                while (input_source[at]) { keep[at] = input_source[at]; at++; }
                keep[at] = 0;
                set_input_source(PIPE_FILE);
                execute(right);
                set_input_source(keep);
            }
            {
                VOLUME* volume;
                char path[PATH_MAX];
                if (resolve_path(PIPE_FILE, &volume, path))
                    fat32_remove(volume, path);
            }
            return;
        }

        if (arrow || less) {
            char command[INPUT_MAX];
            const char* first = arrow;
            boot_uint64_t length;
            boot_uint64_t at = 0;

            if (!first || (less && less < first)) first = less;
            length = (boot_uint64_t)(first - input);
            if (length + 1 >= sizeof(command)) { print_line("Line too long."); return; }
            memcpy(command, input, length);
            command[length] = 0;

            if (less) {
                const char* name = skip_spaces(less + 1);
                at = 0;
                while (name[at] && !is_space(name[at]) && name[at] != '>' &&
                       at + 1 < sizeof(target))
                    { target[at] = name[at]; at++; }
                target[at] = 0;
                set_input_source(target);
            }

            if (arrow) {
                const char* name = skip_spaces(arrow + (append ? 2 : 1));
                at = 0;
                while (name[at] && !is_space(name[at]) && at + 1 < sizeof(target))
                    { target[at] = name[at]; at++; }
                target[at] = 0;
                if (!target[0]) { print_line("Nothing to redirect into."); input_source[0] = 0; return; }
                if (!capture_begin(target, append)) {
                    print("Could not write ");
                    print_line(target);
                    input_source[0] = 0;
                    return;
                }
                execute(command);
                if (!capture_end()) {
                    print("Could not write all of ");
                    print_line(target);
                }
            } else {
                execute(command);
            }
            input_source[0] = 0;
            return;
        }
    }

    /* A built-in that has not failed leaves ERRORLEVEL at zero, as DOS did:
       a batch file testing it after `copy` should see the copy's result and
       not what a program left three lines earlier. The ones that can fail set
       it themselves. */
    last_exit_code = 0;
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
    if (word_is(input, "LAYOUT")) { command_layout(&arguments); return; }
    if (word_is(input, "DISK")) { command_disk(); return; }
    if (word_is(input, "FORMAT")) { command_format(&arguments); return; }
    if (word_is(input, "CHKDSK")) { command_chkdsk(&arguments); return; }
    if (word_is(input, "PART")) { command_part(&arguments); return; }
    if (word_is(input, "SETUP")) { command_setup(); return; }
    if (word_is(input, "SHUTDOWN")) { command_shutdown(); return; }
    if (word_is(input, "REBOOT")) { command_reboot(); return; }
    if (word_is(input, "DATE")) { command_date(); return; }
    if (word_is(input, "TIME")) { command_time(); return; }
    if (word_is(input, "VER")) { command_ver(); return; }
    if (word_is(input, "CREDITS")) { command_credits(); return; }
    /* Her name, on its own. */
    if (word_is(input, "NONCONFORMIE")) { dedication(); return; }
    if (word_is(input, "SET")) { command_set(arguments.tail); return; }
    if (word_is(input, "BEEP")) { command_beep(&arguments); return; }
    if (word_is(input, "SOUND")) { command_sound(&arguments); return; }
    if (word_is(input, "NET")) {
        if (word_is(arguments.operand[0], "START")) command_net_start();
        else if (word_is(arguments.operand[0], "SET")) command_net_set(&arguments);
        else if (word_is(arguments.operand[0], "USB")) {
            if (!usb_net_ready()) {
                print_line("No USB network device.");
            } else {
                print_line("Probing the USB network device; the result is in");
                print_line("the log, which can leave over the other wire.");
                usb_net_probe();
                print_line("Done.");
            }
        }
        else command_net();
        return;
    }
    if (word_is(input, "PING")) { command_ping(&arguments); return; }
    if (word_is(input, "DOSGET")) { command_dosget(&arguments); return; }
    if (word_is(input, "POINTER")) { command_pointer(); return; }
    if (word_is(input, "LOG")) { command_log(&arguments); return; }
    if (word_is(input, "HELP")) { command_help(); return; }
    /* `echo.` prints a blank line - the DOS idiom for one, since a bare
       `echo` prints the on/off state instead. */
    if (word_is(input, "ECHO.")) { print_line(""); return; }
    if (word_is(input, "ECHO")) {
        const char* what = skip_spaces(arguments.tail);

        /* ECHO ON and ECHO OFF are settings; everything else is text. `ECHO`
           with nothing after it reports the setting, as DOS did, rather than
           printing a blank line - which is what `ECHO.` is for. */
        if (word_is(what, "ON")) { batch_echo = 1; return; }
        if (word_is(what, "OFF")) { batch_echo = 0; return; }
        if (!*what) {
            print("ECHO is ");
            print_line(batch_echo ? "on" : "off");
            return;
        }
        print_line(arguments.tail);
        return;
    }
    if (word_is(input, "PAUSE")) { command_pause(); return; }
    if (word_is(input, "FIND")) { command_find(&arguments); return; }
    if (word_is(input, "SORT")) { command_sort(&arguments); return; }
    if (word_is(input, "XCOPY")) { command_xcopy(&arguments); return; }
    if (word_is(input, "LABEL")) { command_label(&arguments); return; }
    if (word_is(input, "MODE")) { command_mode(); return; }
    if (word_is(input, "GOTO")) { command_goto(&arguments); return; }
    if (word_is(input, "IF")) { command_if(arguments.tail); return; }
    if (word_is(input, "SHIFT")) {
        /* Everything moves down one, so a batch file can walk its arguments
           without knowing how many there are. %0 goes with the rest, which is
           what DOS did and what makes `:loop  ... shift  if not "%1"=="" goto
           loop` terminate. */
        for (int index = 0; index + 1 < batch_argument_count; index++)
            memcpy(batch_argument[index], batch_argument[index + 1], PATH_MAX);
        if (batch_argument_count) batch_argument_count--;
        return;
    }
    if (word_is(input, "CALL")) {
        const char* what = skip_spaces(arguments.tail);

        if (!*what) { print_line("Usage: call file.bat [arguments]"); return; }
        execute(what);
        return;
    }
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

/* Run whatever the last program asked to have run after it.
 *
 * A loop rather than recursion, and that is the whole reason this is a separate
 * function called from the top level. A graphical shell that runs a program and
 * asks to be brought back afterwards does that on every launch, all session; if
 * each hop were a nested call the kernel stack would grow with every program
 * the user started and the machine would fall over after a long enough evening.
 * Here each one finishes completely before the next is taken.
 *
 * There is no limit on the hops. A program that chains itself and exits loops
 * forever - but so does a program with an empty `for(;;)` in it, and the cure
 * for both is the same one. */
static void run_chained(void) {
    static char line[INPUT_MAX];

    while (program_chain_take(line, sizeof(line))) {
        serial_write("CHAIN: ");
        serial_write(line);
        serial_write("\n");
        if (line[0]) execute(line);
    }
}

/* Put the boot log on the disk without anybody asking.
 *
 * A machine with no working keyboard cannot be asked to save its own log,
 * which is exactly the machine whose log is worth having: no input, no `log`
 * command, no explanation, and a prompt nobody can type at. So the system
 * writes it itself, every boot, to the root of the volume it started from.
 *
 * Failures here are silent on purpose. This runs before the shell and is not
 * the shell's business; a read-only stick or a full disk is not a reason to
 * add a line of alarm to a screen that may be the only thing a user sees. */
static void save_boot_log(void) {
    FAT_ENTRY entry;
    const char* path = "\\KOI.LOG";

    if (!current_volume) return;
    if (fat32_stat(current_volume, path, &entry))
        fat32_remove(current_volume, path);
    if (!fat32_create(current_volume, path, 0, &entry)) return;
    (void)fat32_write(current_volume, &entry, 0, boot_log(), boot_log_length());
}


__attribute__((noreturn)) void command_run(void) {
    static char input[INPUT_MAX];

    current_volume = volume_boot();
    /* From here on, a disk appearing or going away rebuilds the volume table.
       Registered now rather than at boot because remounting relocates the
       shell, and there is no shell to relocate until there is one. */
    block_on_change(disks_changed);
    console_use_theme();
    command_ver();
    print("\n");

    /* AUTOEXEC.BAT at the root of the boot drive, if there is one. It runs
       before the check below, because on a machine with no keyboard whatever
       it prints is the only thing the system will ever say. */
    if (current_volume) {
        FAT_ENTRY entry;
        if (fat32_stat(current_volume, "\\AUTOEXEC.BAT", &entry))
            run_batch(current_volume, "\\AUTOEXEC.BAT", "");
        run_chained();
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
        /* Written after the message rather than before, so the file says this
           too. It is the whole reason the file exists. */
        save_boot_log();
        print_line("");
        print_line("The boot log has been written to KOI.LOG on this drive.");
        print_line("Read it on another machine - it says how far this got.");
        console_use_theme();
        for (;;) __asm__ volatile ("hlt");
    }

    /* And on a machine that does have a keyboard, so that a boot which went
       wrong in some other way has still left its account of itself. */
    save_boot_log();

    for (;;) {
        print_prompt();
        keyboard_read_line(input, sizeof(input));
        /* keyboard_read_line echoes to the screen only, so the line is
           repeated here for the serial log - without the prompt, which
           print_prompt() has already written to both. */
        serial_write(input);
        serial_write("\n");
        if (input[0]) execute(input);
        run_chained();
    }
}
