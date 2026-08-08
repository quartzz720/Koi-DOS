#include "net.h"
#include "xhci.h"
#include "string.h"
#include "serial.h"
#include "timer.h"

/* ---- The wire ------------------------------------------------------------
 *
 * Every field here is big-endian, and the machine is not. The conversions live
 * in these four functions and nowhere else: a byte swap scattered through the
 * code is a bug that only shows up as an address nobody recognises.
 */

static boot_uint16_t get_be16(const boot_uint8_t* at) {
    return (boot_uint16_t)((at[0] << 8) | at[1]);
}

static boot_uint32_t get_be32(const boot_uint8_t* at) {
    return ((boot_uint32_t)at[0] << 24) | ((boot_uint32_t)at[1] << 16) |
           ((boot_uint32_t)at[2] << 8) | (boot_uint32_t)at[3];
}

static void put_be16(boot_uint8_t* at, boot_uint16_t value) {
    at[0] = (boot_uint8_t)(value >> 8);
    at[1] = (boot_uint8_t)value;
}

static void put_be32(boot_uint8_t* at, boot_uint32_t value) {
    at[0] = (boot_uint8_t)(value >> 24);
    at[1] = (boot_uint8_t)(value >> 16);
    at[2] = (boot_uint8_t)(value >> 8);
    at[3] = (boot_uint8_t)value;
}

#define ETHERNET_HEADER 14
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806

#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_UDP 17

#define FRAME_MAX 1514

static const boot_uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static boot_uint32_t our_address;
static boot_uint32_t our_netmask;
static boot_uint32_t our_gateway;
static boot_uint32_t our_dns;
static int configured;

/* One entry is enough for now: everything not on this wire goes to the
   gateway, and the gateway is one machine. A table comes when something needs
   to talk to a second neighbour. */
static boot_uint32_t neighbour_address;
static boot_uint8_t neighbour_mac[6];
static int neighbour_known;

/* What the poll loop is waiting for, if anything. Set before sending, cleared
   by the frame that answers. */
static boot_uint32_t ping_pending_id;
static boot_uint32_t ping_pending_sequence;
static int ping_answered;

static boot_uint32_t dns_pending_id;
static boot_uint32_t dns_result;
static int dns_answered;

static int dhcp_offer_seen;
static int dhcp_ack_seen;
static boot_uint32_t dhcp_transaction;
static boot_uint32_t dhcp_offered;
static boot_uint32_t dhcp_server;

static void log(const char* text) { serial_write(text); }
static void log_dec(boot_uint64_t value) { serial_write_dec(value); }

/* ---- Checksums -----------------------------------------------------------
 *
 * The same one's-complement sum for IP, ICMP and UDP: add every 16-bit word,
 * fold the carries back in, invert. The odd trailing byte is padded on the
 * right, which is the part people get wrong.
 */
static boot_uint16_t checksum_partial(const boot_uint8_t* data,
                                      boot_uint32_t length,
                                      boot_uint32_t start) {
    boot_uint32_t sum = start;
    boot_uint32_t index = 0;

    while (index + 1 < length) {
        sum += (boot_uint32_t)get_be16(data + index);
        index += 2;
    }
    if (index < length) sum += (boot_uint32_t)data[index] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (boot_uint16_t)sum;
}

static boot_uint16_t checksum(const boot_uint8_t* data, boot_uint32_t length) {
    return (boot_uint16_t)~checksum_partial(data, length, 0);
}

/* ---- Sending -------------------------------------------------------------- */

/* One frame buffer for building, one for what has just been received. Nothing
   here is re-entrant and nothing here is interrupted: this is a DOS, and the
   only thing running is whatever asked. */
static boot_uint8_t out_frame[FRAME_MAX];
static boot_uint8_t in_frame[FRAME_MAX];

static void begin_frame(const boot_uint8_t* destination, boot_uint16_t type) {
    memcpy(out_frame, destination, 6);
    memcpy(out_frame + 6, usb_net_address(), 6);
    put_be16(out_frame + 12, type);
}

