#include "string.h"

void* memset(void* destination, int value, boot_uint64_t size) {
    boot_uint8_t* bytes = (boot_uint8_t*)destination;
    for (boot_uint64_t i = 0; i < size; i++) bytes[i] = (boot_uint8_t)value;
    return destination;
}

void* memcpy(void* destination, const void* source, boot_uint64_t size) {
    boot_uint8_t* to = (boot_uint8_t*)destination;
    const boot_uint8_t* from = (const boot_uint8_t*)source;
    for (boot_uint64_t i = 0; i < size; i++) to[i] = from[i];
    return destination;
}

void* memmove(void* destination, const void* source, boot_uint64_t size) {
    boot_uint8_t* to = (boot_uint8_t*)destination;
    const boot_uint8_t* from = (const boot_uint8_t*)source;

    /* Copy backwards when the regions overlap with the destination above the
       source. The console scroller relies on this. */
    if (to > from && to < from + size) {
        for (boot_uint64_t i = size; i > 0; i--) to[i - 1] = from[i - 1];
        return destination;
    }
    for (boot_uint64_t i = 0; i < size; i++) to[i] = from[i];
    return destination;
}

int memcmp(const void* left, const void* right, boot_uint64_t size) {
    const boot_uint8_t* a = (const boot_uint8_t*)left;
    const boot_uint8_t* b = (const boot_uint8_t*)right;
    for (boot_uint64_t i = 0; i < size; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

boot_uint64_t strlen(const char* text) {
    boot_uint64_t length = 0;
    while (text[length]) length++;
    return length;
}

int strcmp(const char* left, const char* right) {
    while (*left && *left == *right) { left++; right++; }
    return (int)(boot_uint8_t)*left - (int)(boot_uint8_t)*right;
}

static char to_upper(char character) {
    return character >= 'a' && character <= 'z' ? (char)(character - 32) : character;
}

/* Glob matching, case insensitive.
 *
 * `*` matches any run of characters and `?` exactly one. DOS gave `*` a
 * narrower meaning - it padded the 8.3 field with question marks, so `*.TXT`
 * was really `????????.TXT` and could not match a name with two dots. With
 * long names that reading only surprises people, so `*` here matches dots too,
 * the way every shell since has behaved.
 *
 * Iterative rather than recursive: a pattern of alternating stars against a
 * long name would nest a recursive matcher hundreds deep, and this runs on a
 * kernel stack. */
int glob_match(const char* pattern, const char* name) {
    const char* star = (const char*)0;
    const char* retry = name;

    while (*name) {
        char p = to_upper(*pattern);
        char n = to_upper(*name);

        if (p == '?' || (p && p == n)) {
            pattern++;
            name++;
        } else if (p == '*') {
            /* Remember where to resume if the rest fails to line up. */
            star = ++pattern;
            retry = name;
        } else if (star) {
            pattern = star;
            name = ++retry;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return !*pattern;
}

