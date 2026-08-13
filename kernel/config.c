#include "config.h"
#include "audio.h"
#include "layout.h"
#include "console.h"
#include "fat32.h"
#include "heap.h"
#include "serial.h"
#include "string.h"
#include "environment.h"

#define LINE_MAX 128
#define PROGRAM_PATH_MAX 256

static const char* color_names[16] = {
    "black", "blue", "green", "cyan",
    "red", "magenta", "brown", "lightgray",
    "darkgray", "lightblue", "lightgreen", "lightcyan",
    "lightred", "lightmagenta", "yellow", "white"
};

/* The search path lives in the environment now, not in a buffer here.
 *
 * There were two answers to "where are programs looked for" - this file's, and
 * whatever `SET PATH=` would have meant - and two answers is how they end up
 * disagreeing. The environment is the one, because it is the one a person can
 * see and change; this file seeds it at boot and writes it back when dosget
 * changes it, which is what a settings file is for.
 *
 * The default is what a machine has before anybody has said otherwise: the two
 * packages that were once part of the system. */
#define PROGRAM_PATH_DEFAULT "\\COMMANDER;\\MIZU"

static int equals_ignoring_case(const char* left, const char* right) {
    while (*left && *right) {
        char a = *left >= 'A' && *left <= 'Z' ? (char)(*left + 32) : *left;
        char b = *right >= 'A' && *right <= 'Z' ? (char)(*right + 32) : *right;
        if (a != b) return 0;
        left++;
        right++;
    }
    return !*left && !*right;
}

int config_parse_color(const char* text) {
    int value = 0;
    int digits = 0;

    if (!text || !*text) return -1;

    for (int index = 0; index < 16; index++)
        if (equals_ignoring_case(text, color_names[index])) return index;

    /* Numbers too, because 0-15 is how the DOS `color` command spelt it and
       some people will reach for that first. */
    for (const char* cursor = text; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') return -1;
        value = value * 10 + (*cursor - '0');
        digits++;
    }
    if (!digits || value > 15) return -1;
    return value;
}

const char* config_color_name(int color) {
    return (color >= 0 && color < 16) ? color_names[color] : "?";
}

static void trim(char* text) {
    boot_uint64_t length = strlen(text);
    boot_uint64_t start = 0;

    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                      text[length - 1] == '\r')) text[--length] = 0;
    while (text[start] == ' ' || text[start] == '\t') start++;
    if (start) {
        boot_uint64_t index = 0;
        while (text[start + index]) { text[index] = text[start + index]; index++; }
        text[index] = 0;
    }
}

const char* config_program_path(void) {
    /* Whatever the environment says, including nothing.
     *
     * The default above is a seed, applied once at boot, and not a value this
     * falls back to - otherwise `set PATH=` would quietly restore two
     * directories nobody asked for, and the one command whose whole meaning is
     * "look nowhere else" would not do it. */
    const char* value = environment_get("PATH");
    return value ? value : "";
}

/* Is `directory` already one of the entries in the search path?
 *
 * Entry by entry rather than a substring search: `\GAME` is not `\GAMES`, and
 * a substring test says it is. Case-insensitive because the path is written by
 * people and read by a filesystem that does not care. */
static int path_contains(const char* directory) {
    const char* cursor = config_program_path();

    while (*cursor) {
        const char* entry;
        boot_uint64_t length = 0;

        while (*cursor == ';' || *cursor == ' ' || *cursor == '\t') cursor++;
        entry = cursor;
        while (*cursor && *cursor != ';') cursor++;
        length = (boot_uint64_t)(cursor - entry);
        while (length && (entry[length - 1] == ' ' || entry[length - 1] == '\t'))
            length--;

        if (length == strlen(directory)) {
            boot_uint64_t index = 0;
            while (index < length) {
                char a = entry[index];
                char b = directory[index];
                if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                if (a != b) break;
                index++;
            }
            if (index == length) return 1;
        }
        if (*cursor == ';') cursor++;
    }
    return 0;
}

/* Write the file out with one key replaced and everything else kept.
 *
 * Kept, because this file is not ours alone: the language lives in it too, and
 * a writer that rebuilds a settings file from what it happens to know destroys
 * whatever it does not. That mistake has already been made here once, when
 * every program shared one file and the second writer erased the first.
 *
 * Beside the real file and renamed on top of it when it is whole. A rename
 * cannot be half-done: the directory entry names the new file or the old one,
 * and there is no third answer. This is not a journalled filesystem and
 * nothing here pretends otherwise; what it buys is that losing power gives
 * "the change did not happen" rather than "the settings are now rubbish". */
