#ifndef KERNEL_LAYOUT_H
#define KERNEL_LAYOUT_H

#include "../include/bootinfo.h"

/* Keyboard layouts.
 *
 * The system could show Russian, Ukrainian and Greek and could not type a word
 * of any of them, which is a strange thing to hand somebody: a screen in their
 * language and a keyboard that is not.
 *
 * A layout here maps one character to another rather than one physical key to
 * a character, which is not how a real layout is defined and is exactly right
 * for this one. ЙЦУКЕН is described by where the letters sit on a QWERTY
 * keyboard, so "the key that would have typed q types й" is the definition
 * restated, not an approximation of it. It also means both keyboards get it
 * for free: PS/2 scancodes and USB usages both arrive here already turned into
 * a character, and neither driver has to learn about alphabets.
 *
 * Two layouts at a time, not four. The machine has a language - it was asked
 * at setup - and what somebody wants is that language and English, switched
 * with one gesture. A cycle through four is a cycle somebody overshoots.
 */

#define LAYOUT_EN 0
#define LAYOUT_RU 1
#define LAYOUT_UK 2
#define LAYOUT_GR 3
#define LAYOUT_COUNT 4

/* The layout that Alt+Shift switches to and back from. Set from the language
   recorded in the settings; English on a machine that has never been asked. */
void layout_set_alternate(int layout);
int layout_alternate(void);

int layout_current(void);
void layout_select(int layout);
void layout_toggle(void);

/* The Alt+Shift gesture, given the modifier state a driver currently sees.
 *
 * Here rather than in the drivers because a machine can have more than one
 * keyboard and every one of them sees the same fingers: two USB keyboards on
 * one emulated machine each reported the combination, each toggled, and the
 * layout came back to where it started. The latch belongs to the gesture, not
 * to the device that noticed it. */
void layout_gesture(int shift_down, int alt_down);

/* The code point `character` produces in the current layout, or the character
   itself when the layout does not change it. Digits, punctuation and control
   codes pass through: a layout that moved the digits would be a layout nobody
   could type a path in. */
boot_uint32_t layout_map(int character);

const char* layout_name(int layout);

#endif
