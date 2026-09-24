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

/* The shapes code page 437 already has, under the names the rest of the world
 * calls them by.
 *
 * The first 256 shapes are code page 437, and its boxes and blocks are exactly
 * the ones ASCII art is made of - but a person writing art types `█`, which is
 * U+2588, and nothing here looked past 255 for it. So the glyph was present,
 * correct, and unreachable: art pasted into a program came out as a screenful
 * of gaps, and the only clue was that the same drawing worked in the editor it
 * was written in.
 *
 * A translation rather than more glyphs. The shapes are in font_glyphs.c and
 * are not ours to copy about; which byte holds each one is a fact about a code
 * page from 1981. */
typedef struct { boot_uint32_t codepoint; boot_uint8_t byte; } CP437_NAME;

static const CP437_NAME cp437_names[] = {
    /* Blocks and shades - what block art is drawn with. */
    { 0x2591, 0xB0 }, { 0x2592, 0xB1 }, { 0x2593, 0xB2 }, { 0x2588, 0xDB },
    { 0x2584, 0xDC }, { 0x258C, 0xDD }, { 0x2590, 0xDE }, { 0x2580, 0xDF },
    /* Single-line box drawing. */
    { 0x2500, 0xC4 }, { 0x2502, 0xB3 }, { 0x250C, 0xDA }, { 0x2510, 0xBF },
    { 0x2514, 0xC0 }, { 0x2518, 0xD9 }, { 0x251C, 0xC3 }, { 0x2524, 0xB4 },
    { 0x252C, 0xC2 }, { 0x2534, 0xC1 }, { 0x253C, 0xC5 },
    /* And double-line, which is what a DOS dialogue box is made of. */
    { 0x2550, 0xCD }, { 0x2551, 0xBA }, { 0x2554, 0xC9 }, { 0x2557, 0xBB },
    { 0x255A, 0xC8 }, { 0x255D, 0xBC }, { 0x2560, 0xCC }, { 0x2563, 0xB9 },
    { 0x2566, 0xCB }, { 0x2569, 0xCA }, { 0x256C, 0xCE },
    /* The odds and ends with a glyph already in the page. */
    { 0x25A0, 0xFE }, { 0x00A0, 0xFF },
    /* Typography, which arrives the moment anything reads the web.
     *
     * An em dash, a curly quote and an ellipsis are on every page written
     * since about 1998, and none of them is in a code page from 1981. The
     * choice is a gap or a near-enough shape; a sentence with holes punched
     * through its punctuation reads as broken text, and one with a hyphen
     * where a dash should be reads as a sentence.
     *
     * Only code points above 255 are listed. Below that, a byte is a byte:
     * the shapes are code page 437's, and a program printing a box-drawing
     * character straight out of a DOS file must go on getting the box. */
    { 0x2022, 0xFA }, { 0x25CF, 0xFA }, { 0x2219, 0xFA },
    { 0x2013, 0x2D }, { 0x2014, 0x2D }, { 0x2015, 0x2D }, { 0x2212, 0x2D },
    { 0x2018, 0x27 }, { 0x2019, 0x27 }, { 0x201B, 0x27 },
    { 0x201C, 0x22 }, { 0x201D, 0x22 }, { 0x201E, 0x22 },
    { 0x201A, 0x2C }, { 0x2026, 0x2E }, { 0x2039, 0x3C }, { 0x203A, 0x3E },
    { 0x2116, 0x4E },                              /* № , as an N */
    { 0x20AC, 0x45 },                              /* € , as an E */
    { 0x2122, 0x54 },                              /* ™ , as a T */
    { 0x221A, 0xFB },
    /* Arrows as ASCII rather than as the code page's own, because the shapes
       in font_glyphs.c stop at the printable range: 0x18 to 0x1B are the four
       arrows in code page 437 and are not in this font, and a mapping to a
       glyph that does not exist is the gap it was meant to remove. Every
       substitution here points at a shape that is really there. */
    { 0x2190, 0x3C }, { 0x2192, 0x3E }, { 0x2191, 0x5E }, { 0x2193, 0x76 },
    { 0x2194, 0x2D }
};

const boot_uint8_t* font_glyph(boot_uint32_t codepoint) {
    if (codepoint < 256) return font_8x16[codepoint];

    for (unsigned index = 0;
         index < sizeof(cp437_names) / sizeof(cp437_names[0]); index++)
        if (cp437_names[index].codepoint == codepoint)
            return font_8x16[cp437_names[index].byte];

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