/* Fill in an IPv4 header and return where the payload starts.
 *
 * No options, so the header is always twenty bytes, and no fragmentation: a
 * frame that does not fit is one this does not send. Both are true of
 * everything above here. */
static boot_uint8_t* begin_ip(boot_uint32_t destination, boot_uint8_t protocol,
                              boot_uint32_t payload_length) {
    static boot_uint16_t identification = 1;
    boot_uint8_t* ip = out_frame + ETHERNET_HEADER;

    memset(ip, 0, 20);
    ip[0] = 0x45;                       /* version 4, five 32-bit words */
    ip[1] = 0;                          /* no differentiated services */
    put_be16(ip + 2, (boot_uint16_t)(20 + payload_length));
    put_be16(ip + 4, identification++);
    put_be16(ip + 6, 0x4000);           /* do not fragment */
    ip[8] = 64;                         /* time to live */
    ip[9] = protocol;
    put_be32(ip + 12, our_address);
    put_be32(ip + 16, destination);
    put_be16(ip + 10, checksum(ip, 20));
    return ip + 20;
}

/* The pseudo-header UDP and TCP checksum over: the addresses, the protocol and
   the length, none of which are in the datagram itself. It exists so that a
   datagram delivered to the wrong host fails its checksum. */
static boot_uint32_t pseudo_header_sum(boot_uint32_t source,
                                       boot_uint32_t destination,
                                       boot_uint8_t protocol,
                                       boot_uint16_t length) {
    return (source >> 16) + (source & 0xFFFF) +
           (destination >> 16) + (destination & 0xFFFF) +
           protocol + length;
}

static int send_udp(const boot_uint8_t* destination_mac, boot_uint32_t destination,
                    boot_uint16_t source_port, boot_uint16_t destination_port,
                    const boot_uint8_t* payload, boot_uint32_t length) {
    boot_uint8_t* udp;
    boot_uint16_t sum;

    if (ETHERNET_HEADER + 20 + 8 + length > FRAME_MAX) return 0;
    begin_frame(destination_mac, ETHERTYPE_IPV4);
    udp = begin_ip(destination, IP_PROTOCOL_UDP, 8 + length);
    put_be16(udp + 0, source_port);
    put_be16(udp + 2, destination_port);
    put_be16(udp + 4, (boot_uint16_t)(8 + length));
    put_be16(udp + 6, 0);
    memcpy(udp + 8, payload, length);

    sum = (boot_uint16_t)~checksum_partial(udp, 8 + length,
              pseudo_header_sum(our_address, destination, IP_PROTOCOL_UDP,
                                (boot_uint16_t)(8 + length)));
    /* Zero means "not computed" on the wire, so a checksum that genuinely
       comes out zero is sent as all ones. The two are equal in one's
       complement; only the meaning differs. */
    put_be16(udp + 6, sum ? sum : 0xFFFF);
    return usb_net_send(out_frame, ETHERNET_HEADER + 20 + 8 + length);
}

/* ---- ARP ----------------------------------------------------------------- */

