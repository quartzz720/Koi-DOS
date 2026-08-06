#include "koi.h"

/* Change the colours the shell uses, and remember the choice.
 *
 * Two halves that could have been one and deliberately are not: the change is
 * applied through a system call, and it is made to stick by writing a file the
 * kernel reads at boot. Nothing here reaches into kernel state to persist
 * anything, and the kernel does not have to know this program exists. */

#define CONFIG_PATH "\\BOOT\\userspace.cfg"

static const char* color_names[16] = {
    "black", "blue", "green", "cyan",
    "red", "magenta", "brown", "lightgray",
    "darkgray", "lightblue", "lightgreen", "lightcyan",
    "lightred", "lightmagenta", "yellow", "white"
};

typedef struct {
    const char* name;
    int foreground;
    int background;
    int prompt;
    const char* description;
} PRESET;

/* Kept small and opinionated. Each one is a look somebody actually shipped,
   rather than an arbitrary pair of colours. */
static const PRESET presets[] = {
    { "dos",    KOI_LIGHT_GRAY,  KOI_BLUE,  KOI_LIGHT_GREEN,
      "the default: grey on blue" },
    { "mono",   KOI_LIGHT_GRAY,  KOI_BLACK, KOI_WHITE,
      "grey on black, like a plain console" },
    { "amber",  KOI_YELLOW,      KOI_BLACK, KOI_BROWN,
      "amber phosphor" },
    { "green",  KOI_LIGHT_GREEN, KOI_BLACK, KOI_GREEN,
      "green phosphor" },
    { "paper",  KOI_BLACK,       KOI_LIGHT_GRAY, KOI_BLUE,
      "dark on light, for a bright room" },
    { "night",  KOI_LIGHT_CYAN,  KOI_BLACK, KOI_CYAN,
      "cyan on black" },
};

#define PRESET_COUNT (int)(sizeof(presets) / sizeof(presets[0]))

static long length_of(const char* text) {
    long length = 0;
    while (text[length]) length++;
    return length;
}

static char lower(char character) {
    return character >= 'A' && character <= 'Z' ? (char)(character + 32) : character;
}

static int same(const char* left, const char* right) {
    while (*left && *right) {
        if (lower(*left) != lower(*right)) return 0;
        left++;
        right++;
    }
    return !*left && !*right;
}

static int parse_color(const char* text) {
    int value = 0;
    int digits = 0;

    if (!text || !*text) return -1;
    for (int index = 0; index < 16; index++)
        if (same(text, color_names[index])) return index;
    for (const char* cursor = text; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') return -1;
        value = value * 10 + (*cursor - '0');
        digits++;
    }
    return (digits && value <= 15) ? value : -1;
}

/* Pull the next whitespace-separated word out of `text`. */
static const char* next_word(const char* text, char* word, long capacity) {
    long length = 0;

    while (*text == ' ' || *text == '\t') text++;
    while (*text && *text != ' ' && *text != '\t' && length + 1 < capacity)
        word[length++] = *text++;
    word[length] = 0;
    return text;
}

static void append(char* buffer, long* position, const char* text) {
    while (*text) buffer[(*position)++] = *text++;
}

static int save(int foreground, int background, int prompt) {
    static char buffer[512];
    long position = 0;
    long handle;
    long written;

    append(buffer, &position,
           "# Koi-DOS user settings.\r\n"
           "# Written by color.exe; read by the kernel at boot.\r\n"
           "# Colours may be names or numbers 0-15.\r\n\r\n");
    append(buffer, &position, "foreground = ");
    append(buffer, &position, color_names[foreground]);
    append(buffer, &position, "\r\nbackground = ");
    append(buffer, &position, color_names[background]);
    append(buffer, &position, "\r\nprompt = ");
    append(buffer, &position, color_names[prompt]);
    append(buffer, &position, "\r\n");

    handle = koi_open(CONFIG_PATH, OPEN_WRITE);
    if (handle == SYSCALL_ERROR) return 0;
    written = koi_write(handle, buffer, position);
    koi_close(handle);
    return written == position;
}

