#ifndef KERNEL_E1000_H
#define KERNEL_E1000_H

#include "pci.h"

/* Intel gigabit Ethernet - the 8254x/8257x family, which is what "e1000"
 * means to everyone who has met it.
 *
 * Written because a phone is a bad thing to debug against. It decides on its
 * own whether tethering is on, changes its USB identity while you are talking
 * to it, and gives no reason for anything. A network card sits still: registers
 * where the manual says, a ring of descriptors, and a link light. And the card
 * in the laptop this was written for is an 82579LM, which is this family.
 *
 * No interrupts. Completion is a bit in a descriptor, and everything else in
 * this system is polled, so the card is too.
 */
int e1000_init(const PCI_DEVICE* device);

/* Is there a card, configured, with a cable in it? */
int e1000_ready(void);

/* True when the card is up but nothing is plugged into it. Worth its own
   question: it is the one failure a user can fix in a second. */
int e1000_link_down(void);

const boot_uint8_t* e1000_address(void);

/* Send one Ethernet frame. Returns 1 when the card took it. */
int e1000_send(const void* frame, boot_uint32_t length);

/* Take the oldest frame that has arrived, or 0. Never waits. */
boot_uint32_t e1000_receive(void* frame, boot_uint32_t size);

/* The card's own view of its rings, written through the caller's printer so
   it can go to a screen or to the log. What the driver believes and what the
   hardware is doing are different things, and only one of them is evidence. */
void e1000_diagnose(void (*out)(const char*), void (*number)(boot_uint64_t));

/* Frames out, frames in, and frames dropped for want of anywhere to put them. */
void e1000_counters(boot_uint32_t* sent, boot_uint32_t* received,
                    boot_uint32_t* dropped);

#endif