static int send_arp(boot_uint16_t operation, const boot_uint8_t* target_mac,
                    boot_uint32_t target_address) {
    boot_uint8_t* arp = out_frame + ETHERNET_HEADER;

    begin_frame(operation == 1 ? broadcast_mac : target_mac, ETHERTYPE_ARP);
    put_be16(arp + 0, 1);               /* Ethernet */
    put_be16(arp + 2, ETHERTYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    put_be16(arp + 6, operation);
    memcpy(arp + 8, usb_net_address(), 6);
    put_be32(arp + 14, our_address);
    memcpy(arp + 18, operation == 1 ? broadcast_mac : target_mac, 6);
    put_be32(arp + 24, target_address);
    return usb_net_send(out_frame, ETHERNET_HEADER + 28);
}

static void handle_arp(const boot_uint8_t* arp, boot_uint32_t length) {
    boot_uint16_t operation;
    boot_uint32_t sender;

    if (length < 28) return;
    if (get_be16(arp + 0) != 1 || get_be16(arp + 2) != ETHERTYPE_IPV4) return;
    if (arp[4] != 6 || arp[5] != 4) return;

    operation = get_be16(arp + 6);
    sender = get_be32(arp + 14);

    /* Anything that speaks learns us a neighbour, request or reply alike: a
       machine that has just asked for our address is one we are about to want
       the address of. */
    if (sender == neighbour_address) {
        memcpy(neighbour_mac, arp + 8, 6);
        neighbour_known = 1;
    }

    if (operation == 1 && get_be32(arp + 24) == our_address && our_address)
        send_arp(2, arp + 8, sender);
}

/* Find the hardware address for one IP, asking until somebody answers. */
static int resolve_neighbour(boot_uint32_t address, boot_uint8_t* out) {
    boot_uint64_t start;

    if (neighbour_known && neighbour_address == address) {
        memcpy(out, neighbour_mac, 6);
        return 1;
    }
    neighbour_address = address;
    neighbour_known = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!send_arp(1, broadcast_mac, address)) return 0;
        start = timer_ticks();
        while (!timer_expired(start, 500)) {
            net_poll();
            if (neighbour_known) {
                memcpy(out, neighbour_mac, 6);
                return 1;
            }
        }
    }
    return 0;
}

/* ---- ICMP ---------------------------------------------------------------- */

static void handle_icmp(const boot_uint8_t* ip, const boot_uint8_t* icmp,
                        boot_uint32_t length) {
    if (length < 8) return;

    if (icmp[0] == 8) {                 /* echo request: answer it */
        boot_uint8_t* reply;
        boot_uint8_t mac[6];

        memcpy(mac, in_frame + 6, 6);
        begin_frame(mac, ETHERTYPE_IPV4);
        reply = begin_ip(get_be32(ip + 12), IP_PROTOCOL_ICMP, length);
        memcpy(reply, icmp, length);
        reply[0] = 0;                   /* echo reply */
        put_be16(reply + 2, 0);
        put_be16(reply + 2, checksum(reply, length));
        usb_net_send(out_frame, ETHERNET_HEADER + 20 + length);
        return;
    }

    if (icmp[0] == 0 &&                 /* echo reply: is it ours? */
        get_be16(icmp + 4) == ping_pending_id &&
        get_be16(icmp + 6) == ping_pending_sequence)
        ping_answered = 1;
}

int net_ping(boot_uint32_t address, boot_uint32_t timeout_ms) {
    static boot_uint16_t sequence = 0;
    boot_uint8_t mac[6];
    boot_uint8_t* icmp;
    boot_uint64_t start;
    boot_uint32_t target;

    if (!configured) return -1;

    /* Off this wire, it goes to the gateway - the address in the packet stays
       the far machine's, only the hardware address changes. That difference is
       the whole of routing at this level. */
    target = ((address ^ our_address) & our_netmask) ? our_gateway : address;
    if (!resolve_neighbour(target, mac)) return -1;

    ping_pending_id = 0x4B4F;           /* "KO" */
    ping_pending_sequence = ++sequence;
    ping_answered = 0;

    begin_frame(mac, ETHERTYPE_IPV4);
    icmp = begin_ip(address, IP_PROTOCOL_ICMP, 8 + 32);
    memset(icmp, 0, 8 + 32);
    icmp[0] = 8;                        /* echo request */
    put_be16(icmp + 4, (boot_uint16_t)ping_pending_id);
    put_be16(icmp + 6, (boot_uint16_t)ping_pending_sequence);
    for (int index = 0; index < 32; index++)
        icmp[8 + index] = (boot_uint8_t)('a' + (index % 26));
    put_be16(icmp + 2, checksum(icmp, 8 + 32));

    start = timer_ticks();
    if (!usb_net_send(out_frame, ETHERNET_HEADER + 20 + 8 + 32)) return -1;

    while (!timer_expired(start, timeout_ms)) {
        net_poll();
        if (ping_answered) {
            boot_uint64_t elapsed = timer_ticks() - start;
            return (int)elapsed;
        }
    }
    return -1;
}

