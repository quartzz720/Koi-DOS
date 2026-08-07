#ifndef KERNEL_XHCI_H
#define KERNEL_XHCI_H

#include "pci.h"

/* The USB 3 host controller.
 *
 * This is the largest driver in the project, and the one that unlocks real
 * hardware: a modern desktop board often has no PS/2 socket wired to it, and a
 * USB stick is invisible to the block layer without it. Keyboards and mass
 * storage are both class drivers over the same core.
 *
 * xHCI fails quietly. A controller that has been set up incorrectly does not
 * complain - it simply never posts an event, and every later step waits for
 * something that will not come. The driver is therefore built and verified in
 * slices, each with something observable at the end of it, and reports what it
 * found rather than only whether it succeeded. */

/* Bring up one xHCI controller. Returns 1 when it is running and its rings are
   live. Call it once per controller found on the bus: a board routinely has
   two - one in the chipset, one in the processor - and which one a device
   lands on is a matter of which socket it was plugged into. */
int xhci_init(const PCI_DEVICE* controller);

/* How many controllers were taken up. */
boot_uint32_t xhci_controller_count(void);

/* Root-hub ports across every running controller, and how many currently have
   something plugged into them. Zero for both when nothing came up. */
boot_uint32_t xhci_port_count(void);
boot_uint32_t xhci_ports_connected(void);

/* True when a USB keyboard was found and configured. */
int xhci_has_keyboard(void);

/* True when a USB mass storage device was found and registered with the block
   layer. From that point on the filesystem reaches it like any other disk. */
int xhci_has_storage(void);

/* Why the last USB storage transfer failed, in the device's own words, or NULL
   when the last one succeeded. "Copy failed" is not a diagnosis; a stick that
   has gone read-only and a stick that is dying deserve different sentences. */
const char* xhci_storage_error(void);

/* Collect anything the keyboard has sent and hand it to the keyboard driver.
   Called from the input wait loop: the controller's interrupt is not wired to
   anything yet, so keystrokes only arrive when someone asks for them. */
void xhci_poll(void);

/* One pass over every root-hub port, acting on anything that has been plugged
   in or pulled out since the last look.
 *
 * Enumeration used to happen once, at boot, which is the whole of what a
 * driver needs when the device is soldered on and the whole of what it must
 * not be when the device is a plug. Called from xhci_poll, so anything that
 * waits for a keystroke also notices a stick appearing. */
void xhci_service(void);

#endif
