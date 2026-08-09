#ifndef KERNEL_FONT_H
#define KERNEL_FONT_H

#include "../include/bootinfo.h"

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

/* One 8x16 glyph per code point, indexed by unsigned char. Each byte is a
   scanline, bit 7 leftmost. Entries with no glyph are blank.
   Coverage: ASCII 0x20-0x7E, plus the CP437 shades, box drawing and block
   characters (0xB0-0xDF) that a DOS-looking UI draws frames with. */
extern const boot_uint8_t font_8x16[256][FONT_HEIGHT];

/* Beyond 255, by code point.
 *
 * The table above is indexed by a byte, which is a code page and not a
 * character set: it can hold 256 shapes and no more, and which 256 is a
 * decision somebody made in 1981. Russian, Ukrainian and Greek do not fit
 * beside the box drawing a DOS-looking system needs, and choosing between them
 * is choosing which of our testers gets a screen of rubbish.
 *
 * So text is UTF-8 and glyphs are found by code point. Most letters cost
 * nothing: A and А and Α are three different characters and one shape, so the
 * table below maps a code point either to a glyph of its own or to one the
 * font already has.
 *
 * Returns a blank glyph rather than nothing for a code point we do not carry,
 * because a missing letter should leave a gap the width of a letter - text
 * that silently closes up is text somebody will read as correct. */
const boot_uint8_t* font_glyph(boot_uint32_t codepoint);

/* Glyphs past code page 437, kept beside the table above in font_glyphs.c. */
typedef struct {
    boot_uint32_t codepoint;
    boot_uint8_t rows[FONT_HEIGHT];
} CODEPOINT_GLYPH;

extern const CODEPOINT_GLYPH font_beyond[];
extern const int font_beyond_count;

/* One UTF-8 character from `text`, and how many bytes it took. A byte that
   cannot start a sequence is handed back as itself, so a file that is not
   UTF-8 still prints as something. */
boot_uint32_t font_decode(const char* text, int* length);

#endif