/* ---- DHCP ---------------------------------------------------------------- */

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC 0x63825363U

/* The options this cares about. There are a hundred more and they are all
   somebody else's problem. */
#define DHCP_OPTION_NETMASK 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_DNS 6
#define DHCP_OPTION_REQUESTED 50
#define DHCP_OPTION_TYPE 53
#define DHCP_OPTION_SERVER 54
#define DHCP_OPTION_PARAMETERS 55
#define DHCP_OPTION_END 255

#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5

static boot_uint8_t dhcp_buffer[548];

/* Build the fixed part of a DHCP message and return where the options go.
 *
 * The 236-byte header is BOOTP's, unchanged since 1985, and most of it has
 * been dead for as long: the file and server-name fields are 192 bytes of
 * zeroes that every DHCP message on earth still carries. */
static boot_uint32_t begin_dhcp(boot_uint8_t type) {
    boot_uint32_t at;

    memset(dhcp_buffer, 0, sizeof(dhcp_buffer));
    dhcp_buffer[0] = 1;                 /* a request, from a client */
    dhcp_buffer[1] = 1;                 /* over Ethernet */
    dhcp_buffer[2] = 6;                 /* six bytes of hardware address */
    put_be32(dhcp_buffer + 4, dhcp_transaction);
    put_be16(dhcp_buffer + 10, 0x8000); /* answer by broadcast: we have no
                                           address yet to be answered at */
    memcpy(dhcp_buffer + 28, usb_net_address(), 6);
    put_be32(dhcp_buffer + 236, DHCP_MAGIC);

    at = 240;
    dhcp_buffer[at++] = DHCP_OPTION_TYPE;
    dhcp_buffer[at++] = 1;
    dhcp_buffer[at++] = type;
    return at;
}

static boot_uint32_t end_dhcp(boot_uint32_t at) {
    dhcp_buffer[at++] = DHCP_OPTION_PARAMETERS;
    dhcp_buffer[at++] = 3;
    dhcp_buffer[at++] = DHCP_OPTION_NETMASK;
    dhcp_buffer[at++] = DHCP_OPTION_ROUTER;
    dhcp_buffer[at++] = DHCP_OPTION_DNS;
    dhcp_buffer[at++] = DHCP_OPTION_END;
    /* Padded to the minimum a BOOTP relay will forward. Nothing here goes
       through a relay, but a server that drops short messages is a real thing
       and the padding costs nothing. */
    while (at < 300) dhcp_buffer[at++] = 0;
    return at;
}

static void handle_dhcp(const boot_uint8_t* message, boot_uint32_t length) {
    boot_uint32_t at = 240;
    boot_uint8_t type = 0;
    boot_uint32_t netmask = 0;
    boot_uint32_t router = 0;
    boot_uint32_t dns = 0;
    boot_uint32_t server = 0;

    if (length < 241) return;
    if (dhcp_buffer[0] && get_be32(message + 4) != dhcp_transaction) return;
    if (message[0] != 2) return;        /* a reply, from a server */
    if (get_be32(message + 236) != DHCP_MAGIC) return;

    while (at < length) {
        boot_uint8_t option = message[at];
        boot_uint8_t size;

        if (option == DHCP_OPTION_END) break;
        if (option == 0) { at++; continue; }   /* padding */
        if (at + 2 > length) break;
        size = message[at + 1];
        if (at + 2 + size > length) break;

        switch (option) {
        case DHCP_OPTION_TYPE: if (size >= 1) type = message[at + 2]; break;
        case DHCP_OPTION_NETMASK: if (size >= 4) netmask = get_be32(message + at + 2); break;
        case DHCP_OPTION_ROUTER: if (size >= 4) router = get_be32(message + at + 2); break;
        case DHCP_OPTION_DNS: if (size >= 4) dns = get_be32(message + at + 2); break;
        case DHCP_OPTION_SERVER: if (size >= 4) server = get_be32(message + at + 2); break;
        default: break;
        }
        at += 2 + size;
    }

    if (type == DHCP_OFFER) {
        dhcp_offered = get_be32(message + 16);   /* "your address" */
        dhcp_server = server;
        dhcp_offer_seen = 1;
    } else if (type == DHCP_ACK) {
        our_address = get_be32(message + 16);
        /* A server that offers no netmask is offering a /24, because it is
           handing out an address on a wire it is also on. */
        our_netmask = netmask ? netmask : 0xFFFFFF00U;
        our_gateway = router;
        our_dns = dns;
        dhcp_ack_seen = 1;
    }
}

