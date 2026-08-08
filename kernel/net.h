#ifndef KERNEL_NET_H
#define KERNEL_NET_H

#include "../include/bootinfo.h"

/* TCP/IP, as far as a DOS-like system needs it.
 *
 * ARP, IPv4, UDP, DHCP, DNS and ICMP echo - which is exactly the set that
 * makes `ping google.com` work and nothing more. There is no TCP here and no
 * sockets; those are a layer somebody writes when a program needs them.
 *
 * Underneath is whatever xhci.c has attached, which today means a phone
 * sharing its connection over USB. Nothing in here knows that.
 *
 * Addresses are held in host order and converted at the edges. The wire is
 * big-endian and the machine is not, and picking one convention for the inside
 * is the only way to keep the byte swaps in places where they can be seen. */

/* Take the interface up: find out who we are by asking a DHCP server, then
   find the gateway's hardware address. Returns 1 when we have an address.
   Takes a few seconds when it fails, which is the honest cost of asking a
   question nobody answers. */
int net_start(void);

/* Is there anything to send frames over - a card or a phone - and which. */
int net_link_ready(void);
const char* net_link_name(void);

/* Frames out, frames in, and frames lost, whichever wire is carrying them. */
void net_traffic(boot_uint32_t* sent, boot_uint32_t* received,
                 boot_uint32_t* lost);

/* Has that happened, and what did it get us? Addresses in host order. */
int net_configured(void);
boot_uint32_t net_address(void);
boot_uint32_t net_netmask(void);
boot_uint32_t net_gateway(void);
boot_uint32_t net_dns(void);
const boot_uint8_t* net_hardware_address(void);

/* Collect whatever has arrived and answer what can be answered without asking
   anybody: ARP requests for our address, and pings. Called from the same loop
   that waits for a keystroke, so a machine sitting at the prompt is still a
   machine that replies. */
void net_poll(void);

/* Take the interface up with an address chosen by hand. Netmask zero means a
   /24; gateway and name server may be zero, which is what a cable between two
   machines looks like. */
int net_configure(boot_uint32_t address, boot_uint32_t netmask,
                  boot_uint32_t gateway, boot_uint32_t dns);

/* One datagram to one place. At most 1472 bytes - what fits in an Ethernet
   frame once the IP and UDP headers are in it - because nothing here
   fragments. Returns 1 when it went. */
int net_send_to(boot_uint32_t address, boot_uint16_t port,
                const void* data, boot_uint32_t length);

/* The same, from a chosen source port - which for some protocols is not a
   detail: it is the address the other end replies to and identifies the
   conversation. */
int net_send_from(boot_uint16_t source_port, boot_uint32_t address,
                  boot_uint16_t port, const void* data, boot_uint32_t length);

/* Collect datagrams addressed to one port. `net_listen(0)` stops.
 *
 * One datagram is held at a time. Everything that uses this sends a request
 * and waits for its reply before sending the next, so a queue would be storage
 * for a situation that does not arise. */
void net_listen(boot_uint16_t port);

/* Wait for one, and say where it came from. Returns its length, or -1. */
int net_receive_from(void* data, boot_uint32_t size, boot_uint32_t timeout_ms,
                     boot_uint32_t* address, boot_uint16_t* port);

/* Look up a name. Returns 1 and fills `out` in host order, or 0. */
int net_resolve(const char* name, boot_uint32_t* out);

/* One echo request, waited for. Returns the round trip in milliseconds, or -1
   if nothing came back before `timeout_ms`. */
int net_ping(boot_uint32_t address, boot_uint32_t timeout_ms);

/* Parse dotted quad into host order. Returns 1 when the whole string was one
   and nothing else. */
int net_parse_address(const char* text, boot_uint32_t* out);

/* Write one as dotted quad into `out`, which needs 16 bytes. */
void net_format_address(boot_uint32_t address, char* out);

#endif
