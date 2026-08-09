#include "koi.h"
#include "dialog.h"
#include "settings.h"
#include "language.h"

/* The questions Mizu asks the first time.
 *
 * On the console rather than in Mizu's own windows, and that is not laziness:
 * these are asked before Mizu has ever drawn anything, on a machine where the
 * thing being configured is the thing that would have to draw the dialogue.
 *
 * The language comes first, and everything after it is asked in that language.
 * Asking somebody in English which language they read is a small rudeness that
 * every installer commits and none of them needs to.
 */

#define AUTOEXEC "\\AUTOEXEC.BAT"
#define START_COMMAND "\\MIZU\\MIZU"
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

static int line_matches(const char* line, const char* command) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '@') line++;
    for (int index = 0; command[index]; index++)
        if (toupper((unsigned char)line[index]) !=
            toupper((unsigned char)command[index])) return 0;
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

/* Both graphical shells claim the same line of AUTOEXEC.BAT, and only one of
   them can have it: a machine that starts two full-screen programs at boot
   starts neither usefully. So turning this on takes the other one out. */
static int set_autostart(int wanted) {
    static char rebuilt[FILE_MAX];
    long out = 0;
    long at = 0;
    long handle;

    if (read_file(AUTOEXEC, file, FILE_MAX) < 0) file[0] = 0;

    while (file[at]) {
        long start = at;
        long length;

        while (file[at] && file[at] != '\n') at++;
        length = at - start;
        if (file[at]) at++;
        if (line_matches(file + start, START_COMMAND)) continue;
        if (wanted && line_matches(file + start, "\\COMMANDER\\COMMANDER")) continue;
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

    koi_remove(AUTOEXEC);
    handle = koi_open(AUTOEXEC, OPEN_WRITE);
    if (handle < 0) return 0;
    if (koi_write(handle, rebuilt, out) != out) { koi_close(handle); return 0; }
    koi_close(handle);
    return 1;
}

/* The questions, one row per language rather than through `say`: this program
   runs before the choice has been made, so it has to be able to speak all
   three at once. */
static const char* const welcome[LANGUAGE_COUNT] = {
    "Mizu is a desktop with windows. It is a package, not part of the system: "
    "it can be removed and what is left is the same Koi-DOS.\n"
    "A few questions before it is used for the first time.",
    "Mizu - это рабочий стол с окнами. Это пакет, а не часть системы: "
    "его можно удалить, и останется тот же Koi-DOS.\n"
    "Несколько вопросов перед первым запуском.",
    "Mizu - це робочий стіл з вікнами. Це пакет, а не частина системи: "
    "його можна видалити, і залишиться той самий Koi-DOS.\n"
    "Кілька питань перед першим запуском."
};

static const char* const ask_autostart[LANGUAGE_COUNT] = {
    "Start Mizu automatically when the machine boots?\n"
    "This adds one line to AUTOEXEC.BAT, and answering No later takes it out "
    "again.",
    "Запускать Mizu автоматически при загрузке машины?\n"
    "Это добавит одну строку в AUTOEXEC.BAT; ответ Нет позже уберёт её.",
    "Запускати Mizu автоматично під час завантаження?\n"
    "Це додасть один рядок до AUTOEXEC.BAT; відповідь Ні згодом прибере його."
};

static const char* const ask_sound[LANGUAGE_COUNT] = {
    "How loud should this machine be?",
    "Насколько громкой должна быть эта машина?",
    "Наскільки гучною має бути ця машина?"
};

static const char* const levels[LANGUAGE_COUNT][3] = {
    { "Quiet", "Normal", "Loud" },
    { "Тихо", "Обычно", "Громко" },
    { "Тихо", "Звичайно", "Гучно" }
};

static const char* const done[LANGUAGE_COUNT] = {
    "Mizu is ready. Type \\MIZU\\MIZU to start it, or run MIZUCFG again to "
    "change any of this.",
    "Mizu готов. Наберите \\MIZU\\MIZU, чтобы запустить, или запустите MIZUCFG "
    "снова, чтобы это изменить.",
    "Mizu готовий. Наберіть \\MIZU\\MIZU, щоб запустити, або запустіть MIZUCFG "
    "знову, щоб це змінити."
};

int main(void) {
    static const char* names[LANGUAGE_COUNT];
    static const int percent[] = { 25, 60, 100 };
    int language;
    int autostart;
    int loudness;

    language_load();
    dialog_begin("Mizu 0.5");

    for (int index = 0; index < LANGUAGE_COUNT; index++)
        names[index] = language_name(index);
    language = dialog_menu("Language / Язык / Мова",
                           "Which language should this machine speak?",
                           names, 0, LANGUAGE_COUNT, language_current());
    if (language < 0) language = language_current();
    language_set(language);

    dialog_message("Mizu", welcome[language]);

    autostart = dialog_yesno(say(SAY_MENU_SYSTEM), ask_autostart[language],
                             autostart_is_set());
    if (autostart >= 0 && !set_autostart(autostart))
        dialog_message("AUTOEXEC.BAT",
                       "AUTOEXEC.BAT could not be written. Nothing else is "
                       "affected.");

    loudness = dialog_menu("Sound", ask_sound[language], levels[language], 0,
                           3, 1);
    if (loudness >= 0) {
        char text[8];
        koi_snprintf(text, sizeof(text), "%d", percent[loudness]);
        koi_sound_volume(percent[loudness] * 255 / 100);
        settings_set("SOUND", "volume", text);
    }

    settings_set("MIZU", "configured", "1");
    dialog_message("Mizu", done[language]);
    dialog_end();
    return 0;
}
