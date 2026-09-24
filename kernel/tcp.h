#ifndef KERNEL_TCP_H
#define KERNEL_TCP_H

#include "../include/bootinfo.h"

/* A conversation, rather than a question and an answer.
 *
 * Everything this machine could do on a network before this was UDP: ask,
 * wait, believe what comes back, forget. That is enough for ARP, DHCP, DNS,
 * ping and TFTP, and it is why packages arrive at twenty-one kilobytes a
 * second - one block per round trip is bounded by the speed of light rather
 * than by the wire.
 *
 * TCP is the other thing: both ends remember where they are, what is lost is
 * sent again, and bytes arrive in the order they were written or not at all.
 * HTTP needs it, which means a browser needs it, which is why it is here.
 *
 * The client half only. This dials out and never listens - there is no
 * accept and none of the states a server passes through, which is half the
 * protocol and none of what a browser does.
 *
 * Every call blocks with a timeout and drives the network while it waits,
 * which is how everything else in this stack behaves. A program that must
 * not stop asks for a short timeout and comes back.
 */

/* Dial. Returns a handle, or -1 when nothing answered in time. The address is
   in host order, as everywhere inside this kernel. */
int tcp_connect(boot_uint32_t address, boot_uint16_t port,
                boot_uint32_t timeout_ms);

/* Send, and wait for it to be acknowledged. Returns how many bytes were
   taken, which is fewer than asked for only when the other end stopped
   answering. */
int tcp_send(int handle, const void* data, boot_uint32_t length,
             boot_uint32_t timeout_ms);

/* Take what has arrived. Returns 0 when the other end has finished and there
   is nothing left - which is how a reply that ends with a close is read to
   its end - and -1 when the connection failed or nothing came in time. */
int tcp_receive(int handle, void* buffer, boot_uint32_t size,
                boot_uint32_t timeout_ms);

/* Say goodbye properly, and let go of the connection either way. */
int tcp_close(int handle, boot_uint32_t timeout_ms);

/* Whether there is any point asking for more: the connection is talking, or
   it has ended with bytes still waiting to be taken. */
int tcp_is_open(int handle);

/* One line into the log, for finding out what a connection thinks it is
   doing. */
void tcp_report(int handle);

#endif
