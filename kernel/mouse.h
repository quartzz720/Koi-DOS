#ifndef KERNEL_MOUSE_H
#define KERNEL_MOUSE_H

#include "../include/bootinfo.h"

/* A pointer, from the PS/2 auxiliary port.
 *
 * On a laptop that is the touchpad. Touchpads come up speaking the ordinary
 * three-byte mouse protocol and keep their own language for gestures, so
 * pointing at things needs nothing vendor-specific - which is the difference
 * between a driver and a project.
 *
 * `width` and `height` bound the pointer; give it the screen's. */
int mouse_init(boot_uint32_t width, boot_uint32_t height);

int mouse_present(void);
int mouse_x(void);
int mouse_y(void);

/* Bit 0 left, bit 1 right, bit 2 middle, and bits 4 and 5 the two side buttons
   on a device that has them. */
int mouse_buttons(void);

/* Is there a wheel? On a touchpad this is also the answer to whether two-finger
   scrolling works, because the pad reports the gesture as wheel movement. */
int mouse_has_wheel(void);

/* Notches turned since the machine started, positive upwards. A running total,
   not a since-you-last-asked figure: a caller remembers what it last saw and
   subtracts, which lets any number of callers watch the same wheel. */
int mouse_scroll(void);

/* How many times a button has gone down, ever: 0 left, 1 right, 2 middle.
 *
 * Asking whether a button is down right now loses clicks. A click lasts a
 * tenth of a second at most, and anything that looks thirty times a second
 * will eventually look between the press and the release and see nothing -
 * which presents as a button that sometimes does not work. A count cannot be
 * missed: the reader compares it with what it last saw. */
boot_uint32_t mouse_presses(int button);

/* How many packets have moved it. Zero after a while is a device that was
   found and is not reporting, which is a different problem from no device. */
boot_uint32_t mouse_movements(void);

void mouse_place(int x, int y);

#endif