static int write_setting(VOLUME* volume, const char* path, const char* key,
                         const char* value) {
    static char rebuilt[2048];
    char temporary[80];
    FAT_ENTRY entry;
    boot_uint64_t out = 0;
    boot_uint64_t length;

    /* Everything already there except the line being replaced. */
    if (fat32_stat(volume, path, &entry) &&
        !(entry.attributes & FAT_ATTRIBUTE_DIRECTORY) &&
        entry.size && entry.size < sizeof(rebuilt) / 2) {
        static char existing[1024];
        boot_uint32_t offset = 0;
        boot_uint32_t index = 0;

        while (offset < entry.size) {
            boot_uint32_t got = fat32_read(volume, &entry, offset,
                                           existing + offset,
                                           entry.size - offset);
            if (!got) break;
            offset += got;
        }
        existing[offset] = 0;

        while (index < offset) {
            boot_uint32_t start = index;
            boot_uint32_t end;
            char line[LINE_MAX];
            boot_uint64_t at = 0;
            char* separator = (char*)0;

            while (index < offset && existing[index] != '\n') index++;
            end = index;
            if (index < offset) index++;
            while (end > start && existing[end - 1] == '\r') end--;

            while (start + at < end && at + 1 < LINE_MAX)
                { line[at] = existing[start + at]; at++; }
            line[at] = 0;

            /* Only a line that sets this very key is dropped. A comment that
               mentions it, or a line whose value happens to look like it, is
               somebody's and stays. Same rule about comments as above: the
               whole line, decided by the first thing on it. */
            {
                boot_uint64_t first = 0;
                while (line[first] == ' ' || line[first] == '\t') first++;
                if (line[first] != '#' && line[first] != ';') {
                    for (boot_uint64_t position = 0; line[position]; position++)
                        if (line[position] == '=')
                            { separator = &line[position]; break; }
                }
            }
            if (separator) {
                char name[LINE_MAX];
                boot_uint64_t copied = 0;

                while (copied < (boot_uint64_t)(separator - line) &&
                       copied + 1 < LINE_MAX)
                    { name[copied] = line[copied]; copied++; }
                name[copied] = 0;
                trim(name);
                if (equals_ignoring_case(name, key)) continue;
            }

            for (boot_uint64_t copied = 0; line[copied] && out + 2 < sizeof(rebuilt);
                 copied++)
                rebuilt[out++] = line[copied];
            rebuilt[out++] = '\n';
        }
    }

    for (boot_uint64_t index = 0; key[index] && out + 4 < sizeof(rebuilt); index++)
        rebuilt[out++] = key[index];
    if (out + 4 < sizeof(rebuilt))
        { rebuilt[out++] = ' '; rebuilt[out++] = '='; rebuilt[out++] = ' '; }
    for (boot_uint64_t index = 0; value[index] && out + 2 < sizeof(rebuilt); index++)
        rebuilt[out++] = value[index];
    rebuilt[out++] = '\n';

    /* The directory may not exist on a machine that has never had a setting
       written; a directory that is already there is not an error. */
    if (!fat32_stat(volume, CONFIG_DIRECTORY, &entry))
        fat32_create(volume, CONFIG_DIRECTORY, 1, &entry);

    length = strlen(path);
    if (length + 1 >= sizeof(temporary) || length < 4) return 0;
    for (boot_uint64_t index = 0; index <= length; index++)
        temporary[index] = path[index];
    /* The same name with a different extension, so the rename stays inside one
       directory and therefore inside one filesystem. */
    temporary[length - 3] = 'T';
    temporary[length - 2] = 'M';
    temporary[length - 1] = 'P';

    if (fat32_stat(volume, temporary, &entry)) fat32_remove(volume, temporary);
    if (!fat32_create(volume, temporary, 0, &entry)) return 0;
    if (fat32_write(volume, &entry, 0, rebuilt, (boot_uint32_t)out) !=
        (boot_uint32_t)out) {
        fat32_remove(volume, temporary);
        return 0;
    }
    if (fat32_stat(volume, path, &entry)) fat32_remove(volume, path);
    if (fat32_rename(volume, temporary, path) < 0) {
        fat32_remove(volume, temporary);
        return 0;
    }
    return 1;
}

