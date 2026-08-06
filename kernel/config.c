#include "config.h"
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

static void apply(const char* key, const char* value, CONSOLE_THEME* theme) {
    int color = config_parse_color(value);
    if (color < 0) return;

    if (equals_ignoring_case(key, "foreground")) theme->foreground = (boot_uint8_t)color;
    else if (equals_ignoring_case(key, "background")) theme->background = (boot_uint8_t)color;
    else if (equals_ignoring_case(key, "prompt")) theme->prompt = (boot_uint8_t)color;
    else if (equals_ignoring_case(key, "error")) theme->error = (boot_uint8_t)color;
    /* Anything else is ignored on purpose: a file written by a newer program
       must not stop an older kernel from booting. */
}

void config_load(VOLUME* volume) {
    FAT_ENTRY entry;
    CONSOLE_THEME theme;
    char* contents;
    boot_uint32_t offset = 0;
    boot_uint32_t index = 0;

    if (!volume) return;
    if (!fat32_stat(volume, CONFIG_PATH, &entry)) return;   /* absent is fine */
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return;
    if (!entry.size || entry.size > 8192) return;

    contents = (char*)kmalloc(entry.size + 1);
    if (!contents) return;
    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset,
                                       contents + offset, entry.size - offset);
        if (!got) break;
        offset += got;
    }
    contents[offset] = 0;

    theme = *console_theme();

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
        apply(line, separator + 1, &theme);
    }

    kfree(contents);

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
    serial_write("CONFIG: userspace.cfg applied\n");
}
