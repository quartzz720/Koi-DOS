#include "koi.h"
#include "dialog.h"
#include "settings.h"

/* The questions Mizu asks the first time, and whenever somebody asks for them
 * again by running this.
 *
 * A graphical shell that arrives over the wire and then sits there waiting to
 * be typed is half an installation. The other half is the handful of decisions
 * that only the person at the machine can make, and the moment to ask them is
 * the moment they installed it - not the first time something goes wrong.
 *
 * Deliberately on the console rather than in Mizu's own graphics. These
 * questions are asked before Mizu has ever run, on a machine where the thing
 * being configured is the thing that would have to draw the dialogue.
 *
 * What is not asked here yet, and why: language and region. Choosing Russian
 * is easy and showing it is not - the console font is looked up by byte on one
 * code page, with no glyph for a codepoint and no keyboard layout behind it.
 * A setting that is accepted and then does nothing is worse than one that is
 * missing, so it is missing.
 */

#define AUTOEXEC "\\AUTOEXEC.BAT"
#define START_COMMAND "\\COMMANDER\\COMMANDER"
#define FILE_MAX 4096

static char file[FILE_MAX];

static long read_file(const char* path, char* into, long limit) {
    long handle = koi_open(path, OPEN_READ);
    long got;

    if (handle < 0) return -1;
    got = koi_read(handle, into, limit - 1);
    koi_close(handle);
    if (got < 0) got = 0;
    into[got] = 0;
    return got;
}

static int write_file(const char* path, const char* text, long length) {
    long handle;

    /* Removed and made again rather than written over: a shorter file written
       into a longer one leaves the tail of the old one behind, and the tail of
       an AUTOEXEC.BAT is a command that still runs. */
    koi_remove(path);
    handle = koi_open(path, OPEN_WRITE);
    if (handle < 0) return 0;
    if (koi_write(handle, text, length) != length) { koi_close(handle); return 0; }
    koi_close(handle);
    return 1;
}

/* Is the command already there? Compared case-insensitively and ignoring
   leading spaces, because a person who edited the file by hand is entitled to
   have written it their own way. */
static int line_matches(const char* line, const char* command) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '@') line++;
    for (int index = 0; command[index]; index++) {
        if (toupper((unsigned char)line[index]) !=
            toupper((unsigned char)command[index])) return 0;
    }
    return 1;
}

static int autostart_is_set(void) {
    long at = 0;

    if (read_file(AUTOEXEC, file, FILE_MAX) < 0) return 0;
    while (file[at]) {
        long start = at;
        while (file[at] && file[at] != '\n') at++;
        if (line_matches(file + start, START_COMMAND)) return 1;
        if (file[at]) at++;
    }
    return 0;
}

static int set_autostart(int wanted) {
    static char rebuilt[FILE_MAX];
    long out = 0;
    long at = 0;
    long got = read_file(AUTOEXEC, file, FILE_MAX);

    if (got < 0) got = 0;

    /* Every line except the one this owns, then the one this owns if it is
       wanted. Rewriting the whole file rather than appending is what makes
       turning it off possible at all. */
    while (file[at]) {
        long start = at;
        long length;
        while (file[at] && file[at] != '\n') at++;
        length = at - start;
        if (file[at]) at++;
        if (line_matches(file + start, START_COMMAND)) continue;
        if (!length) continue;
        for (long index = 0; index < length && out < FILE_MAX - 2; index++)
            rebuilt[out++] = file[start + index];
        rebuilt[out++] = '\n';
    }
    if (wanted) {
        const char* command = START_COMMAND;
        for (int index = 0; command[index] && out < FILE_MAX - 2; index++)
            rebuilt[out++] = command[index];
        rebuilt[out++] = '\n';
    }
    rebuilt[out] = 0;
    return write_file(AUTOEXEC, rebuilt, out);
}

/* Cancel means cancel.
 *
 * It used to mean "skip this question", so the button was drawn, and pressing
 * it moved on to the next question exactly as OK did with the default answer -
 * a Cancel with no way to cancel. Now it stops.
 *
 * What has already been answered stays answered: those settings were applied
 * when they were given, and pretending otherwise would mean undoing work
 * somebody deliberately did. What does not happen is the rest of the
 * questions, and the flag that says these were asked - so the machine asks
 * again next time rather than quietly deciding on somebody's behalf. */
static int cancelled(void) {
    dialog_message("Cancelled",
        "Nothing else was changed. Anything already answered has been kept.\n"
        "Run CMDRCFG again to finish; until then Koi-Commander will ask "
        "these questions the next time it starts.");
    dialog_end();
    koi_print("Cancelled. Run CMDRCFG again to finish.\n");
    return 0;
}

int main(void) {
    static const char* const levels[] = { "Quiet", "Normal", "Loud" };
    static const char* const notes[] = {
        "25 percent - headphones", "60 percent", "100 percent - speakers"
    };
    static const int percent[] = { 25, 60, 100 };
    int autostart;
    int loudness;

    dialog_begin("Koi-Commander - settings");

    dialog_message("Koi-Commander",
        "Koi-Commander is a file manager with two panels, in the line Norton "
        "started. It is a package, not part of the system: it can be removed "
        "and what is left is the same Koi-DOS.\n"
        "A few questions before it is used for the first time.");

    autostart = dialog_yesno("Starting up",
        "Start Koi-Commander automatically when the machine boots?\n"
        "This adds one line to AUTOEXEC.BAT, and answering No later takes "
        "it out again.", autostart_is_set());
    if (autostart < 0) return cancelled();
    if (!set_autostart(autostart))
        dialog_message("Starting up",
            "AUTOEXEC.BAT could not be written. The setting was not "
            "saved; everything else still works.");

    loudness = dialog_menu("Sound",
        "How loud should this machine be?\n"
        "The default has been described, by somebody wearing headphones, "
        "as too loud.", levels, notes, 3, 1, 0);
    if (loudness < 0) return cancelled();
    {
        char text[8];
        koi_snprintf(text, sizeof(text), "%d", percent[loudness]);
        koi_sound_volume(percent[loudness] * 255 / 100);
        /* Written as well as applied, so it survives the next boot - which it
           did not, and the machine came back at its default loudness every
           time somebody turned it on. */
        settings_set("SOUND", "volume", text);
    }

    {
        char summary[256];
        koi_snprintf(summary, sizeof(summary),
                     "Koi-Commander %s start with the machine, and the volume is %s.\n"
                     "Run CMDRCFG at any time to change these, or start it now "
                     "by typing \\COMMANDER\\COMMANDER.",
                     autostart == 1 ? "will" : "will not",
                     levels[loudness]);
        dialog_message("Done", summary);
    }

    /* The flag the commander looks at on startup, so the questions are
       asked once and then never again unless somebody asks for them. */
    settings_set("CMDR", "configured", "1");
    dialog_end();
    koi_print("Koi-Commander is ready. Type \\COMMANDER\\COMMANDER to start it.\n");
    return 0;
}
