#include "koi.h"

/* A directory listing written as an ordinary program rather than a built-in
   command. The point is that nothing here is privileged: it sees the same
   filesystem the shell does, through SYS_FINDFIRST and SYS_FINDNEXT. */

static void print_padded(koi_uint64 value, int width) {
    char buffer[24];
    int length = 0;

    do { buffer[length++] = (char)('0' + (value % 10U)); value /= 10U; }
    while (value);
    for (int pad = length; pad < width; pad++) koi_putchar(' ');
    while (length--) koi_putchar(buffer[length]);
}

static void print_two(unsigned value) {
    koi_putchar((char)('0' + (value / 10U) % 10U));
    koi_putchar((char)('0' + value % 10U));
}

int main(const char* arguments) {
    KOI_FIND_DATA found;
    long search;
    long files = 0;
    long directories = 0;
    koi_uint64 bytes = 0;
    const char* pattern = (arguments && arguments[0]) ? arguments : "*";

    search = koi_findfirst(pattern, &found);
    if (search == SYSCALL_ERROR) {
        koi_print("No files matching ");
        koi_print(pattern);
        koi_print("\n");
        return 1;
    }

    do {
        print_padded(1980U + (found.date >> 9), 4);
        koi_putchar('-');
        print_two((found.date >> 5) & 0x0FU);
        koi_putchar('-');
        print_two(found.date & 0x1FU);
        koi_print("  ");
        print_two((found.time >> 11) & 0x1FU);
        koi_putchar(':');
        print_two((found.time >> 5) & 0x3FU);

        if (found.attributes & KOI_ATTRIBUTE_DIRECTORY) {
            koi_print("        <DIR>  ");
            directories++;
        } else {
            print_padded(found.size, 13);
            koi_print("  ");
            files++;
            bytes += found.size;
        }
        koi_print(found.name);
        koi_print("\n");
    } while (koi_findnext(search, &found) == 0);

    koi_findclose(search);

    koi_print("\n");
    print_padded((koi_uint64)files, 8);
    koi_print(" file(s)");
    print_padded(bytes, 16);
    koi_print(" bytes\n");
    print_padded((koi_uint64)directories, 8);
    koi_print(" dir(s)\n");
    return 0;
}
