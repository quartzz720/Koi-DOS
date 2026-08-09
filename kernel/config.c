#include "config.h"
#include "audio.h"
#include "layout.h"
#include "console.h"
#include "fat32.h"
#include "heap.h"
#include "serial.h"
#include "string.h"

#define LINE_MAX 128

static const char* color_names[16] = {
    "black", "blue", "green", "cyan",
    "red", "magenta", "brown", "lightgray",
    "darkgray", "lightblue", "lightgreen", "lightcyan",
    "lightred", "lightmagenta", "yellow", "white"
};

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
static void apply_language(const char* key, const char* value, void* context) {
    (void)context;
    if (!equals_ignoring_case(key, "language")) return;
    if (value[0] == 'r' && value[1] == 'u') layout_set_alternate(LAYOUT_RU);
    else if (value[0] == 'u' && value[1] == 'k') layout_set_alternate(LAYOUT_UK);
    else if (value[0] == 'e' && value[1] == 'l') layout_set_alternate(LAYOUT_GR);
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

        for (boot_uint64_t position = 0; line[position]; position++) {
            if (line[position] == '#' || line[position] == ';') {
                line[position] = 0;
                break;
            }
            if (line[position] == '=' && !separator) separator = &line[position];
        }
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
    read_settings(volume, CONFIG_DIRECTORY "\\SYSTEM.CFG", apply_language,
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
    serial_write("CONFIG: settings applied\n");
}
