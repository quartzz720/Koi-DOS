#include "settings.h"

#define SETTINGS_MAX 4096

static char text[SETTINGS_MAX];
static char rebuilt[SETTINGS_MAX];
static char path[80];
static char spare[80];

/* \BOOT\CONFIG\<SECTION>.CFG, upper case because every other name on this
   filesystem is. */
static const char* path_of(const char* section) {
    long at = 0;
    const char* prefix = SETTINGS_DIRECTORY "\\";

    while (prefix[at]) { path[at] = prefix[at]; at++; }
    for (long index = 0; section[index] && at < (long)sizeof(path) - 6; index++)
        path[at++] = toupper((unsigned char)section[index]);
    path[at++] = '.'; path[at++] = 'C'; path[at++] = 'F'; path[at++] = 'G';
    path[at] = 0;
    return path;
}

static long load(const char* section) {
    long handle = koi_open(path_of(section), OPEN_READ);
    long got;

    if (handle < 0) { text[0] = 0; return 0; }
    got = koi_read(handle, text, SETTINGS_MAX - 1);
    koi_close(handle);
    if (got < 0) got = 0;
    text[got] = 0;
    return got;
}

static int is_blank(char character) {
    return character == ' ' || character == '\t';
}

/* Does this line carry `key`? The format allows spaces on either side of the
   `=`, so the comparison has to skip them rather than assume a shape. */
static const char* value_of(const char* line, long length, const char* key) {
    long at = 0;
    long index = 0;

    while (at < length && is_blank(line[at])) at++;
    if (at < length && (line[at] == '#' || line[at] == ';')) return 0;
    while (key[index]) {
        if (at >= length) return 0;
        if (toupper((unsigned char)line[at]) != toupper((unsigned char)key[index]))
            return 0;
        at++;
        index++;
    }
    while (at < length && is_blank(line[at])) at++;
    if (at >= length || line[at] != '=') return 0;
    at++;
    while (at < length && is_blank(line[at])) at++;
    return line + at;
}

int settings_get(const char* section, const char* key, char* into, long size) {
    long at = 0;

    into[0] = 0;
    load(section);
    while (text[at]) {
        long start = at;
        long length;
        const char* found;

        while (text[at] && text[at] != '\n') at++;
        length = at - start;
        if (text[at]) at++;
        /* A carriage return belongs to the line ending, not to the value. */
        while (length && (text[start + length - 1] == '\r' ||
                          is_blank(text[start + length - 1]))) length--;

        found = value_of(text + start, length, key);
        if (found) {
            long copied = 0;
            long available = length - (found - (text + start));
            while (copied < available && copied < size - 1)
                { into[copied] = found[copied]; copied++; }
            into[copied] = 0;
            return 1;
        }
    }
    return 0;
}

int settings_set(const char* section, const char* key, const char* value) {
    long at = 0;
    long out = 0;
    long handle;

    load(section);

    /* Every line that is not this key, in the order it was found. Rewriting
       the whole file is unavoidable - there is no way to change the middle of
       one - but rewriting it from what one program happens to know is not. */
    while (text[at]) {
        long start = at;
        long length;

        while (text[at] && text[at] != '\n') at++;
        length = at - start;
        if (text[at]) at++;
        while (length && text[start + length - 1] == '\r') length--;

        if (value_of(text + start, length, key)) continue;
        if (!length) continue;
        for (long index = 0; index < length && out < SETTINGS_MAX - 2; index++)
            rebuilt[out++] = text[start + index];
        rebuilt[out++] = '\n';
    }

    for (long index = 0; key[index] && out < SETTINGS_MAX - 4; index++)
        rebuilt[out++] = key[index];
    if (out < SETTINGS_MAX - 4) { rebuilt[out++] = ' '; rebuilt[out++] = '='; rebuilt[out++] = ' '; }
    for (long index = 0; value[index] && out < SETTINGS_MAX - 2; index++)
        rebuilt[out++] = value[index];
    rebuilt[out++] = '\n';
    rebuilt[out] = 0;

    /* Written beside the real one and moved on top of it when it is whole.
     *
     * The old way removed the file and made it again, which leaves a window -
     * short, but real - where the settings do not exist at all, and a longer
     * one where they exist half-written. Losing power in either is losing the
     * settings. A rename cannot be half-done: the directory entry either names
     * the new file or the old one, and there is no third answer.
     *
     * This is not a journalled filesystem and nothing here pretends otherwise.
     * What it buys is that the failure is "the change did not happen" rather
     * than "the file is now rubbish", and those are very different mornings. */
    koi_mkdir(SETTINGS_DIRECTORY);

    {
        long at = 0;
        const char* live = path_of(section);
        while (live[at] && at < (long)sizeof(spare) - 1) { spare[at] = live[at]; at++; }
        spare[at] = 0;
        /* Same name, different extension: it lands in the same directory, so
           the rename never crosses a device. */
        if (at > 4) { spare[at-3] = 'T'; spare[at-2] = 'M'; spare[at-1] = 'P'; }
    }

    koi_remove(spare);
    handle = koi_open(spare, OPEN_WRITE);
    if (handle < 0) return 0;
    if (koi_write(handle, rebuilt, out) != out) { koi_close(handle); return 0; }
    koi_close(handle);

    koi_remove(path_of(section));
    if (koi_rename(spare, path_of(section)) < 0) {
        /* The new file is written and could not be put in place. Say so rather
           than leaving the caller believing a setting stuck. */
        return 0;
    }
    return 1;
}