int config_add_program_path(VOLUME* volume, const char* directory) {
    char rebuilt[PROGRAM_PATH_MAX];
    const char* current = config_program_path();
    boot_uint64_t length = strlen(current);
    boot_uint64_t extra = strlen(directory ? directory : "");

    if (!directory || !directory[0]) return 0;
    if (path_contains(directory)) return 1;

    /* No room is not a reason to write half a path: the file would then name a
       directory that does not exist, forever. */
    if (length + extra + 2 >= PROGRAM_PATH_MAX) return 0;

    for (boot_uint64_t index = 0; index < length; index++)
        rebuilt[index] = current[index];
    if (length) rebuilt[length++] = ';';
    for (boot_uint64_t index = 0; index < extra; index++)
        rebuilt[length++] = directory[index];
    rebuilt[length] = 0;

    if (!environment_set("PATH", rebuilt)) return 0;
    if (!volume) return 1;   /* on the path now, but not after a reboot */
    return write_setting(volume, CONFIG_DIRECTORY "\\SYSTEM.CFG", "path",
                         rebuilt);
}

int config_remove_program_path(VOLUME* volume, const char* directory) {
    char rebuilt[PROGRAM_PATH_MAX];
    boot_uint64_t out = 0;
    const char* cursor = config_program_path();
    int removed = 0;

    if (!directory || !directory[0]) return 0;
    if (!path_contains(directory)) return 1;

    /* Kept entry by entry rather than cut out of the middle: the separators
       have to come out with it, and a path that ends in a `;` names an entry
       with no directory in it. */
    while (*cursor) {
        const char* entry;
        boot_uint64_t length;

        while (*cursor == ';' || *cursor == ' ' || *cursor == '\t') cursor++;
        entry = cursor;
        while (*cursor && *cursor != ';') cursor++;
        length = (boot_uint64_t)(cursor - entry);
        while (length && (entry[length - 1] == ' ' || entry[length - 1] == '\t'))
            length--;
        if (*cursor == ';') cursor++;
        if (!length) continue;

        if (length == strlen(directory)) {
            boot_uint64_t index = 0;
            while (index < length) {
                char a = entry[index];
                char b = directory[index];
                if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                if (a != b) break;
                index++;
            }
            if (index == length) { removed = 1; continue; }
        }

        if (out && out + 1 < sizeof(rebuilt)) rebuilt[out++] = ';';
        for (boot_uint64_t index = 0; index < length && out + 1 < sizeof(rebuilt);
             index++)
            rebuilt[out++] = entry[index];
    }
    rebuilt[out] = 0;
    if (!removed) return 1;
    if (!environment_set("PATH", rebuilt)) return 0;

    if (!volume) return 1;
    return write_setting(volume, CONFIG_DIRECTORY "\\SYSTEM.CFG", "path",
                         rebuilt);
}

static void apply_console(const char* key, const char* value, void* context) {
    CONSOLE_THEME* theme = (CONSOLE_THEME*)context;
    int color = config_parse_color(value);
    if (color < 0) return;

    if (equals_ignoring_case(key, "foreground")) theme->foreground = (boot_uint8_t)color;
    else if (equals_ignoring_case(key, "background")) theme->background = (boot_uint8_t)color;
    else if (equals_ignoring_case(key, "prompt")) theme->prompt = (boot_uint8_t)color;
    else if (equals_ignoring_case(key, "error")) theme->error = (boot_uint8_t)color;
    /* Anything else is ignored on purpose: a file written by a newer program
       must not stop an older kernel from booting. */
}

/* The second layout follows the language the machine was set to. Somebody who
   chose Ukrainian at setup wants Ukrainian and English on Alt+Shift, and
   having to say so twice is the kind of question software asks when it has not
   been thought about. */
static void apply_system(const char* key, const char* value, void* context) {
    (void)context;

    if (equals_ignoring_case(key, "language")) {
        if (value[0] == 'r' && value[1] == 'u') layout_set_alternate(LAYOUT_RU);
        else if (value[0] == 'u' && value[1] == 'k') layout_set_alternate(LAYOUT_UK);
        else if (value[0] == 'e' && value[1] == 'l') layout_set_alternate(LAYOUT_GR);
    } else if (equals_ignoring_case(key, "path")) {
        char text[PROGRAM_PATH_MAX];
        boot_uint64_t length = 0;

        while (value[length] && length + 1 < PROGRAM_PATH_MAX) {
            text[length] = value[length];
            length++;
        }
        text[length] = 0;
        trim(text);
        environment_set("PATH", text);
    }
}

static void apply_sound(const char* key, const char* value, void* context) {
    int* percent = (int*)context;
    int total = 0;

    if (!equals_ignoring_case(key, "volume")) return;
    for (const char* cursor = value; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') return;
        total = total * 10 + (*cursor - '0');
    }
    if (total <= 100) *percent = total;
}

