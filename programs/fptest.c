#include "koi.h"

/* Floating point, and whether it survives being interrupted.
 *
 * Two things are being tested and they are different. That arithmetic works
 * at all is the easy half - it either compiles and runs or it does not. The
 * half worth a program is that it goes on being right while the machine takes
 * the processor away and gives it to somebody else: a task switch that does
 * not save these registers is a bug that only appears when two programs do
 * sums at once, and only sometimes.
 *
 * So: a long sum with a known answer, computed while something else runs.
 */
int main(const char* arguments) {
    double total = 0.0;
    double expected;
    long rounds = 200000;
    (void)arguments;

    for (long index = 1; index <= rounds; index++) total += 1.0 / (double)index;

    /* Harmonic series; compared against itself computed a second way rather
       than against a constant, because the constant would have to be typed
       and typing it is where the mistake would be. */
    expected = 0.0;
    for (long index = rounds; index >= 1; index--) expected += 1.0 / (double)index;

    koi_print("floating point: ");
    koi_print_dec((koi_uint64)(total * 1000000.0));
    koi_print(" against ");
    koi_print_dec((koi_uint64)(expected * 1000000.0));
    koi_print("\n");
    /* Summed in opposite orders, so they differ in the last places and not
       before them. A difference larger than that means the registers were
       disturbed. */
    koi_print((total > expected ? total - expected : expected - total) < 0.000001
              ? "floating point: agrees\n" : "floating point: WRONG\n");
    return 0;
}
