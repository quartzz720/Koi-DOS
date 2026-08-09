#include "font.h"

/* Finding a glyph, and reading UTF-8.
 *
 * The shapes live in font_glyphs.c and are somebody else's; everything here is
 * ours. A byte can index 256 shapes and no more, and which 256 was decided in
 * 1981 - Russian, Ukrainian and Greek do not fit beside the box drawing this
 * system draws frames with, and choosing between them is choosing which tester
 * gets a screen of rubbish. So text is UTF-8 and glyphs are found by code
 * point.
 */

static const boot_uint8_t blank[FONT_HEIGHT] = { 0 };

const boot_uint8_t* font_glyph(boot_uint32_t codepoint) {
    if (codepoint < 256) return font_8x16[codepoint];

    for (int index = 0; index < font_beyond_count; index++)
        if (font_beyond[index].codepoint == codepoint)
            return font_beyond[index].rows;

    /* A letter we do not carry leaves a gap the width of a letter. Text that
       silently closes up over a missing glyph is text somebody reads as
       correct. */
    return blank;
}

boot_uint32_t font_decode(const char* text, int* length) {
    const boot_uint8_t* raw = (const boot_uint8_t*)text;
    boot_uint32_t value;
    int extra_bytes;

    if (raw[0] < 0x80) { *length = 1; return raw[0]; }
    if ((raw[0] & 0xE0) == 0xC0) { value = raw[0] & 0x1F; extra_bytes = 1; }
    else if ((raw[0] & 0xF0) == 0xE0) { value = raw[0] & 0x0F; extra_bytes = 2; }
    else if ((raw[0] & 0xF8) == 0xF0) { value = raw[0] & 0x07; extra_bytes = 3; }
    else { *length = 1; return raw[0]; }   /* not a start byte; pass it through */

    for (int index = 1; index <= extra_bytes; index++) {
        if ((raw[index] & 0xC0) != 0x80) { *length = 1; return raw[0]; }
        value = (value << 6) | (raw[index] & 0x3F);
    }
    *length = extra_bytes + 1;
    return value;
}