int net_start(void) {
    boot_uint64_t start;
    boot_uint32_t at;
    boot_uint8_t mac[6];

    if (!usb_net_ready()) return 0;
    configured = 0;
    our_address = 0;
    neighbour_known = 0;

    /* The transaction id ties an offer to the request that provoked it. The
       clock is not a good source of randomness and does not have to be: what
       is needed is a number this boot has not used before. */
    dhcp_transaction = (boot_uint32_t)(timer_ticks() * 2654435761U) | 1;

    for (int attempt = 0; attempt < 4; attempt++) {
        dhcp_offer_seen = 0;
        at = begin_dhcp(DHCP_DISCOVER);
        at = end_dhcp(at);
        if (!send_udp(broadcast_mac, 0xFFFFFFFFU, DHCP_CLIENT_PORT,
                      DHCP_SERVER_PORT, dhcp_buffer, at))
            return 0;

        start = timer_ticks();
        while (!timer_expired(start, 1000)) {
            net_poll();
            if (dhcp_offer_seen) break;
        }
        if (dhcp_offer_seen) break;
    }
    if (!dhcp_offer_seen) {
        log("NET: nobody offered an address\n");
        return 0;
    }

    for (int attempt = 0; attempt < 4; attempt++) {
        dhcp_ack_seen = 0;
        at = begin_dhcp(DHCP_REQUEST);
        dhcp_buffer[at++] = DHCP_OPTION_REQUESTED;
        dhcp_buffer[at++] = 4;
        put_be32(dhcp_buffer + at, dhcp_offered);
        at += 4;
        if (dhcp_server) {
            dhcp_buffer[at++] = DHCP_OPTION_SERVER;
            dhcp_buffer[at++] = 4;
            put_be32(dhcp_buffer + at, dhcp_server);
            at += 4;
        }
        at = end_dhcp(at);
        if (!send_udp(broadcast_mac, 0xFFFFFFFFU, DHCP_CLIENT_PORT,
                      DHCP_SERVER_PORT, dhcp_buffer, at))
            return 0;

        start = timer_ticks();
        while (!timer_expired(start, 1000)) {
            net_poll();
            if (dhcp_ack_seen) break;
        }
        if (dhcp_ack_seen) break;
    }
    if (!dhcp_ack_seen) {
        log("NET: the offer was never confirmed\n");
        return 0;
    }

    configured = 1;
    log("NET: address ");
    for (int index = 0; index < 4; index++) {
        if (index) log(".");
        log_dec((our_address >> (24 - index * 8)) & 0xFF);
    }
    log("\n");

    /* Learn the gateway now rather than on the first packet: it is the one
       thing that tells a working configuration from an address on a wire with
       nothing else on it. */
    if (our_gateway) (void)resolve_neighbour(our_gateway, mac);
    return 1;
}

/* ---- DNS ----------------------------------------------------------------- */

static boot_uint8_t dns_buffer[512];

/* Names go on the wire as a run of counted labels: "www.google.com" becomes
   3 w w w 6 g o o g l e 3 c o m 0. Returns the length written, or 0. */
