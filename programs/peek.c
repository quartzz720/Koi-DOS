#include "koi.h"

/* Read one byte of memory and print it.
 *
 * A diagnostic, and the only honest way to test isolation: everything else
 * about a program with its own address space looks exactly like a program
 * without one. This reads an address it was given and says what is there, and
 * what happens next is the whole answer -
 *
 *     peek 100000          at ring 0: prints a byte of the kernel
 *     ring3 peek 100000    at ring 3: page fault, and the shell survives
 *
 * The address is hexadecimal, because every address anybody wants to type
 * here came out of a fault message and those are hexadecimal too.
 *
 * It defaults to reading its own first byte, which is always allowed and is
 * the control case: if that faults, something is wrong with the loader rather
 * than with the memory being asked for.
 */

static unsigned long long from_hex(const char* text) {
    unsigned long long value = 0;

    while (*text == ' ') text++;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    for (; *text; text++) {
        unsigned digit;

        if (*text >= '0' && *text <= '9') digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = (unsigned)(*text - 'a' + 10);
        else if (*text >= 'A' && *text <= 'F') digit = (unsigned)(*text - 'A' + 10);
        else break;
        value = (value << 4) | digit;
    }
    return value;
}

/* Hexadecimal, because the addresses anybody types here came out of a fault
   message and those are hexadecimal too. */
static void print_hex(unsigned long long value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[17];
    int at = 16;

    text[16] = 0;
    if (!value) { koi_print("0"); return; }
    while (value && at > 0) {
        text[--at] = digits[value & 0xF];
        value >>= 4;
    }
    koi_print(text + at);
}

int main(const char* arguments) {
    unsigned long long address;
    const volatile unsigned char* at;

    if (arguments && arguments[0]) address = from_hex(arguments);
    else address = (unsigned long long)(unsigned long)&main;

    koi_print("Reading 0x");
    print_hex(address);
    koi_print("\n");

    at = (const volatile unsigned char*)(unsigned long)address;
    koi_print("It holds 0x");
    print_hex(*at);
    koi_print("\n");
    return 0;
}
