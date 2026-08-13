#include "layout.h"
#include "serial.h"

/* One row per layout, in the order of the ASCII characters below it.
 *
 * Written as the QWERTY character each letter replaces, because that is how
 * anybody who uses these layouts knows them: й is where q is. A table indexed
 * by scancode would say the same thing in a form nobody could check by eye,
 * and a layout table that cannot be checked by eye is a layout table with a
 * letter in the wrong place.
 *
 * Zero means "this key is not moved" - the digits and most punctuation, which
 * every one of these layouts leaves where it found it.
 */
static const char* const source =
    "qwertyuiop[]asdfghjkl;'zxcvbnm,./`"
    "QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?~";

/* ЙЦУКЕН, the Russian layout every Russian keyboard has been printed with
   since typewriters. */
static const boot_uint32_t russian[] = {
    0x0439,0x0446,0x0443,0x043A,0x0435,0x043D,0x0433,0x0448,0x0449,0x0437,
    0x0445,0x044A,0x0444,0x044B,0x0432,0x0430,0x043F,0x0440,0x043E,0x043B,
    0x0434,0x0436,0x044D,0x044F,0x0447,0x0441,0x043C,0x0438,0x0442,0x044C,
    0x0431,0x044E,0x002E,0x0451,
    0x0419,0x0426,0x0423,0x041A,0x0415,0x041D,0x0413,0x0428,0x0429,0x0417,
    0x0425,0x042A,0x0424,0x042B,0x0412,0x0410,0x041F,0x0420,0x041E,0x041B,
    0x0414,0x0416,0x042D,0x042F,0x0427,0x0421,0x041C,0x0418,0x0422,0x042C,
    0x0411,0x042E,0x002C,0x0401
};

/* The same shape with the four letters that are Ukrainian and not Russian: і
   where ы is, ї where ъ is, є where э is, and ґ on the key Russian gives to ё.
   Getting those four wrong is the difference between a layout somebody can use
   and one they have to work around. */
static const boot_uint32_t ukrainian[] = {
    0x0439,0x0446,0x0443,0x043A,0x0435,0x043D,0x0433,0x0448,0x0449,0x0437,
    0x0445,0x0457,0x0444,0x0456,0x0432,0x0430,0x043F,0x0440,0x043E,0x043B,
    0x0434,0x0436,0x0454,0x044F,0x0447,0x0441,0x043C,0x0438,0x0442,0x044C,
    0x0431,0x044E,0x002E,0x0491,
    0x0419,0x0426,0x0423,0x041A,0x0415,0x041D,0x0413,0x0428,0x0429,0x0417,
    0x0425,0x0407,0x0424,0x0406,0x0412,0x0410,0x041F,0x0420,0x041E,0x041B,
    0x0414,0x0416,0x0404,0x042F,0x0427,0x0421,0x041C,0x0418,0x0422,0x042C,
    0x0411,0x042E,0x002C,0x0490
};

/* The Greek layout, which is nearly QWERTY with Greek letters on the keys
   whose names sound alike - ς on w because it is the final sigma and σ is on
   s. Keys with no Greek letter keep what they had. */
static const boot_uint32_t greek[] = {
    0x003B,0x03C2,0x03B5,0x03C1,0x03C4,0x03C5,0x03B8,0x03B9,0x03BF,0x03C0,
    0x005B,0x005D,0x03B1,0x03C3,0x03B4,0x03C6,0x03B3,0x03B7,0x03BE,0x03BA,
    0x03BB,0x0384,0x03B6,0x03C7,0x03C8,0x03C9,0x03B2,0x03BD,0x03BC,0x002C,
    0x002E,0x002F,0x0060,0x0060,
    0x003A,0x03A3,0x0395,0x03A1,0x03A4,0x03A5,0x0398,0x0399,0x039F,0x03A0,
    0x007B,0x007D,0x0391,0x03A3,0x0394,0x03A6,0x0393,0x0397,0x039E,0x039A,
    0x039B,0x00A8,0x0396,0x03A7,0x03A8,0x03A9,0x0392,0x039D,0x039C,0x003C,
    0x003E,0x003F,0x007E,0x007E
};

static int current = LAYOUT_EN;
static int alternate = LAYOUT_EN;

void layout_set_alternate(int layout) {
    if (layout < 0 || layout >= LAYOUT_COUNT) return;
    alternate = layout;
}

int layout_alternate(void) { return alternate; }
int layout_current(void) { return current; }

void layout_select(int layout) {
    if (layout < 0 || layout >= LAYOUT_COUNT) return;
    current = layout;
}

void layout_toggle(void) {
    current = (current == LAYOUT_EN) ? alternate : LAYOUT_EN;
    serial_write("LAYOUT: now ");
    serial_write(layout_name(current));
    serial_write("\n");
}

static int gesture_armed;

static int gesture_allowed;

void layout_gesture_enable(int enabled) {
    gesture_allowed = enabled != 0;
    /* Switching it off puts the keyboard back to English rather than leaving
       whatever was selected: a program that ended while the other layout was
       on would otherwise hand the shell a keyboard typing in Cyrillic. */
    if (!gesture_allowed) layout_select(LAYOUT_EN);
}

int layout_gesture_enabled(void) { return gesture_allowed; }

void layout_gesture(int shift_down, int alt_down) {
    if (!gesture_allowed) return;
    if (shift_down && alt_down) {
        if (!gesture_armed) { gesture_armed = 1; layout_toggle(); }
        return;
    }
    gesture_armed = 0;
}

boot_uint32_t layout_map(int character) {
    const boot_uint32_t* table;

    if (current == LAYOUT_RU) table = russian;
    else if (current == LAYOUT_UK) table = ukrainian;
    else if (current == LAYOUT_GR) table = greek;
    else return (boot_uint32_t)character;

    for (int index = 0; source[index]; index++)
        if (source[index] == character) return table[index];
    return (boot_uint32_t)character;
}

const char* layout_name(int layout) {
    switch (layout) {
    case LAYOUT_RU: return "Russian";
    case LAYOUT_UK: return "Ukrainian";
    case LAYOUT_GR: return "Greek";
    default: return "English";
    }
}