static boot_uint32_t encode_name(const char* name, boot_uint8_t* out,
                                 boot_uint32_t size) {
    boot_uint32_t at = 0;
    boot_uint32_t label = 0;

    while (*name) {
        boot_uint32_t length = 0;

        label = at++;
        if (at >= size) return 0;
        while (name[length] && name[length] != '.') length++;
        if (!length || length > 63 || at + length + 1 >= size) return 0;
        memcpy(out + at, name, length);
        out[label] = (boot_uint8_t)length;
        at += length;
        name += length;
        if (*name == '.') name++;
    }
    if (!at || at + 1 > size) return 0;
    out[at++] = 0;
    return at;
}

/* Step over a name in a reply, which may end in a pointer back into the
   message rather than a zero byte. We only need its length, not its text. */
static boot_uint32_t skip_name(const boot_uint8_t* message, boot_uint32_t at,
                               boot_uint32_t length) {
    while (at < length) {
        boot_uint8_t size = message[at];
        if ((size & 0xC0) == 0xC0) return at + 2;   /* a pointer, and the end */
        if (!size) return at + 1;
        at += 1 + size;
    }
    return length;
}

static void handle_dns(const boot_uint8_t* message, boot_uint32_t length) {
    boot_uint32_t questions;
    boot_uint32_t answers;
    boot_uint32_t at = 12;

    if (length < 12) return;
    if (get_be16(message + 0) != dns_pending_id) return;
    if (!(message[2] & 0x80)) return;   /* not a reply */
    if (message[3] & 0x0F) return;      /* the server said no */

    questions = get_be16(message + 4);
    answers = get_be16(message + 6);

    for (boot_uint32_t index = 0; index < questions; index++) {
        at = skip_name(message, at, length);
        at += 4;                        /* type and class */
    }
    for (boot_uint32_t index = 0; index < answers && at + 10 <= length; index++) {
        boot_uint16_t type;
        boot_uint16_t size;

        at = skip_name(message, at, length);
        if (at + 10 > length) return;
        type = get_be16(message + at);
        size = get_be16(message + at + 8);
        at += 10;
        if (at + size > length) return;
        /* Type 1 is an address; everything else in the answer section is a
           name pointing at another name, which this does not follow. */
        if (type == 1 && size == 4) {
            dns_result = get_be32(message + at);
            dns_answered = 1;
            return;
        }
        at += size;
    }
}

int net_resolve(const char* name, boot_uint32_t* out) {
    boot_uint32_t at;
    boot_uint8_t mac[6];
    boot_uint64_t start;
    boot_uint32_t target;

    if (net_parse_address(name, out)) return 1;
    if (!configured || !our_dns) return 0;

    target = ((our_dns ^ our_address) & our_netmask) ? our_gateway : our_dns;
    if (!resolve_neighbour(target, mac)) return 0;

    dns_pending_id = (boot_uint32_t)(timer_ticks() & 0xFFFF) | 1;
    dns_answered = 0;

    memset(dns_buffer, 0, sizeof(dns_buffer));
    put_be16(dns_buffer + 0, (boot_uint16_t)dns_pending_id);
    put_be16(dns_buffer + 2, 0x0100);   /* a query, recursion wanted */
    put_be16(dns_buffer + 4, 1);        /* one question */
    at = encode_name(name, dns_buffer + 12, sizeof(dns_buffer) - 16);
    if (!at) return 0;
    at += 12;
    put_be16(dns_buffer + at, 1);       /* an address */
    put_be16(dns_buffer + at + 2, 1);   /* on the internet */
    at += 4;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (!send_udp(mac, our_dns, 40000, 53, dns_buffer, at)) return 0;
        start = timer_ticks();
        while (!timer_expired(start, 2000)) {
            net_poll();
            if (dns_answered) {
                *out = dns_result;
                return 1;
            }
        }
    }
    return 0;
}