static void show_swatch(void) {
    koi_print("  ");
    for (int index = 0; index < 16; index++) {
        koi_color(index, KOI_BLACK);
        koi_putchar((char)0xDB);
        koi_putchar((char)0xDB);
    }
    /* Put the drawing colour back where it was. Leaving it on the last swatch
       colour would tint everything printed afterwards. Asking the kernel to
       change nothing is the cheapest way to say "use the theme again". */
    koi_theme(-1, -1, -1, -1);

    koi_print("\n  ");
    for (int index = 0; index < 16; index++) {
        koi_print_dec((koi_uint64)index);
        if (index < 10) koi_putchar(' ');
    }
    koi_print("\n");
}

static void usage(void) {
    koi_print("Changes the colours the shell uses, and remembers them.\n\n");
    koi_print("  color <text> <background>   set both\n");
    koi_print("  color /t <colour>           set the text colour only\n");
    koi_print("  color /b <colour>           set the background only\n");
    koi_print("  color /p <colour>           set the prompt colour only\n");
    koi_print("  color <preset>              use a preset\n");
    koi_print("  color /l                    list the colours\n\n");

    koi_print("Presets:\n");
    for (int index = 0; index < PRESET_COUNT; index++) {
        koi_print("  ");
        koi_print(presets[index].name);
        for (long pad = length_of(presets[index].name); pad < 10; pad++)
            koi_putchar(' ');
        koi_print(presets[index].description);
        koi_print("\n");
    }

    koi_print("\nColours are names or numbers 0-15:\n");
    show_swatch();
    koi_print("\nThe choice is written to ");
    koi_print(CONFIG_PATH);
    koi_print("\nand applied again at every boot.\n");
}

int main(const char* arguments) {
    char word[64];
    const char* cursor = arguments ? arguments : "";
    int foreground = -1;
    int background = -1;
    int prompt = -1;

    cursor = next_word(cursor, word, sizeof(word));
    if (!word[0]) { usage(); return 0; }

    if (same(word, "/l") || same(word, "/list")) {
        show_swatch();
        return 0;
    }

    /* A preset names all three at once. */
    for (int index = 0; index < PRESET_COUNT; index++) {
        if (!same(word, presets[index].name)) continue;
        foreground = presets[index].foreground;
        background = presets[index].background;
        prompt = presets[index].prompt;
        break;
    }

    if (foreground < 0 && word[0] == '/') {
        char value[64];
        int color;

        next_word(cursor, value, sizeof(value));
        color = parse_color(value);
        if (color < 0) {
            koi_print("color: expected a colour after ");
            koi_print(word);
            koi_print("\n");
            return 1;
        }
        if (same(word, "/t")) foreground = color;
        else if (same(word, "/b")) background = color;
        else if (same(word, "/p")) prompt = color;
        else {
            koi_print("color: unknown option ");
            koi_print(word);
            koi_print("\n");
            return 1;
        }
    } else if (foreground < 0) {
        char second[64];

        foreground = parse_color(word);
        if (foreground < 0) {
            koi_print("color: ");
            koi_print(word);
            koi_print(" is not a colour or a preset. Run color for help.\n");
            return 1;
        }
        next_word(cursor, second, sizeof(second));
        if (second[0]) {
            background = parse_color(second);
            if (background < 0) {
                koi_print("color: ");
                koi_print(second);
                koi_print(" is not a colour.\n");
                return 1;
            }
        }
    }

    /* Refusing this is friendlier than letting somebody paint the screen a
       single colour and lose the prompt they would need to undo it. */
    if (foreground >= 0 && background >= 0 && foreground == background) {
        koi_print("color: text and background would be the same colour.\n");
        return 1;
    }

    /* Apply, and take back the whole resulting theme. Writing only the part
       that changed would lose the rest: `color /b black` after `color amber`
       would otherwise save a default foreground over the amber one. */
    {
        long theme = koi_theme(foreground, background, prompt, -1);
        int saved = save(KOI_THEME_FOREGROUND(theme),
                         KOI_THEME_BACKGROUND(theme),
                         KOI_THEME_PROMPT(theme));

        /* Clear before reporting. A text console only paints a cell when it
           writes a character to it, so without this the new background would
           appear one character at a time and the old one would stay behind
           everywhere else - which looks like a bug rather than a change. */
        koi_cls();

        if (!saved) {
            koi_print("Colours changed, but ");
            koi_print(CONFIG_PATH);
            koi_print(" could not be written.\n");
            koi_print("They will go back to the previous ones at the next boot.\n");
            return 1;
        }
    }

    koi_print("Colours changed and saved.\n");
    return 0;
}
