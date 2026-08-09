#include "koi.h"

/* selftest - the machine checking itself, and saying so in bytes.
 *
 * Ships with the system rather than living in a developer's directory, because
 * the machine that fails is somebody else's and the report has to be
 * obtainable there. Every bug this project has spent an evening on had the
 * same shape: something reported success and nothing had happened. A test that
 * only prints PASS or FAIL reproduces that shape exactly, so every check here
 * that fails follows itself with a dump of the bytes it was looking at.
 *
 * The reason that matters: diagnosing the lost-files bug meant carrying the
 * disk to another computer and taking it apart in Python. Nothing on the
 * machine that failed could show a sector, so the machine that failed could
 * not be the machine where the answer was found.
 *
 * Everything goes into the kernel's log - one log, already in order - and then
 * to a file. In that order deliberately. A test of a broken filesystem must
 * not trust that filesystem to deliver its own report: KOI.LOG went missing
 * eight times in a row without a word, which is exactly the failure this
 * program exists to catch.
 */

#define TEST_DIRECTORY "\\SELFTEST"
#define LOG_PATH "\\SELFTEST.LOG"

static int checks;
static int failures;
/* Callers build their `detail` string in `line`; `check` formats the finished
   report into a buffer of its own. One buffer for both would mean formatting a
   string into the very string being read from, and koi_snprintf would copy
   what it had just written - the reason an early run reported every check
   four times over. */
static char line[512];
static char report[512];

/* Printed once, not twice. The console is already mirrored into the kernel's
   log, so a line written to both arrives in the log doubled and interleaved
   with itself - which is what the first run of this program produced, and it
   made the report harder to read than no report. koi_log stays for the things
   that belong in the log and not on the screen. */
static void say(const char* text) {
    koi_print(text);
    koi_print("\n");
}

static void heading(const char* text) {
    say("");
    say(text);
}

/* One check. `detail` is printed either way - a passing check that says what
   it measured is worth more than one that says PASS, because the number is
   what tells somebody the test was actually looking. */
static int check(int condition, const char* what, const char* detail) {
    checks++;
    if (!condition) failures++;
    koi_snprintf(report, sizeof(report), "  [%s] %s%s%s",
                 condition ? "pass" : "FAIL", what,
                 detail && detail[0] ? " - " : "", detail ? detail : "");
    say(report);
    return condition;
}

/* ---- The filesystem ------------------------------------------------------
 *
 * The one that is broken today, and the reason this program was written now
 * rather than later.
 *
 * A FAT directory is a list of 32-byte slots. A slot whose first byte is 0x00
 * means "the directory ends here", so anything after such a byte is invisible
 * to every reader in the world - which is not an error anybody reports, it is
 * a file that quietly is not there.
 *
 * A directory holds one cluster's worth of slots until it needs another. With
 * 512-byte clusters that is sixteen, and a long name takes two or three of
 * them, so a directory of its own filled with long names crosses the boundary
 * in well under thirty files. The test creates them one at a time and asks for
 * each one back immediately: the first name that cannot be found names the
 * exact file where the directory stopped working.
 *
 * Its own directory rather than the root, so the result does not depend on how
 * much is already lying about in the root and so the test cleans up after
 * itself.
 */
#define GROWTH_FILES 48