/* ---- Receiving ----------------------------------------------------------- */

static void handle_udp(const boot_uint8_t* udp, boot_uint32_t length) {
    boot_uint16_t destination;

    if (length < 8) return;
    destination = get_be16(udp + 2);
    if (destination == DHCP_CLIENT_PORT) handle_dhcp(udp + 8, length - 8);
    else if (get_be16(udp + 0) == 53) handle_dns(udp + 8, length - 8);
}

static void handle_ip(const boot_uint8_t* ip, boot_uint32_t length) {
    boot_uint32_t header;
    boot_uint32_t total;
    boot_uint32_t destination;

    if (length < 20) return;
    if ((ip[0] >> 4) != 4) return;
    header = (boot_uint32_t)(ip[0] & 0x0F) * 4;
    total = get_be16(ip + 2);
    if (header < 20 || total < header || total > length) return;

    /* Ours, or a broadcast. Before an address is configured everything is
       ours, which is how the DHCP reply gets in - it is addressed to a machine
       that does not have that address yet. */
    destination = get_be32(ip + 16);
    if (our_address && destination != our_address &&
        destination != 0xFFFFFFFFU &&
        (destination | our_netmask) != 0xFFFFFFFFU)
        return;

    switch (ip[9]) {
    case IP_PROTOCOL_ICMP: handle_icmp(ip, ip + header, total - header); break;
    case IP_PROTOCOL_UDP: handle_udp(ip + header, total - header); break;
    default: break;
    }
}

void net_poll(void) {
    boot_uint32_t length;

    /* The controller collects frames into its own queue as its events arrive,
       and that only happens when somebody drains the event ring - so this has
       to do both halves. */
    xhci_poll();

    while ((length = usb_net_receive(in_frame, sizeof(in_frame))) != 0) {
        boot_uint16_t type;

        if (length < ETHERNET_HEADER) continue;
        /* Addressed to us or to everyone. The device filters too, but it was
           asked to pass multicast and this is cheaper than parsing what it
           passed. */
        if (in_frame[0] != 0xFF && memcmp(in_frame, usb_net_address(), 6) != 0)
            continue;

        type = get_be16(in_frame + 12);
        if (type == ETHERTYPE_ARP)
            handle_arp(in_frame + ETHERNET_HEADER, length - ETHERNET_HEADER);
        else if (type == ETHERTYPE_IPV4)
            handle_ip(in_frame + ETHERNET_HEADER, length - ETHERNET_HEADER);
    }
}

/* ---- What we are --------------------------------------------------------- */

int net_configured(void) { return configured; }
boot_uint32_t net_address(void) { return our_address; }
boot_uint32_t net_netmask(void) { return our_netmask; }
boot_uint32_t net_gateway(void) { return our_gateway; }
boot_uint32_t net_dns(void) { return our_dns; }

const boot_uint8_t* net_hardware_address(void) { return usb_net_address(); }

int net_parse_address(const char* text, boot_uint32_t* out) {
    boot_uint32_t value = 0;

    for (int part = 0; part < 4; part++) {
        boot_uint32_t octet = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            octet = octet * 10 + (boot_uint32_t)(*text++ - '0');
            if (++digits > 3 || octet > 255) return 0;
        }
        if (!digits) return 0;
        value = (value << 8) | octet;
        if (part < 3) {
            if (*text != '.') return 0;
            text++;
        }
    }
    if (*text) return 0;
    *out = value;
    return 1;
}

void net_format_address(boot_uint32_t address, char* out) {
    int at = 0;

    for (int part = 0; part < 4; part++) {
        boot_uint32_t octet = (address >> (24 - part * 8)) & 0xFF;

        if (part) out[at++] = '.';
        if (octet >= 100) out[at++] = (char)('0' + octet / 100);
        if (octet >= 10) out[at++] = (char)('0' + (octet / 10) % 10);
        out[at++] = (char)('0' + octet % 10);
    }
    out[at] = 0;
}
