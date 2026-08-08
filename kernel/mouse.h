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

/* Bit 0 left, bit 1 right, bit 2 middle. */
int mouse_buttons(void);

/* How many packets have moved it. Zero after a while is a device that was
   found and is not reporting, which is a different problem from no device. */
boot_uint32_t mouse_movements(void);

void mouse_place(int x, int y);

#endif
