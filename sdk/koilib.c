#include "koi.h"

/* The parts of a C library a program cannot do without.
 *
 * Not a C library, and not trying to be one. This is the subset that either
 * the compiler requires or that any program larger than a page of code
 * rewrites badly on its own: the memory and string primitives, character
 * classification, number conversion, one formatter, and a heap.
 *
 * The memory ones are not optional. GCC emits calls to memcpy, memset,
 * memmove and memcmp of its own accord even with -ffreestanding - a struct
 * assignment is enough - so a program without them fails to link for reasons
 * that have nothing to do with what it wrote.
 */

/* ---- Memory ------------------------------------------------------------- */

void* memset(void* destination, int value, koi_uint64 count) {
    koi_uint8* out = (koi_uint8*)destination;
    while (count--) *out++ = (koi_uint8)value;
    return destination;
}

void* memcpy(void* destination, const void* source, koi_uint64 count) {
    koi_uint8* out = (koi_uint8*)destination;
    const koi_uint8* in = (const koi_uint8*)source;
    while (count--) *out++ = *in++;
    return destination;
}

void* memmove(void* destination, const void* source, koi_uint64 count) {
    koi_uint8* out = (koi_uint8*)destination;
    const koi_uint8* in = (const koi_uint8*)source;

    /* Backwards when the regions overlap the wrong way round, or the copy
       would overwrite bytes it has not read yet. */
    if (out > in && out < in + count) {
        out += count;
        in += count;
        while (count--) *--out = *--in;
        return destination;
    }
    while (count--) *out++ = *in++;
    return destination;
}

int memcmp(const void* left, const void* right, koi_uint64 count) {
    const koi_uint8* a = (const koi_uint8*)left;
    const koi_uint8* b = (const koi_uint8*)right;

    while (count--) {
        if (*a != *b) return (int)*a - (int)*b;
        a++;
        b++;
    }
    return 0;
}

/* ---- Strings ------------------------------------------------------------ */

koi_uint64 strlen(const char* text) {
    koi_uint64 length = 0;
    while (text[length]) length++;
    return length;
}

char* strcpy(char* destination, const char* source) {
    char* start = destination;
    while ((*destination++ = *source++)) { }
    return start;
}

char* strncpy(char* destination, const char* source, koi_uint64 count) {
    koi_uint64 index = 0;
    while (index < count && source[index]) { destination[index] = source[index]; index++; }
    /* The standard pads with zeroes rather than terminating, and code that
       relies on it exists, so this does too. */
    while (index < count) destination[index++] = 0;
    return destination;
}

char* strcat(char* destination, const char* source) {
    char* end = destination + strlen(destination);
    while ((*end++ = *source++)) { }
    return destination;
}

