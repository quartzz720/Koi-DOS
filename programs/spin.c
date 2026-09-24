#include "koi.h"

/* A program that does not stop on its own.
 *
 * It exists to be interrupted. Ctrl+C is one of the few things that cannot be
 * checked by looking at the code: what it claims is that a program which never
 * intends to return can be made to, and the only way to see that is to have
 * one and press the key.
 *
 * Four shapes, because they fail differently:
 *
 *   spin              a loop that prints - the accidental infinite loop
 *   spin wait         blocked on a keystroke that never comes
 *   spin quiet        a loop that calls nothing at all
 *   spin work A 6     six rounds of arithmetic, each one announced
 *
 * The third is the honest one. Ctrl+C still cannot reach it: the key sets a
 * flag the kernel hands over at the next system call, and this program has no
 * next system call. What has changed is what that costs. There is a scheduler
 * now, and a program at ring 3 has the processor taken away from it every ten
 * milliseconds whether it offers or not - so `ring3 spin quiet` is a program
 * that cannot be stopped on a machine that goes on working around it, rather
 * than a machine that has to be reset. At ring 0 it is still the old story,
 * and that is the trade the ring is for.
 *
 * The fourth is the bench for that scheduler, and it has to be this shape to
 * be one: work at ring 3 that asks the kernel for nothing, so that a switch
 * away from it can only have been taken rather than given.
 *
 *     start ring3 spin work A 6
 *     ring3 spin work B 6
 *
 * If the lines come out interleaved, the processor is being taken away from a
 * program that never offered it, which is the whole of what preemption means
 * and the only way to watch it happen.
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

/* Something slow and unoptimisable. `volatile` because without it the compiler
   is entirely right to notice that nothing reads the sum and delete the loop,
   and a bench that has been optimised into nothing always passes. */
static koi_uint64 a_moment_of_work(void) {
    volatile koi_uint64 sum = 0;

    for (koi_uint64 index = 0; index < 40000000ULL; index++) sum += index;
    return sum;
}

static int number(const char* text) {
    int value = 0;

    while (*text == ' ') text++;
    if (*text < '0' || *text > '9') return 0;
    for (; *text >= '0' && *text <= '9'; text++) value = value * 10 + (*text - '0');
    return value;
}

/* `spin work <label> <rounds>`: rounds of arithmetic with a word printed
   between them, so that two of these running at once can be told apart by
   looking at the output. */
static int work(const char* arguments) {
    char label[16];
    int at = 0;
    int rounds;

    while (*arguments == ' ') arguments++;
    while (*arguments && *arguments != ' ' && at < (int)sizeof(label) - 1)
        label[at++] = *arguments++;
    label[at] = 0;
    if (!at) {
        label[0] = '?';
        label[1] = 0;
    }
    rounds = number(arguments);
    if (rounds < 1 || rounds > 100) rounds = 10;

    for (int round = 1; round <= rounds; round++) {
        if (!a_moment_of_work()) koi_print("");
        koi_print(label);
        koi_print(" ");
        koi_print_dec((koi_uint64)round);
        koi_print("\n");
    }
    koi_print(label);
    koi_print(" done\n");
    return 0;
}

int main(const char* arguments) {
    const char* mode = arguments ? arguments : "";

    while (*mode == ' ') mode++;

    if (mode[0] == 'w' && mode[1] == 'o' && mode[2] == 'r' && mode[3] == 'k' &&
        (mode[4] == ' ' || !mode[4]))
        return work(mode + 4);

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
