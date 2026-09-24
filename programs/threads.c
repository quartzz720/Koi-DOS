#include "koi.h"

/* threads - two lines of execution in one program, watched side by side.
 *
 * The first thing written against the thread calls, and deliberately the
 * smallest: a worker that counts while the main thread prints. If the numbers
 * climb while the printing happens, the processor is being shared inside one
 * program - which is the whole claim, and it is worth a program that tests it
 * rather than a page that asserts it.
 *
 * The counter is deliberately shared and deliberately locked, because the
 * second thing worth proving is that the lock works: without it, two threads
 * adding to one number lose additions, and the loss is invisible in every
 * individual run.
 */

#define STACK_BYTES 16384
#define ROUNDS 200000

static KOI_LOCK lock;
static volatile long counter;
static volatile int workers_running;
static volatile int stop_now;

static void worker(void* argument) {
    long which = (long)argument;

    for (long at = 0; at < ROUNDS && !stop_now; at++) {
        koi_lock(&lock);
        counter++;
        koi_unlock(&lock);
    }
    koi_printf("  worker %ld finished its %d\n", which, ROUNDS);
    workers_running--;
    koi_thread_exit();
}

int main(void) {
    void* stacks[2];
    long spun = 0;

    koi_print("Two threads, one program.\n\n");

    for (long at = 0; at < 2; at++) {
        stacks[at] = koi_alloc(STACK_BYTES);
        if (!stacks[at]) { koi_print("no memory for a stack\n"); return 1; }
        workers_running++;
        if (!koi_thread_start(worker, (char*)stacks[at] + STACK_BYTES,
                              (void*)at)) {
            koi_print("the kernel would not start a thread\n");
            workers_running--;
            return 1;
        }
    }

    /* The main thread keeps working the whole time. If threads are real, this
       prints while the counters climb; if they are not, it prints only after
       the workers are done. */
    while (workers_running) {
        long seen;

        koi_lock(&lock);
        seen = counter;
        koi_unlock(&lock);
        koi_printf("  main thread sees %ld\n", seen);
        spun++;
        koi_sleep(50);
        if (spun > 200) { stop_now = 1; break; }
    }

    koi_printf("\nboth workers added %d each; the counter is %ld\n",
               ROUNDS, counter);
    koi_print(counter == 2L * ROUNDS
              ? "Nothing was lost: the lock held.\n"
              : "Additions were LOST - the lock did not hold.\n");
    for (int at = 0; at < 2; at++) koi_free(stacks[at]);
    return 0;
}
