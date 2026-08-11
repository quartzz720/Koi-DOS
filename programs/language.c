#include "language.h"
#include "settings.h"

static int current = LANGUAGE_EN;

/* English, Russian, Ukrainian - in that order, every row, no gaps. A missing
   translation would be a null pointer somewhere in a drawing loop, so a phrase
   that has not been translated carries the English rather than nothing. */
static const char* phrases[SAY_COUNT][LANGUAGE_COUNT] = {
    /* SAY_DESKTOP_TITLE  */ { "Mizu 0.51", "Mizu 0.51", "Mizu 0.51", "Mizu 0.51" },
    /* DIALOG_YES         */ { "Yes", "Да", "Так", "Ναι" },
    /* DIALOG_NO          */ { "No", "Нет", "Ні", "Όχι" },
    /* DIALOG_OK          */ { "OK", "OK", "OK", "OK" },
    /* DIALOG_CANCEL      */ { "Cancel", "Отмена", "Скасувати", "Ακύρωση" },
    /* SAY_MENU_SYSTEM    */ { "System", "Система", "Система", "Σύστημα" },
    /* SAY_MENU_RUN       */ { "Run", "Запуск", "Запуск", "Εκτέλεση" },
    /* SAY_MENU_VIEW      */ { "View", "Вид", "Вигляд", "Προβολή" },
    /* SAY_MENU_FILE      */ { "File", "Файл", "Файл", "Αρχείο" },
    /* SAY_MENU_FORMAT    */ { "Format", "Формат", "Формат", "Μορφοποίηση" },
    /* SAY_MENU_OPTIONS   */ { "Options", "Настройки", "Налаштування", "Επιλογές" },
    /* SAY_ABOUT          */ { "About Mizu", "О системе", "Про систему", "Πληροφορίες" },
    /* SAY_EXIT           */ { "Exit to DOS", "Выход в DOS", "Вихід у DOS", "Έξοδος στο DOS" },
    /* SAY_CONTROL_PANEL  */ { "Control Panel", "Панель управления",
                               "Панель керування", "Πίνακας ελέγχου" },
    /* SAY_CLOCK          */ { "Date & time", "Дата и время", "Дата і час", "Ημερομηνία και ώρα" },
    /* SAY_COMMANDER      */ { "Koi Commander", "Koi Commander", "Koi Commander", "Koi Commander" },
    /* SAY_TILE           */ { "Tile windows", "Разложить окна",
                               "Розкласти вікна", "Τακτοποίηση παραθύρων" },
    /* SAY_NOTEEDIT       */ { "NoteEdit", "Блокнот", "Блокнот", "NoteEdit" },
    /* SAY_SAVE           */ { "Save", "Сохранить", "Зберегти", "Αποθήκευση" },
    /* SAY_CLOSE          */ { "Close", "Закрыть", "Закрити", "Κλείσιμο" },
    /* SAY_BOLD           */ { "Bold", "Жирный", "Жирний", "Έντονα" },
    /* SAY_ITALIC         */ { "Italic", "Курсив", "Курсив", "Πλάγια" },
    /* SAY_UNDERLINE      */ { "Underline", "Подчёркнутый", "Підкреслений", "Υπογράμμιση" },
    /* SAY_PLAIN          */ { "Plain", "Обычный", "Звичайний", "Απλό" },
    /* SAY_ONE_AT_A_TIME_1*/ { "Windows belong to one program:",
                               "Окна принадлежат одной программе:",
                               "Вікна належать одній програмі:", "Τα παράθυρα ανήκουν σε ένα πρόγραμμα:" },
    /* SAY_ONE_AT_A_TIME_2*/ { "the system holds one at a time.",
                               "система держит одну за раз.",
                               "система тримає одну за раз.", "το σύστημα διατηρεί ένα κάθε φορά." },
    /* SAY_FREE           */ { "KiB free", "КиБ свободно", "КіБ вільно", "KiB διαθέσιμα" },
    /* SAY_COULD_NOT_SAVE */ { "could not save", "не удалось сохранить",
                               "не вдалося зберегти", "δεν ήταν δυνατή η αποθήκευση" }
};

static const char* const weekdays[LANGUAGE_COUNT][7] = {
    { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" },
    { "Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб" },
    { "Нд", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб" },
    { "Κυ", "Δε", "Τρ", "Τε", "Πέ", "Πα", "Σά" }
};

/* Genitive in Russian and Ukrainian, because a date reads "the ninth of
   August" and the month is the thing the ninth is of. A table of nominative
   month names would be correct as a list and wrong in every date it appeared
   in, which is the kind of translation that tells a reader the software was
   not written for them. */
static const char* const months[LANGUAGE_COUNT][12] = {
    { "January", "February", "March", "April", "May", "June",
      "July", "August", "September", "October", "November", "December" },
    { "января", "февраля", "марта", "апреля", "мая", "июня",
      "июля", "августа", "сентября", "октября", "ноября", "декабря" },
    { "січня", "лютого", "березня", "квітня", "травня", "червня",
      "липня", "серпня", "вересня", "жовтня", "листопада", "грудня" },
    { "Ιανουαρίου", "Φεβρουαρίου", "Μαρτίου", "Απριλίου", "Μαΐου", "Ιουνίου",
      "Ιουλίου", "Αυγούστου", "Σεπτεμβρίου", "Οκτωβρίου", "Νοεμβρίου", "Δεκεμβρίου" }
};

const char* language_weekday(int index) {
    return (index >= 0 && index < 7) ? weekdays[current][index] : "";
}

const char* language_month(int month) {
    return (month >= 1 && month <= 12) ? months[current][month - 1] : "";
}

static const char* names[LANGUAGE_COUNT] = {
    "English", "Русский", "Українська", "Ελληνικά"
};

static const char* codes[LANGUAGE_COUNT] = { "en", "ru", "uk", "el" };

void language_load(void) {
    char code[8];

    current = LANGUAGE_EN;
    if (!settings_get("SYSTEM", "language", code, sizeof(code))) return;
    for (int index = 0; index < LANGUAGE_COUNT; index++)
        if (code[0] == codes[index][0] && code[1] == codes[index][1])
            current = index;
}

void language_preview(int language) {
    if (language < 0 || language >= LANGUAGE_COUNT) return;
    current = language;
}

void language_set(int language) {
    if (language < 0 || language >= LANGUAGE_COUNT) return;
    current = language;
    settings_set("SYSTEM", "language", codes[language]);
}

int language_current(void) { return current; }

const char* language_name(int language) {
    return (language >= 0 && language < LANGUAGE_COUNT) ? names[language] : "?";
}

const char* say(int phrase) {
    const char* text;

    if (phrase < 0 || phrase >= SAY_COUNT) return "";
    text = phrases[phrase][current];
    return text ? text : phrases[phrase][LANGUAGE_EN];
}

int language_columns(const char* text) {
    int columns = 0;

    for (int index = 0; text[index]; index++)
        if (((unsigned char)text[index] & 0xC0) != 0x80) columns++;
    return columns;
}