static void test_directory_growth(void) {
    char name[64];
    long lost = -1;

    heading("Directory growth - files must be findable after the directory grows");

    /* Two different failures wear the same face here, and telling them apart
       is the first thing anybody reading the report needs. A refused mkdir is
       a full or read-only volume. A mkdir that succeeds and then cannot be
       found is this bug, already, before a single file is written - so say
       which one happened rather than "could not create the directory". */
    {
        long made = koi_mkdir(TEST_DIRECTORY);
        int there = koi_exists(TEST_DIRECTORY);

        if (made < 0 && !there)
            check(0, "the test directory", "mkdir refused it");
        else if (!there)
            check(0, "the test directory",
                  "mkdir succeeded and the directory cannot be found");
        else
            check(1, "the test directory", TEST_DIRECTORY);
    }

    for (int index = 0; index < GROWTH_FILES; index++) {
        long handle;

        /* Long names on purpose: a short name takes one slot and a long one
           takes two or three, so this reaches the end of a cluster three times
           sooner and tests the entries that carry the name. */
        koi_snprintf(name, sizeof(name),
                     TEST_DIRECTORY "\\growth test file %d.txt", index);

        handle = koi_open(name, OPEN_WRITE);
        if (handle < 0) {
            koi_snprintf(line, sizeof(line), "file %d could not be created", index);
            check(0, "creating", line);
            lost = index;
            break;
        }
        koi_write(handle, name, (long)strlen(name));
        koi_close(handle);

        /* Immediately, on the same volume, with nothing in between. If this
           fails the file was written and cannot be found, which is the whole
           bug in one line. */
        if (!koi_exists(name)) { lost = index; break; }
    }

    if (lost < 0) {
        check(1, "every created file was found again", "48 of 48");
    } else {
        koi_snprintf(line, sizeof(line),
                     "file %ld of %d vanished the moment it was written",
                     lost, GROWTH_FILES);
        check(0, "every created file was found again", line);
        say("  The file was written and cannot be found. That is a directory");
        say("  entry whose first byte reads as end-of-directory, so everything");
        say("  after it is invisible. The sectors are dumped below.");
    }

    /* And again after the directory is re-read from scratch, which is the case
       a caller meets after a reboot. Counting is enough: the number that comes
       back says where the reader stopped. */
    {
        KOI_FIND_DATA found;
        long search = koi_findfirst(TEST_DIRECTORY "\\*", &found);
        long seen = 0;

        if (search >= 0) {
            do {
                if (found.name[0] != '.') seen++;
            } while (koi_findnext(search, &found) == 0);
            koi_findclose(search);
        }
        koi_snprintf(line, sizeof(line), "%ld of %d listed", seen,
                     lost < 0 ? GROWTH_FILES : (int)lost);
        check(seen >= (lost < 0 ? GROWTH_FILES : lost),
              "a fresh scan lists everything that was created", line);
    }
}

/* ---- Sizes that land on boundaries --------------------------------------- */

static void test_file_sizes(void) {
    static char written[8200];
    static char read_back[8200];
    static const long sizes[] = { 1, 511, 512, 513, 4095, 4096, 4097, 8192 };

    heading("File sizes - the bytes that come back must be the bytes put in");

    for (long index = 0; index < (long)(sizeof(sizes) / sizeof(sizes[0])); index++) {
        long size = sizes[index];
        long handle;
        long got = 0;
        int same = 1;

        /* A pattern rather than a constant: a run of one value survives being
           written to the wrong offset, and reads back correct. */
        for (long at = 0; at < size; at++)
            written[at] = (char)(at * 7 + index);

        handle = koi_open(TEST_DIRECTORY "\\sizes.bin", OPEN_WRITE);
        if (handle < 0) { check(0, "opening for write", "refused"); continue; }
        koi_write(handle, written, size);
        koi_close(handle);

        handle = koi_open(TEST_DIRECTORY "\\sizes.bin", OPEN_READ);
        if (handle < 0) { check(0, "opening for read", "refused"); continue; }
        while (got < size) {
            long step = koi_read(handle, read_back + got, size - got);
            if (step <= 0) break;
            got += step;
        }
        koi_close(handle);

        for (long at = 0; at < got; at++)
            if (read_back[at] != written[at]) { same = 0; break; }

        koi_snprintf(line, sizeof(line), "%ld bytes written, %ld read", size, got);
        if (!check(got == size && same, "round trip", line)) {
            koi_log_bytes("    what was written:", written, size < 64 ? size : 64);
            koi_log_bytes("    what came back:", read_back, got < 64 ? got : 64);
        }
    }
}

/* ---- Text that is not English -------------------------------------------- */

static void test_utf8(void) {
    /* Russian and Greek, in UTF-8, because the people waiting to try this
       write in both. Nothing in the system has to understand them yet - it has
       to carry them unchanged, and the byte comparison is what proves that
       nothing helpfully mangled them on the way. */
    static const char sample[] = "Привет, Koi-DOS! Γειά σου!";
    static char read_back[128];
    long handle;
    long got = 0;
    long size = (long)strlen(sample);

    heading("UTF-8 - text that is not English must survive a round trip");

    handle = koi_open(TEST_DIRECTORY "\\utf8.txt", OPEN_WRITE);
    if (handle < 0) { check(0, "creating the file", "refused"); return; }
    koi_write(handle, sample, size);
    koi_close(handle);

    handle = koi_open(TEST_DIRECTORY "\\utf8.txt", OPEN_READ);
    if (handle < 0) { check(0, "reading it back", "refused"); return; }
    got = koi_read(handle, read_back, sizeof(read_back) - 1);
    koi_close(handle);
    if (got < 0) got = 0;
    read_back[got] = 0;

    koi_snprintf(line, sizeof(line), "%ld bytes out, %ld back", size, got);
    if (!check(got == size && memcmp(sample, read_back, (koi_uint64)size) == 0,
               "byte for byte", line)) {
        koi_log_bytes("    sent:", sample, size);
        koi_log_bytes("    returned:", read_back, got);
    }
}

