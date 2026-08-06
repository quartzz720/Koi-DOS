#include "koi.h"

/* Writes its arguments to a file, so that the write half of the file system
   calls is exercised by something other than the shell.
   Usage: save <file> <text...> */

static long word_length(const char* text) {
    long length = 0;
    while (text[length] && text[length] != ' ') length++;
    return length;
}

int main(const char* arguments) {
    static char name[128];
    const char* text;
    long name_length;
    long handle;
    long length = 0;
    long written;

    if (!arguments || !arguments[0]) {
        koi_print("usage: save <file> <text...>\n");
        return 1;
    }

    name_length = word_length(arguments);
    if (name_length >= (long)sizeof(name)) return 1;
    for (long index = 0; index < name_length; index++) name[index] = arguments[index];
    name[name_length] = 0;

    text = arguments + name_length;
    while (*text == ' ') text++;
    while (text[length]) length++;
    if (!length) { koi_print("save: nothing to write\n"); return 1; }

    handle = koi_open(name, OPEN_WRITE);
    if (handle == SYSCALL_ERROR) {
        koi_print("save: cannot create ");
        koi_print(name);
        koi_print("\n");
        return 1;
    }

    written = koi_write(handle, text, length);
    koi_close(handle);

    koi_print_dec((koi_uint64)written);
    koi_print(" bytes written to ");
    koi_print(name);
    koi_print("\n");
    return written == length ? 0 : 1;
}
