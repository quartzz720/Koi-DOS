#ifndef KERNEL_NET_INTERNAL_H
#define KERNEL_NET_INTERNAL_H

#include "../include/bootinfo.h"

/* What the layers above IP need from the layer below it.
 *
 * net.c has ARP, IP, UDP, ICMP and everything that makes them work; tcp.c is
 * a different protocol on top of the same IP. Rather than copy a frame
 * builder and a checksum into the second file - two copies of arithmetic that
 * is easy to get subtly wrong and impossible to test separately - the few
 * things both need are declared here.
 *
 * This is not a public interface. Programs see net.h; this is one file in the
 * kernel talking to another, and nothing outside those two should include it.
 */

#define NET_ETHERNET_HEADER 14
#define NET_FRAME_MAX 1514
#define NET_IP_PROTOCOL_TCP 6

/* Byte order at the edges. The wire is big-endian and the machine is not. */
boot_uint16_t net_get_be16(const boot_uint8_t* at);
boot_uint32_t net_get_be32(const boot_uint8_t* at);
void net_put_be16(boot_uint8_t* at, boot_uint16_t value);
void net_put_be32(boot_uint8_t* at, boot_uint32_t value);

/* The one's complement sum everything from IP upwards is checked with.
   `partial` carries a running total in, for a checksum computed over more
   than one region - which is what a pseudo-header is. */
boot_uint16_t net_checksum_partial(const boot_uint8_t* data,
                                   boot_uint32_t length,
                                   boot_uint32_t partial);

/* The addresses, protocol and length that are not in the segment itself, and
   which the checksum covers so that a segment delivered to the wrong host
   fails rather than being believed. */
boot_uint32_t net_pseudo_header_sum(boot_uint32_t source,
                                    boot_uint32_t destination,
                                    boot_uint8_t protocol,
                                    boot_uint16_t length);

/* Build a frame and an IPv4 header into the shared outgoing buffer, and
   return where the payload goes. One buffer, because one frame is being built
   at a time and nothing here is reentrant. */
boot_uint8_t* net_begin_ip_frame(const boot_uint8_t* destination_mac,
                                 boot_uint32_t destination,
                                 boot_uint8_t protocol,
                                 boot_uint32_t payload_length);

/* Hand the built frame to whichever wire is carrying frames. */
int net_send_frame(boot_uint32_t length);

/* The hardware address to send to for an IP address: the machine itself when
   it is on our network, the gateway when it is not. Asks ARP and waits, so it
   is slow the first time and free afterwards. */
int net_hardware_for(boot_uint32_t address, boot_uint8_t* mac);

/* Called by net.c when a TCP segment arrives, with the IP header before it so
   that the addresses are still at hand. */
void tcp_receive_segment(boot_uint32_t source, boot_uint32_t destination,
                         const boot_uint8_t* segment, boot_uint32_t length);

#endif
