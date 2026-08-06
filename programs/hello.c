#include "koi.h"

/* The first Koi-DOS program. Proves the whole path: loaded from FAT32 by the
   kernel, entered at a fixed address, talking to the kernel only through
   int 0x40, and returning an exit code the shell can print. */

int main(const char* arguments) {
    long version = koi_version();

    koi_color(KOI_YELLOW, KOI_BLUE);
    koi_print("Hello from a Koi-DOS program.\n");
    koi_color(KOI_LIGHT_GRAY, KOI_BLUE);

    koi_print("Running on Koi-DOS ");
    koi_print_dec((koi_uint64)(version >> 8));
    koi_putchar('.');
    koi_print_dec((koi_uint64)(version & 0xFF));
    koi_print("\n");

    if (arguments && arguments[0]) {
        koi_print("You gave me: ");
        koi_print(arguments);
        koi_print("\n");
    } else {
        koi_print("No arguments given.\n");
    }
    return 0;
}
