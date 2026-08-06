#ifndef KERNEL_KEYBOARD_H
#define KERNEL_KEYBOARD_H

#include "../include/bootinfo.h"

/* Keys that have no ASCII value. Returned by keyboard_getchar() as values
   above 0xFF so a caller can switch on them alongside ordinary characters. */
#define KEY_UP 0x100
#define KEY_DOWN 0x101
#define KEY_LEFT 0x102
#define KEY_RIGHT 0x103
#define KEY_HOME 0x104
#define KEY_END 0x105
#define KEY_PAGE_UP 0x106
#define KEY_PAGE_DOWN 0x107
#define KEY_DELETE 0x108
#define KEY_INSERT 0x109
#define KEY_F1 0x110  /* F1..F12 are consecutive from here. */

/* What keyboard_init() found. The distinction matters: a chipset's 8042 exists
   in silicon whether or not the board wired it to a connector, and ACPI's
   IAPC_BOOT_ARCH flag reports the controller, not the socket. Plenty of modern
   desktop boards therefore declare an 8042 and have nowhere to plug a keyboard
   in - so "the controller answered" is not the same as "there is a keyboard". */
#define KEYBOARD_ABSENT 0        /* no 8042 at all */
#define KEYBOARD_READY 1         /* controller and a keyboard that answered */
#define KEYBOARD_NO_DEVICE 2     /* controller present, nothing attached */

int keyboard_init(void);

/* Push a key into the shared buffer. Used by the USB driver, so that a key
   reaches the shell the same way whichever kind of keyboard produced it. */
void keyboard_submit(int key);

/* Non-blocking: returns 0 when nothing is buffered. */
/* Is there any way at all to read a keystroke - PS/2 or USB?
 *
 * The shell has to ask before it starts, because `keyboard_read_line` returns
 * an empty line both when the user pressed Enter and when nothing can ever
 * arrive. Without this distinction a machine with no keyboard fills the screen
 * with prompts instead of saying what is wrong. */
int keyboard_available(void);

/* Specifically whether a PS/2 keyboard came up, as opposed to any keyboard.
   The interrupt routing has to know: a USB one has no IRQ to move. */
int keyboard_present_ps2(void);

int keyboard_poll(void);

/* Blocking: waits for a key. Shows the console caret while waiting. */
int keyboard_getchar(void);

/* Read a line into `buffer`, handling backspace. Returns the length. */
boot_uint64_t keyboard_read_line(char* buffer, boot_uint64_t capacity);

#endif
