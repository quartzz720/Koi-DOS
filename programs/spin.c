#include "koi.h"

/* A program that does not stop on its own.
 *
 * It exists to be interrupted. Ctrl+C is one of the few things that cannot be
 * checked by looking at the code: what it claims is that a program which never
 * intends to return can be made to, and the only way to see that is to have
 * one and press the key.
 *
 * Three shapes, because they fail differently:
 *
 *   spin          a loop that prints - the accidental infinite loop
 *   spin wait     blocked on a keystroke that never comes
 *   spin quiet    a loop that calls nothing at all
 *
 * The third is the honest one. Nothing in the kernel can stop it, because it
 * never enters the kernel to be stopped, and pretending otherwise would need a
 * scheduler that can take the processor away. It is here so that the limit is
 * demonstrated rather than described.
 */

static int same(const char* left, const char* right) {
    while (*left && *right) {
        char a = *left, b = *right;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
        left++;
        right++;
    }
    return !*left && !*right;
}

int main(const char* arguments) {
    const char* mode = arguments ? arguments : "";

    while (*mode == ' ') mode++;

    if (same(mode, "wait")) {
        koi_print("Waiting for a key that is not coming. Ctrl+C to stop.\n");
        koi_getchar();
        koi_print("A key arrived after all.\n");
        return 0;
    }

    if (same(mode, "quiet")) {
        koi_print("Looping without calling anything. Ctrl+C cannot reach this;\n");
        koi_print("the machine has to be reset. This is the known limit.\n");
        for (;;) { }
    }

    koi_print("Looping. Ctrl+C to stop.\n");
    for (long count = 0; ; count++) {
        koi_print_dec((koi_uint64)count);
        koi_print(" ");
        koi_sleep(200);
    }
}
