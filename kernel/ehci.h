#ifndef KERNEL_EHCI_H
#define KERNEL_EHCI_H

#include "pci.h"

/* USB 2.0 host controllers.
 *
 * The reason this exists is a laptop: two EHCI controllers in the chipset with
 * nothing driving them, and every socket on the left-hand side of the machine
 * therefore dead. On a desktop made in the last decade the USB 2 ports are
 * wired to the xHCI controller and this file has nothing to do; on anything
 * older it is the difference between a keyboard and no keyboard. */
int ehci_init(const PCI_DEVICE* controller);

/* How many were taken up. */
boot_uint32_t ehci_controller_count(void);

/* Root ports across all of them, and how many have something in them. */
boot_uint32_t ehci_port_count(void);
boot_uint32_t ehci_ports_connected(void);

/* Ports handed to a companion controller because the device on them is too
   slow for EHCI to talk to. Reported because those are devices that are
   physically present and that this system cannot see - which is worth saying
   out loud rather than leaving as an unexplained absence. */
boot_uint32_t ehci_ports_released(void);

#endif
