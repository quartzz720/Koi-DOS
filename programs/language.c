#include "language.h"
#include "settings.h"

static int current = LANGUAGE_EN;

/* English, Russian, Ukrainian - in that order, every row, no gaps. A missing
   translation would be a null pointer somewhere in a drawing loop, so a phrase
   that has not been translated carries the English rather than nothing. */
static const char* phrases[SAY_COUNT][LANGUAGE_COUNT] = {
    /* SAY_DESKTOP_TITLE  */ { "Mizu-DOS 0.5", "Mizu-DOS 0.5", "Mizu-DOS 0.5" },
    /* SAY_MENU_SYSTEM    */ { "System", "Система", "Система" },
    /* SAY_MENU_RUN       */ { "Run", "Запуск", "Запуск" },
    /* SAY_MENU_VIEW      */ { "View", "Вид", "Вигляд" },
    /* SAY_MENU_FILE      */ { "File", "Файл", "Файл" },
    /* SAY_MENU_FORMAT    */ { "Format", "Формат", "Формат" },
    /* SAY_MENU_OPTIONS   */ { "Options", "Настройки", "Налаштування" },
    /* SAY_ABOUT          */ { "About Mizu", "О системе", "Про систему" },
    /* SAY_EXIT           */ { "Exit to DOS", "Выход в DOS", "Вихід у DOS" },
    /* SAY_CONTROL_PANEL  */ { "Control Panel", "Панель управления",
                               "Панель керування" },
    /* SAY_CLOCK          */ { "Clock & Date", "Часы и дата", "Годинник і дата" },
    /* SAY_COMMANDER      */ { "Koi-Commander", "Koi-Commander", "Koi-Commander" },
    /* SAY_TILE           */ { "Tile windows", "Разложить окна",
                               "Розкласти вікна" },
    /* SAY_NOTEEDIT       */ { "NoteEdit", "Блокнот", "Блокнот" },
    /* SAY_SAVE           */ { "Save", "Сохранить", "Зберегти" },
    /* SAY_CLOSE          */ { "Close", "Закрыть", "Закрити" },
    /* SAY_BOLD           */ { "Bold", "Жирный", "Жирний" },
    /* SAY_ITALIC         */ { "Italic", "Курсив", "Курсив" },
    /* SAY_UNDERLINE      */ { "Underline", "Подчёркнутый", "Підкреслений" },
    /* SAY_PLAIN          */ { "Plain", "Обычный", "Звичайний" },
    /* SAY_TWO_PANELS     */ { "two panels", "две панели", "дві панелі" },
    /* SAY_WRITE_TEXT     */ { "write text", "писать текст", "писати текст" },
    /* SAY_AND_A_DATE     */ { "and a date", "и дата", "і дата" },
    /* SAY_THIS_SYSTEM    */ { "this system", "об этой ОС", "про цю ОС" },
    /* SAY_ONE_AT_A_TIME_1*/ { "Windows belong to one program:",
                               "Окна принадлежат одной программе:",
                               "Вікна належать одній програмі:" },
    /* SAY_ONE_AT_A_TIME_2*/ { "the system holds one at a time.",
                               "система держит одну за раз.",
                               "система тримає одну за раз." },
    /* SAY_FREE           */ { "KiB free", "КиБ свободно", "КіБ вільно" },
    /* SAY_COULD_NOT_SAVE */ { "could not save", "не удалось сохранить",
                               "не вдалося зберегти" }
};

static const char* names[LANGUAGE_COUNT] = {
    "English", "Русский", "Українська"
};

static const char* codes[LANGUAGE_COUNT] = { "en", "ru", "uk" };

void language_load(void) {
    char code[8];

    current = LANGUAGE_EN;
    if (!settings_get("SYSTEM", "language", code, sizeof(code))) return;
    for (int index = 0; index < LANGUAGE_COUNT; index++)
        if (code[0] == codes[index][0] && code[1] == codes[index][1])
            current = index;
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
