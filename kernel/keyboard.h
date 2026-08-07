#ifndef KERNEL_KEYBOARD_H
#define KERNEL_KEYBOARD_H

#include "../include/bootinfo.h"
#include "../include/syscall.h"

/* Keys that have no ASCII value. Returned by keyboard_getchar() as values
   above 0xFF so a caller can switch on them alongside ordinary characters.
 *
 * The numbers themselves are in include/syscall.h, because programs read the
 * arrow keys too and a second copy of a constant is how two copies drift
 * apart. These are the kernel's names for them. */
#define KEY_UP KOI_KEY_UP
#define KEY_DOWN KOI_KEY_DOWN
#define KEY_LEFT KOI_KEY_LEFT
#define KEY_RIGHT KOI_KEY_RIGHT
#define KEY_HOME KOI_KEY_HOME
#define KEY_END KOI_KEY_END
#define KEY_PAGE_UP KOI_KEY_PAGE_UP
#define KEY_PAGE_DOWN KOI_KEY_PAGE_DOWN
#define KEY_DELETE KOI_KEY_DELETE
#define KEY_INSERT KOI_KEY_INSERT
#define KEY_F1 KOI_KEY_F1  /* F1..F12 are consecutive from here. */

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

/* Is a keystroke waiting right now, without taking it?
 *
 * The distinction from keyboard_available() is the whole point: that one says
 * whether a keyboard exists at all, this one says whether a key has been
 * pressed. Anything that has to keep running while it waits - a game, an
 * animation, a long operation that should be interruptible - needs to ask the
 * second question, and answering it with the first would report every machine
 * with a keyboard as permanently holding a key down.
 *
 * Collects from USB first, because that controller's interrupt is still not
 * routed and its keystrokes only arrive when someone goes and looks. */
int keyboard_pending(void);

/* Blocking: waits for a key. Shows the console caret while waiting. */
int keyboard_getchar(void);

/* Read a line into `buffer`, handling backspace. Returns the length. */
boot_uint64_t keyboard_read_line(char* buffer, boot_uint64_t capacity);

#endif
