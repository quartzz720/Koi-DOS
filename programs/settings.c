#include "settings.h"

#define SETTINGS_MAX 4096

static char text[SETTINGS_MAX];
static char rebuilt[SETTINGS_MAX];
static char path[80];

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

    /* Removed and remade rather than written over: a shorter file written into
       a longer one leaves the tail of the old one behind. */
    /* The directory may not exist yet; already there is not an error. */
    koi_mkdir(SETTINGS_DIRECTORY);
    koi_remove(path_of(section));
    handle = koi_open(path_of(section), OPEN_WRITE);
    if (handle < 0) return 0;
    if (koi_write(handle, rebuilt, out) != out) { koi_close(handle); return 0; }
    koi_close(handle);
    return 1;
}