/* One settings file, read and handed to `apply` a key at a time. Returns 0
   when there is no such file, which is not an error anywhere here. */
static int read_settings(VOLUME* volume, const char* path,
                         void (*apply)(const char*, const char*, void*),
                         void* context) {
    FAT_ENTRY entry;
    char* contents;
    boot_uint32_t offset = 0;
    boot_uint32_t index = 0;

    if (!volume) return 0;
    if (!fat32_stat(volume, path, &entry)) return 0;        /* absent is fine */
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return 0;
    if (!entry.size || entry.size > 8192) return 0;

    contents = (char*)kmalloc(entry.size + 1);
    if (!contents) return 0;
    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset,
                                       contents + offset, entry.size - offset);
        if (!got) break;
        offset += got;
    }
    contents[offset] = 0;

    while (index < offset) {
        char line[LINE_MAX];
        boot_uint64_t length = 0;
        char* separator = (char*)0;

        while (index < offset && contents[index] != '\n' && length + 1 < LINE_MAX)
            line[length++] = contents[index++];
        while (index < offset && contents[index] != '\n') index++;
        if (index < offset) index++;
        line[length] = 0;

        /* A comment is a whole line, marked by the first thing on it.
         *
         * It used to be any `#` or `;` anywhere, which cut the line short
         * there - and `;` is what separates the entries of the program search
         * path. So `path = \COMMANDER;\MIZU` was read as `\COMMANDER` and
         * everything after the first entry silently disappeared, one boot
         * after it was written. Nothing complained: a path with one entry is a
         * perfectly good path, and the programs in the others were simply not
         * found any more.
         *
         * Trailing comments after a value are gone with it, which is the same
         * rule the program side has always used. */
        {
            boot_uint64_t first = 0;
            while (line[first] == ' ' || line[first] == '\t') first++;
            if (line[first] == '#' || line[first] == ';') continue;
        }
        for (boot_uint64_t position = 0; line[position]; position++)
            if (line[position] == '=') { separator = &line[position]; break; }
        if (!separator) continue;

        *separator = 0;
        trim(line);
        trim(separator + 1);
        apply(line, separator + 1, context);
    }

    kfree(contents);
    return 1;
}

/* Settings, as a directory of small files with one owner each.
 *
 * It was one file that every program rewrote from what that program knew
 * about, and the second writer destroyed the first one's keys - the commander
 * recorded that it had asked its questions, somebody changed a colour, and the
 * machine asked them again. A rule saying "read it, change one line, write it
 * back" would have worked and would have had to be remembered by everybody
 * forever. Two programs that never open the same file cannot collide at all,
 * and that is a property of the arrangement rather than of anybody's care.
 *
 * Still plain text, still one `key = value` per line: a DOS-like system whose
 * settings need a special program to read them would have got the wrong half
 * of the idea.
 *
 * The old single file is still read first, so a machine that has one keeps its
 * colours; anything in the new files wins over it. */
void config_load(VOLUME* volume) {
    CONSOLE_THEME theme;
    int percent = -1;

    if (!volume) return;
    theme = *console_theme();

    read_settings(volume, CONFIG_LEGACY_PATH, apply_console, &theme);
    read_settings(volume, CONFIG_DIRECTORY "\\CONSOLE.CFG", apply_console, &theme);
    read_settings(volume, CONFIG_DIRECTORY "\\SOUND.CFG", apply_sound, &percent);
    read_settings(volume, CONFIG_DIRECTORY "\\SYSTEM.CFG", apply_system,
                  (void*)0);

    if (percent >= 0) audio_set_volume(percent * 255 / 100);

    {
        const CONSOLE_THEME* current = console_theme();
        int changed = theme.foreground != current->foreground ||
                      theme.background != current->background ||
                      theme.prompt != current->prompt ||
                      theme.error != current->error;

        console_set_theme(&theme);
        console_use_theme();
        /* Repaint when the theme actually changed. The boot messages above
           were printed before this file could be read, so without a clear the
           screen ends up half in the old colours and half in the new - which
           reads as a fault rather than a setting. Nothing is lost: the same
           messages went to the serial port. */
        if (changed) console_clear();
    }
    /* A machine that has never had a path written still has one, and `set`
       should show it. Seeded rather than left implicit: a variable that governs
       where programs are found and does not appear in the list is a variable
       somebody will spend an afternoon looking for. */
    if (!environment_get("PATH")) environment_set("PATH", PROGRAM_PATH_DEFAULT);

    serial_write("CONFIG: settings applied\n");
}
