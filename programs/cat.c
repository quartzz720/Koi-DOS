#include "koi.h"

/* Reads a file through the system call interface rather than asking the shell
   to do it - the point being that a program can reach the filesystem itself. */

int main(const char* arguments) {
    static char buffer[512];
    long handle;
    long got;

    if (!arguments || !arguments[0]) {
        koi_print("usage: cat <file>\n");
        return 1;
    }

    handle = koi_open(arguments, OPEN_READ);
    if (handle == SYSCALL_ERROR) {
        koi_print("cat: cannot open ");
        koi_print(arguments);
        koi_print("\n");
        return 1;
    }

    koi_print("size: ");
    koi_print_dec((koi_uint64)koi_filesize(handle));
    koi_print(" bytes\n");

    while ((got = koi_read(handle, buffer, sizeof(buffer))) > 0) {
        for (long index = 0; index < got; index++) {
            if (buffer[index] == '\r') continue;
            koi_putchar(buffer[index]);
        }
    }
    koi_close(handle);
    return 0;
}