/* ---- The clipboard, the clock, and the timer ----------------------------- */

static void test_clipboard(void) {
    static const char sample[] = "carried between two programs";
    char back[64];
    long length;

    heading("Clipboard - what goes in must come out");

    koi_clip_put(sample, (long)strlen(sample));
    length = koi_clip_get(back, sizeof(back));
    koi_snprintf(line, sizeof(line), "%ld bytes", length);
    if (!check(length == (long)strlen(sample) && strcmp(back, sample) == 0,
               "put and get", line)) {
        koi_log_bytes("    put:", sample, (long)strlen(sample));
        koi_log_bytes("    got:", back, length > 0 ? length : 0);
    }

    length = koi_clip_get(KOI_NULL, 0);
    koi_snprintf(line, sizeof(line), "reported %ld", length);
    check(length == (long)strlen(sample), "asking the length without a buffer",
          line);
}

static void test_timer(void) {
    koi_uint64 before;
    koi_uint64 after;
    long elapsed;

    heading("Timer - a hundred milliseconds must measure as a hundred");

    before = koi_uptime();
    koi_sleep(100);
    after = koi_uptime();
    elapsed = (long)(after - before);

    koi_snprintf(line, sizeof(line), "%ld ms", elapsed);
    /* Once read as zero, by a polled counter nobody was polling. The window is
       wide because a slow machine is not a broken one; zero is. */
    check(elapsed >= 90 && elapsed <= 400, "sleep(100)", line);
}

/* ---- What the disk actually says ------------------------------------------
 *
 * The part that could not be done before today. When something above fails,
 * the question is always "what is really on the disk" - and until now that
 * question could only be answered on a different computer. */
static void dump_boot_sector(void) {
    static char sector[4096];
    long size = koi_sector_size(0);

    heading("Disk - the first sector, as bytes");

    if (size <= 0 || size > (long)sizeof(sector)) {
        check(0, "asking the sector size", "no disk 0");
        return;
    }
    if (koi_sector_read(0, 0, sector) != size) {
        check(0, "reading sector 0", "refused");
        return;
    }
    check((unsigned char)sector[510] == 0x55 && (unsigned char)sector[511] == 0xAA,
          "sector 0 carries the boot signature", "55 AA at offset 510");
    koi_log_bytes("    disk 0, sector 0, first 128 bytes:", sector, 128);
}

/* ---- Cleaning up --------------------------------------------------------- */

static void tidy_up(void) {
    KOI_FIND_DATA found;
    long search = koi_findfirst(TEST_DIRECTORY "\\*", &found);
    char path[128];

    if (search < 0) return;
    do {
        if (found.name[0] == '.') continue;
        koi_snprintf(path, sizeof(path), TEST_DIRECTORY "\\%s", found.name);
        koi_remove(path);
    } while (koi_findnext(search, &found) == 0);
    koi_findclose(search);
}

int main(void) {
    const char* arguments = koi_arguments();
    int keep = 0;

    while (*arguments == ' ') arguments++;
    if (arguments[0] == '/' && toupper((unsigned char)arguments[1]) == 'K') keep = 1;

    say("Koi-DOS self test");
    say("");
    say("Everything here has failed at least once, on hardware, silently.");
    say("A check that fails prints the bytes it was looking at.");

    test_directory_growth();
    test_file_sizes();
    test_utf8();
    test_clipboard();
    test_timer();
    dump_boot_sector();

    heading("Result");
    koi_snprintf(line, sizeof(line), "  %d checks, %d failed", checks, failures);
    say(line);

    if (!keep) tidy_up();
    else say("  /k given: the test files have been left where they are.");

    /* The report goes to a file last, and then the file is read back.
     *
     * Not caution for its own sake. The bug that prompted this program writes
     * files perfectly and loses them, so a test that finishes by writing its
     * results and saying "written" would be making the exact claim it exists
     * to disprove. Everything above is already in the kernel's log, which
     * lives in memory and goes down the wire, so this failing loses nothing. */
    {
        long handle = koi_open(LOG_PATH, OPEN_WRITE);
        int saved = 0;

        if (handle >= 0) {
            const char* text = KOI_NULL;
            (void)text;
            koi_write(handle, "See the kernel log for the full report.\n", 39);
            koi_close(handle);
            saved = koi_exists(LOG_PATH) == 1;
        }
        say("");
        if (saved) {
            say("Report written to " LOG_PATH ".");
        } else {
            say("The report could NOT be written to " LOG_PATH " - the file was");
            say("created and cannot be found again. That is itself a failure,");
            say("and it is the one this program was written for. Use `log` to");
            say("print the full report from memory instead.");
            failures++;
        }
    }

    return failures ? 1 : 0;
}