int strcmp(const char* left, const char* right) {
    while (*left && *left == *right) { left++; right++; }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char* left, const char* right, koi_uint64 count) {
    while (count && *left && *left == *right) { left++; right++; count--; }
    if (!count) return 0;
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char* strchr(const char* text, int character) {
    for (; *text; text++)
        if (*text == (char)character) return (char*)text;
    return (char)character ? (char*)0 : (char*)text;
}

char* strrchr(const char* text, int character) {
    const char* found = (const char*)0;
    for (; *text; text++)
        if (*text == (char)character) found = text;
    if (!(char)character) return (char*)text;
    return (char*)found;
}

char* strstr(const char* haystack, const char* needle) {
    koi_uint64 length = strlen(needle);

    if (!length) return (char*)haystack;
    for (; *haystack; haystack++)
        if (!strncmp(haystack, needle, length)) return (char*)haystack;
    return (char*)0;
}

/* ---- Characters --------------------------------------------------------- */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isprint(int c) { return c >= 0x20 && c < 0x7F; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

/* ---- Numbers ------------------------------------------------------------ */

int abs(int value) { return value < 0 ? -value : value; }

long strtol(const char* text, char** end, int base) {
    long value = 0;
    int negative = 0;

    while (isspace((int)(unsigned char)*text)) text++;
    if (*text == '-') { negative = 1; text++; }
    else if (*text == '+') text++;

    if (!base) {
        /* The standard's guess: 0x is hexadecimal, a leading 0 is octal. */
        if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
        else if (text[0] == '0') { base = 8; text++; }
        else base = 10;
    } else if (base == 16 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }

    for (;;) {
        int digit;
        if (isdigit((int)(unsigned char)*text)) digit = *text - '0';
        else if (isalpha((int)(unsigned char)*text)) digit = tolower((int)(unsigned char)*text) - 'a' + 10;
        else break;
        if (digit >= base) break;
        value = value * base + digit;
        text++;
    }

    if (end) *end = (char*)text;
    return negative ? -value : value;
}

int atoi(const char* text) {
    return (int)strtol(text, (char**)0, 10);
}

/* ---- Formatting ---------------------------------------------------------
 *
 * One formatter, which everything else calls. The conversions are the ones
 * that actually appear in the programs this has to build - d, i, u, x, X, c,
 * s, p and %% - with width, left alignment and zero padding. `%f` is absent on
 * purpose: there is no floating point here at all, so a program cannot have
 * produced a value to print.
 */

typedef struct {
    char* out;          /* where the next character goes, or NULL to count */
    koi_uint64 room;    /* how many more may be written */
    koi_uint64 written; /* how many would have been, room or not */
} SINK;

static void emit(SINK* sink, char character) {
    sink->written++;
    if (!sink->out || !sink->room) return;
    *sink->out++ = character;
    sink->room--;
}

static void emit_padded(SINK* sink, const char* text, koi_uint64 length,
                        int width, int left, char pad) {
    koi_uint64 fill = (koi_uint64)width > length ? (koi_uint64)width - length : 0;

    if (left) {
        while (length--) emit(sink, *text++);
        /* Padding on the right is always spaces: zeroes after a number would
           multiply it by ten. */
        while (fill--) emit(sink, ' ');
    } else {
        while (fill--) emit(sink, pad);
        while (length--) emit(sink, *text++);
    }
}

static koi_uint64 render_unsigned(koi_uint64 value, int base, int upper,
                                  char* buffer) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char temporary[24];
    koi_uint64 length = 0;

    do {
        temporary[length++] = digits[value % (koi_uint64)base];
        value /= (koi_uint64)base;
    } while (value);

    for (koi_uint64 index = 0; index < length; index++)
        buffer[index] = temporary[length - 1 - index];
    return length;
}

/* Grow a rendered number to the precision asked for by pushing zeroes in
   front of it. Returns the new length; leaves the digits alone when the
   precision is absent or already met. */
static koi_uint64 apply_precision(char* digits, koi_uint64 length,
                                  int precision) {
    koi_uint64 wanted;

    if (precision < 0) return length;
    wanted = (koi_uint64)precision;
    if (wanted <= length) return length;
    for (koi_uint64 index = length; index > 0; index--)
        digits[index - 1 + (wanted - length)] = digits[index - 1];
    for (koi_uint64 index = 0; index < wanted - length; index++)
        digits[index] = '0';
    return wanted;
}

int koi_vformat(char* out, koi_uint64 size, const char* format,
                __builtin_va_list arguments) {
    SINK sink;
    char number[32];

    sink.out = out;
    sink.room = size ? size - 1 : 0;
    sink.written = 0;

    for (; *format; format++) {
        int left = 0;
        int width = 0;
        int precision = -1;      /* -1 means none was given */
        char pad = ' ';
        int is_long = 0;

        if (*format != '%') { emit(&sink, *format); continue; }
        format++;
        if (!*format) break;
        if (*format == '%') { emit(&sink, '%'); continue; }

        for (;;) {
            if (*format == '-') { left = 1; format++; continue; }
            if (*format == '0') { pad = '0'; format++; continue; }
            break;
        }
        while (isdigit((int)(unsigned char)*format))
            width = width * 10 + (*format++ - '0');
        /* Precision. On a number it is the least number of digits, zero
           filled; on a string it is the most characters to take. DOOM builds
           its font lump names with "%.3d" and gets STCFN033, so a formatter
           without this produces a name no WAD has ever contained. */
        if (*format == '.') {
            format++;
            precision = 0;
            while (isdigit((int)(unsigned char)*format))
                precision = precision * 10 + (*format++ - '0');
        }
        while (*format == 'l' || *format == 'h') {
            if (*format == 'l') is_long = 1;
            format++;
        }

        switch (*format) {
        case 'd': case 'i': {
            long value = is_long ? __builtin_va_arg(arguments, long)
                                 : (long)__builtin_va_arg(arguments, int);
            koi_uint64 length;
            int negative = value < 0;
            koi_uint64 magnitude = (koi_uint64)(negative ? -value : value);

            length = render_unsigned(magnitude, 10, 0, number + 1);
            length = apply_precision(number + 1, length, precision);
            if (negative) {
                number[0] = '-';
                emit_padded(&sink, number, length + 1, width, left,
                            precision >= 0 ? ' ' : pad);
            } else {
                emit_padded(&sink, number + 1, length, width, left,
                            precision >= 0 ? ' ' : pad);
            }
            break;
        }
        case 'u': {
            koi_uint64 value = is_long ? __builtin_va_arg(arguments, unsigned long)
                                       : (koi_uint64)__builtin_va_arg(arguments, unsigned int);
            emit_padded(&sink, number,
                        apply_precision(number,
                                        render_unsigned(value, 10, 0, number),
                                        precision),
                        width, left, precision >= 0 ? ' ' : pad);
            break;
        }
        case 'x': case 'X': {
            koi_uint64 value = is_long ? __builtin_va_arg(arguments, unsigned long)
                                       : (koi_uint64)__builtin_va_arg(arguments, unsigned int);
            emit_padded(&sink, number,
                        apply_precision(number,
                                        render_unsigned(value, 16,
                                                        *format == 'X', number),
                                        precision),
                        width, left, precision >= 0 ? ' ' : pad);
            break;
        }
        case 'p': {
            koi_uint64 value = (koi_uint64)__builtin_va_arg(arguments, void*);
            emit(&sink, '0');
            emit(&sink, 'x');
            emit_padded(&sink, number, render_unsigned(value, 16, 0, number),
                        0, 0, ' ');
            break;
        }
        case 'c': {
            char character = (char)__builtin_va_arg(arguments, int);
            emit_padded(&sink, &character, 1, width, left, ' ');
            break;
        }
        case 's': {
            const char* text = __builtin_va_arg(arguments, const char*);
            koi_uint64 length;
            if (!text) text = "(null)";
            length = strlen(text);
            /* On a string, precision is a limit rather than a minimum - and
               the string need not be terminated within it. */
            if (precision >= 0 && (koi_uint64)precision < length)
                length = (koi_uint64)precision;
            emit_padded(&sink, text, length, width, left, ' ');
            break;
        }
        default:
            /* An unknown conversion is shown rather than swallowed: silently
               dropping it hides the mistake in the format string. */
            emit(&sink, '%');
            emit(&sink, *format);
            break;
        }
    }

    if (sink.out) *sink.out = 0;
    return (int)sink.written;
}

int koi_snprintf(char* out, koi_uint64 size, const char* format, ...) {
    __builtin_va_list arguments;
    int written;

    __builtin_va_start(arguments, format);
    written = koi_vformat(out, size, format, arguments);
    __builtin_va_end(arguments);
    return written;
}

int koi_sprintf(char* out, const char* format, ...) {
    __builtin_va_list arguments;
    int written;

    /* No size, because the caller did not give one. This is the interface C
       has always had and the reason buffers overflow; it is here because the
       code being ported uses it, not because it is a good idea. */
    __builtin_va_start(arguments, format);
    written = koi_vformat(out, 0x7FFFFFFF, format, arguments);
    __builtin_va_end(arguments);
    return written;
}

int koi_printf(const char* format, ...) {
    __builtin_va_list arguments;
    char line[1024];
    int written;

    __builtin_va_start(arguments, format);
    written = koi_vformat(line, sizeof(line), format, arguments);
    __builtin_va_end(arguments);
    koi_print(line);
    return written;
}

/* ---- A heap -------------------------------------------------------------
 *
 * The system hands out whole pages and nothing smaller; this divides one run
 * of them up. First fit over a list of blocks, with adjacent free blocks
 * merged on release - which is the simplest arrangement that does not
 * fragment itself to death, and is what DOS's own allocator did.
 *
 * More is asked of the system when the arena runs out, so a program does not
 * have to guess its own high-water mark in advance.
 */

#define ARENA_MINIMUM (256 * 1024)
#define BLOCK_MAGIC 0x4B424C4BU     /* "KBLK" */

typedef struct BLOCK {
    koi_uint32 magic;
    koi_uint32 used;
    koi_uint64 size;                /* payload bytes, not counting this header */
    struct BLOCK* next;
} BLOCK;

static BLOCK* heap_first;

static int heap_grow(koi_uint64 wanted) {
    koi_uint64 size = wanted + sizeof(BLOCK);
    BLOCK* block;

    if (size < ARENA_MINIMUM) size = ARENA_MINIMUM;
    block = (BLOCK*)koi_alloc((long)size);
    if (!block) return 0;

    block->magic = BLOCK_MAGIC;
    block->used = 0;
    block->size = size - sizeof(BLOCK);
    block->next = heap_first;
    heap_first = block;
    return 1;
}

/* Cut a block in two when the tail is worth keeping. A split that leaves less
   than a header plus a little is not worth the header. */
static void heap_split(BLOCK* block, koi_uint64 wanted) {
    BLOCK* tail;

    if (block->size < wanted + sizeof(BLOCK) + 32) return;
    tail = (BLOCK*)((koi_uint8*)(block + 1) + wanted);
    tail->magic = BLOCK_MAGIC;
    tail->used = 0;
    tail->size = block->size - wanted - sizeof(BLOCK);
    tail->next = block->next;
    block->next = tail;
    block->size = wanted;
}

void* malloc(koi_uint64 size) {
    BLOCK* block;

    if (!size) return (void*)0;
    size = (size + 15) & ~(koi_uint64)15;   /* keep payloads aligned */

    for (int attempt = 0; attempt < 2; attempt++) {
        for (block = heap_first; block; block = block->next) {
            if (block->used || block->size < size) continue;
            heap_split(block, size);
            block->used = 1;
            return (void*)(block + 1);
        }
        if (!heap_grow(size)) break;
    }
    return (void*)0;
}

void free(void* address) {
    BLOCK* block;

    if (!address) return;
    block = (BLOCK*)address - 1;
    if (block->magic != BLOCK_MAGIC) return;   /* not ours; leave it alone */
    block->used = 0;

    /* Merge with whatever follows, as far as it goes. Only forwards, because
       the list has no back pointer - which is enough: a run of frees collapses
       on the next pass through. */
    for (block = heap_first; block; block = block->next) {
        while (block->next && !block->used && !block->next->used &&
               (koi_uint8*)(block + 1) + block->size == (koi_uint8*)block->next) {
            block->size += sizeof(BLOCK) + block->next->size;
            block->next = block->next->next;
        }
    }
}

void* calloc(koi_uint64 count, koi_uint64 size) {
    koi_uint64 total = count * size;
    void* address = malloc(total);
    if (address) memset(address, 0, total);
    return address;
}

void* realloc(void* address, koi_uint64 size) {
    BLOCK* block;
    void* replacement;

    if (!address) return malloc(size);
    if (!size) { free(address); return (void*)0; }

    block = (BLOCK*)address - 1;
    if (block->magic != BLOCK_MAGIC) return (void*)0;
    if (block->size >= size) return address;

    replacement = malloc(size);
    if (!replacement) return (void*)0;
    memcpy(replacement, address, block->size);
    free(address);
    return replacement;
}
