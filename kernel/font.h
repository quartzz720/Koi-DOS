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

#endif
