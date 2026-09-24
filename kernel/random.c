#include "random.h"
#include "sha256.h"
#include "string.h"
#include "timer.h"
#include "rtc.h"
#include "serial.h"

/* Where keys come from.
 *
 * ---- The shape ------------------------------------------------------------
 *
 * A pool of 32 bytes and SHA-256. Asking for bytes hashes the pool together
 * with a counter and hands back the result; the pool is then replaced by a
 * hash of itself, so the bytes just given out cannot be run backwards to
 * recover what comes next. This is the standard hash-based generator and it is
 * the smallest construction with that property.
 *
 * ---- Where the unpredictability actually comes from -----------------------
 *
 * RDRAND, when the processor has it - which every x86-64 since Ivy Bridge
 * does, and it is a hardware noise source with a conditioner behind it. Its
 * output goes into the pool rather than straight out to a caller: trusting a
 * generator inside somebody else's silicon completely is a decision this
 * project does not have to make, and mixing costs nothing.
 *
 * Without it: the timestamp counter, the real-time clock, and the timing
 * jitter of a short loop of reads. That is genuinely weaker, and it is the
 * reason `random_is_strong()` exists rather than a comfortable silence.
 *
 * ---- The failure this is written against ---------------------------------
 *
 * A generator that returns the same bytes on every boot passes every test
 * anybody thinks to run, because the bytes look perfectly random. They are
 * simply the same ones the machine next to it produced. So the pool is stirred
 * at startup from several sources, stirred again by anything unpredictable
 * that happens later, and never used before random_start() has run - which the
 * counter below makes visible rather than assumed.
 */

static boot_uint8_t pool[SHA256_SIZE];
static boot_uint64_t counter;
static int started;
static int have_hardware;

static boot_uint64_t timestamp(void) {
    boot_uint32_t low, high;

    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((boot_uint64_t)high << 32) | low;
}

static int processor_has_rdrand(void) {
    boot_uint32_t a, b, c, d;

    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(1), "c"(0));
    return (c & (1U << 30)) != 0;
}

/* One 64-bit value from the processor. It is allowed to fail - the standard
   says to retry a bounded number of times and then give up, and a generator
   that spins forever on a broken unit is a machine that stops booting. */
static int rdrand(boot_uint64_t* out) {
    for (int attempt = 0; attempt < 16; attempt++) {
        boot_uint64_t value = 0;
        boot_uint8_t worked = 0;

        __asm__ volatile ("rdrand %0; setc %1"
                          : "=r"(value), "=qm"(worked));
        if (worked) { *out = value; return 1; }
    }
    return 0;
}

void random_stir(const void* data, boot_uint32_t length) {
    SHA256 hash;
    boot_uint64_t now = timestamp();

    sha256_start(&hash);
    sha256_add(&hash, pool, sizeof(pool));
    sha256_add(&hash, &now, sizeof(now));
    if (data && length) sha256_add(&hash, data, length);
    sha256_finish(&hash, pool);
}

void random_start(void) {
    RTC_TIME clock;
    boot_uint64_t samples[16];

    have_hardware = processor_has_rdrand();

    /* Whatever the machine can say about itself at this instant. */
    rtc_read(&clock);
    random_stir(&clock, sizeof(clock));
    {
        boot_uint64_t ticks = timer_ticks();

        random_stir(&ticks, sizeof(ticks));
    }

    if (have_hardware) {
        for (int at = 0; at < 16; at++)
            if (!rdrand(&samples[at])) { have_hardware = 0; break; }
        if (have_hardware) random_stir(samples, sizeof(samples));
    }

    if (!have_hardware) {
        /* The fallback: how long a short loop takes, sampled repeatedly. The
           counts differ between runs because interrupts, refresh cycles and
           the memory system do not repeat exactly - it is a few bits per
           sample, not many, which is why sixty-four of them are taken. */
        for (int round = 0; round < 64; round++) {
            boot_uint64_t before = timestamp();
            volatile int spin = 0;

            for (int at = 0; at < 997; at++) spin += at;
            samples[round % 16] = (timestamp() - before) ^ (boot_uint64_t)spin;
            if (round % 16 == 15) random_stir(samples, sizeof(samples));
        }
        serial_write("RANDOM: no RDRAND; keys come from timing, which is "
                     "weaker\n");
    } else {
        serial_write("RANDOM: RDRAND present\n");
    }

    started = 1;
}

int random_is_strong(void) {
    return started && have_hardware;
}

void random_bytes(void* out, boot_uint32_t length) {
    boot_uint8_t* at = (boot_uint8_t*)out;

    /* Asking before startup is a mistake in the caller, not a reason to hand
       back zeroes quietly. */
    if (!started) random_start();

    while (length) {
        boot_uint8_t block[SHA256_SIZE];
        SHA256 hash;
        boot_uint32_t take = length < SHA256_SIZE ? length : SHA256_SIZE;
        boot_uint64_t extra = 0;

        if (have_hardware && rdrand(&extra)) { /* mixed in below */ }

        counter++;
        sha256_start(&hash);
        sha256_add(&hash, pool, sizeof(pool));
        sha256_add(&hash, &counter, sizeof(counter));
        sha256_add(&hash, &extra, sizeof(extra));
        sha256_finish(&hash, block);
        memcpy(at, block, take);
        at += take;
        length -= take;

        /* Forward secrecy: the pool becomes a hash of itself, so what was
           just handed out cannot be used to work out what came before it. */
        {
            boot_uint8_t next[SHA256_SIZE];

            sha256_start(&hash);
            sha256_add(&hash, pool, sizeof(pool));
            sha256_add(&hash, block, sizeof(block));
            sha256_finish(&hash, next);
            memcpy(pool, next, sizeof(pool));
        }
    }
}
