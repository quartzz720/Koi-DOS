#include "tcp.h"
#include "task.h"
#include "net.h"
#include "net_internal.h"
#include "timer.h"
#include "string.h"
#include "serial.h"

/* One task at a time in here.
 *
 * A wait below hands the processor over so the machine keeps moving, which
 * means another task can arrive at one of these calls while this one is still
 * inside it - and there is one connection table, one send buffer and one card.
 * The gate makes that impossible without making the wait a stall: whoever
 * arrives second gives up turns until the first is out. */
static KERNEL_GATE network_gate;

static int tcp_connect_locked(boot_uint32_t address, boot_uint16_t port,
                              boot_uint32_t timeout_ms);
static int tcp_send_locked(int handle, const void* data, boot_uint32_t length,
                           boot_uint32_t timeout_ms);
static int tcp_receive_locked(int handle, void* buffer, boot_uint32_t size,
                              boot_uint32_t timeout_ms);
static int tcp_close_locked(int handle, boot_uint32_t timeout_ms);

/* TCP, the client half.
 *
 * Everything this machine could do on a network until now was one question
 * and one answer: ARP, DHCP, DNS, ping, TFTP. Ask, wait, believe what comes
 * back, forget. That is UDP, and it is why the packages arrive at twenty-one
 * kilobytes a second - a protocol that sends one block per round trip is
 * bounded by the speed of light and not by the wire.
 *
 * A conversation is different. Both sides remember where they are, anything
 * lost is sent again, and the bytes arrive in the order they were written or
 * they do not arrive at all. That is TCP, and it is what HTTP needs, which is
 * what a browser needs.
 *
 * ---- What this is, and what it is not ------------------------------------
 *
 * The half a client uses. It dials out, it never listens - so there is no
 * accept, no backlog, and none of the states a server passes through. That
 * removes about half of the protocol and none of what a browser does.
 *
 * One segment in flight at a time. A real implementation keeps a window full
 * and slides it; this sends, waits for the acknowledgement, and sends the
 * next - which costs a round trip per segment when sending. It is written
 * this way on purpose: what a client sends is a request of a few hundred
 * bytes, and what it receives is the page. The receiving side takes a whole
 * window at a time, which is where the speed actually matters.
 *
 * Data that arrives out of order is dropped rather than held. The
 * acknowledgement then repeats what we are still waiting for, the other end
 * sends it again, and the transfer continues - slower, when a packet is lost,
 * and correct. Holding it is a reassembly queue, and a reassembly queue is
 * where a first TCP goes wrong.
 *
 * No window scaling, no selective acknowledgement, no timestamps. Every one
 * of them is negotiated by an option nobody has to offer.
 */

#define TCP_CONNECTIONS 4
#define TCP_RECEIVE_RING 16384
#define TCP_SEND_MAX 1460          /* one segment, no fragmentation */

/* The flags, in the byte they live in. */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* Where a connection is.
 *
 * Fewer states than the specification names, because the ones missing belong
 * to a listener. What is here: dialling, talking, and the two ways a
 * conversation ends - we hang up, or they do. */
#define STATE_FREE 0
#define STATE_SYN_SENT 1
#define STATE_ESTABLISHED 2
#define STATE_FIN_SENT 3           /* we said we are done, waiting for theirs */
#define STATE_THEIRS_CLOSED 4      /* they are done; we may still send */
#define STATE_CLOSED 5             /* over, and the bytes already here remain */

typedef struct {
    int state;
    boot_uint32_t remote_address;
    boot_uint16_t remote_port;
    boot_uint16_t local_port;
    boot_uint8_t remote_mac[6];

    /* Sequence numbers, in host order. `send_next` is what the next byte we
       send will be numbered; `send_unacknowledged` is the oldest byte they
       have not confirmed. They differ only while something is in flight. */
    boot_uint32_t send_next;
    boot_uint32_t send_unacknowledged;
    boot_uint32_t receive_next;    /* the next byte we expect from them */

    /* The segment in flight, kept so it can be sent again. */
    boot_uint8_t pending[TCP_SEND_MAX];
    boot_uint32_t pending_length;
    boot_uint32_t pending_sequence;
    boot_uint64_t pending_sent_at;
    int pending_tries;

    /* What has arrived and nobody has taken yet. A ring, because a page
       arrives faster than a program reads it and the alternative is telling
       the other end to stop - which is a window of zero and a conversation
       that has to be restarted by hand. */
    boot_uint8_t ring[TCP_RECEIVE_RING];
    boot_uint32_t ring_head;       /* next byte to be taken */
    boot_uint32_t ring_tail;       /* next byte to be filled */

    int reset;                     /* they refused or gave up */
} CONNECTION;

static CONNECTION connections[TCP_CONNECTIONS];
static boot_uint16_t next_local_port = 49152;   /* the ephemeral range */

static void log(const char* text) { serial_write(text); }
static void log_dec(boot_uint64_t value) { serial_write_dec(value); }

static boot_uint32_t ring_used(const CONNECTION* c) {
    return c->ring_tail - c->ring_head;
}

static boot_uint32_t ring_free(const CONNECTION* c) {
    return TCP_RECEIVE_RING - ring_used(c) - 1;
}

/* ---- Sending ------------------------------------------------------------- */

static int send_segment(CONNECTION* c, boot_uint8_t flags,
                        const boot_uint8_t* data, boot_uint32_t length) {
    boot_uint8_t* tcp;
    boot_uint16_t sum;
    boot_uint32_t total = 20 + length;

    if (NET_ETHERNET_HEADER + 20 + total > NET_FRAME_MAX) return 0;

    tcp = net_begin_ip_frame(c->remote_mac, c->remote_address,
                             NET_IP_PROTOCOL_TCP, total);
    memset(tcp, 0, 20);
    net_put_be16(tcp + 0, c->local_port);
    net_put_be16(tcp + 2, c->remote_port);
    net_put_be32(tcp + 4, c->send_next);
    net_put_be32(tcp + 8, (flags & TCP_ACK) ? c->receive_next : 0);
    tcp[12] = 5 << 4;                       /* five 32-bit words, no options */
    tcp[13] = flags;
    /* What we can still take. The other end is not allowed to send more than
       this, which is what stops a page arriving faster than it is read. */
    net_put_be16(tcp + 14, (boot_uint16_t)(ring_free(c) > 0xFFFF
                                           ? 0xFFFF : ring_free(c)));
    if (length) memcpy(tcp + 20, data, length);

    sum = (boot_uint16_t)~net_checksum_partial(tcp, total,
              net_pseudo_header_sum(net_address(), c->remote_address,
                                    NET_IP_PROTOCOL_TCP,
                                    (boot_uint16_t)total));
    net_put_be16(tcp + 16, sum ? sum : 0xFFFF);
    return net_send_frame(NET_ETHERNET_HEADER + 20 + total);
}

/* Send data and remember it until it is acknowledged. */
static int send_and_hold(CONNECTION* c, boot_uint8_t flags,
                         const boot_uint8_t* data, boot_uint32_t length) {
    if (length > TCP_SEND_MAX) length = TCP_SEND_MAX;
    if (length) memcpy(c->pending, data, length);
    c->pending_length = length;
    c->pending_sequence = c->send_next;
    c->pending_sent_at = timer_ticks();
    c->pending_tries = 1;
    if (!send_segment(c, flags, data, length)) return 0;
    /* SYN and FIN each occupy one sequence number although they carry no
       data. That is what makes an acknowledgement of them distinguishable
       from an acknowledgement of nothing. */
    c->send_next += length + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
    return 1;
}

/* An acknowledgement carrying nothing. Sent whenever data arrives, because
   the other end is waiting for it before sending more. */
static void send_ack(CONNECTION* c) {
    send_segment(c, TCP_ACK, (const boot_uint8_t*)0, 0);
}

/* ---- What arrives -------------------------------------------------------- */

static CONNECTION* find(boot_uint32_t remote, boot_uint16_t remote_port,
                        boot_uint16_t local_port) {
    for (int index = 0; index < TCP_CONNECTIONS; index++) {
        CONNECTION* c = &connections[index];

        if (c->state == STATE_FREE) continue;
        if (c->local_port != local_port) continue;
        if (c->remote_port != remote_port) continue;
        if (c->remote_address != remote) continue;
        return c;
    }
    return (CONNECTION*)0;
}

void tcp_receive_segment(boot_uint32_t source, boot_uint32_t destination,
                         const boot_uint8_t* segment, boot_uint32_t length) {
    CONNECTION* c;
    boot_uint16_t source_port;
    boot_uint16_t destination_port;
    boot_uint32_t sequence;
    boot_uint32_t acknowledged;
    boot_uint32_t header;
    boot_uint8_t flags;
    const boot_uint8_t* data;
    boot_uint32_t data_length;

    (void)destination;
    if (length < 20) return;

    source_port = net_get_be16(segment + 0);
    destination_port = net_get_be16(segment + 2);
    sequence = net_get_be32(segment + 4);
    acknowledged = net_get_be32(segment + 8);
    header = (boot_uint32_t)(segment[12] >> 4) * 4;
    flags = segment[13];
    if (header < 20 || header > length) return;
    data = segment + header;
    data_length = length - header;

    c = find(source, source_port, destination_port);
    if (!c) return;                 /* nothing here asked for this */

    if (flags & TCP_RST) {
        c->reset = 1;
        c->state = STATE_CLOSED;
        return;
    }

    switch (c->state) {
    case STATE_SYN_SENT:
        if (!(flags & TCP_SYN) || !(flags & TCP_ACK)) return;
        if (acknowledged != c->send_next) return;
        c->receive_next = sequence + 1;
        c->send_unacknowledged = acknowledged;
        c->pending_length = 0;
        c->state = STATE_ESTABLISHED;
        send_ack(c);
        return;

    case STATE_ESTABLISHED:
    case STATE_FIN_SENT:
    case STATE_THEIRS_CLOSED:
        break;

    default:
        return;
    }

    /* Anything of ours they have confirmed is no longer in flight. */
    if (flags & TCP_ACK) {
        if (acknowledged - c->send_unacknowledged <= c->send_next -
                                                     c->send_unacknowledged) {
            c->send_unacknowledged = acknowledged;
            if (acknowledged == c->send_next) c->pending_length = 0;
        }
    }

    if (data_length) {
        if (sequence == c->receive_next) {
            boot_uint32_t room = ring_free(c);
            boot_uint32_t take = data_length < room ? data_length : room;

            for (boot_uint32_t index = 0; index < take; index++)
                c->ring[(c->ring_tail + index) % TCP_RECEIVE_RING] =
                    data[index];
            c->ring_tail += take;
            c->receive_next += take;
            send_ack(c);
        } else {
            /* Out of order, or something we have already taken. Either way
               the answer is the same: say again what we are waiting for. The
               other end sends it, and nothing here has to remember a hole. */
            send_ack(c);
            return;
        }
    }

    if ((flags & TCP_FIN) && sequence + data_length == c->receive_next) {
        c->receive_next++;
        send_ack(c);
        if (c->state == STATE_FIN_SENT) c->state = STATE_CLOSED;
        else c->state = STATE_THEIRS_CLOSED;
    }

    if (c->state == STATE_FIN_SENT && c->send_unacknowledged == c->send_next &&
        !(flags & TCP_FIN)) {
        /* They have taken our goodbye but have not said theirs. Nothing more
           to do here; the wait below gives up on its own. */
    }
}

/* ---- Waiting ------------------------------------------------------------- */

/* One pass of everything that makes progress happen: frames in, and whatever
   is in flight sent again if it has been too long.
 *
 * Three seconds, tripled each time, three tries. A network that drops one
 * packet in ten still works; one that drops everything is told so in under
 * ten seconds rather than in a minute. */
#define RETRY_MS 3000
#define RETRY_LIMIT 3

static void pump(CONNECTION* c) {
    net_poll();
    if (!c->pending_length && c->state != STATE_SYN_SENT) return;
    if (c->state == STATE_CLOSED) return;
    if (!timer_expired(c->pending_sent_at, RETRY_MS)) return;
    if (c->pending_tries >= RETRY_LIMIT) return;

    c->pending_tries++;
    c->pending_sent_at = timer_ticks();
    log("TCP: nothing came back; sending again\n");
    if (c->state == STATE_SYN_SENT) {
        boot_uint32_t was = c->send_next;
        c->send_next = c->pending_sequence;
        send_segment(c, TCP_SYN, (const boot_uint8_t*)0, 0);
        c->send_next = was;
    } else {
        boot_uint32_t was = c->send_next;
        c->send_next = c->pending_sequence;
        send_segment(c, TCP_ACK | TCP_PSH, c->pending, c->pending_length);
        c->send_next = was;
    }
}

/* ---- What the rest of the system uses ------------------------------------ */

int tcp_connect(boot_uint32_t address, boot_uint16_t port,
                boot_uint32_t timeout_ms) {
    int answer;

    gate_enter(&network_gate);
    answer = tcp_connect_locked(address, port, timeout_ms);
    gate_leave(&network_gate);
    return answer;
}

static int tcp_connect_locked(boot_uint32_t address, boot_uint16_t port,
                              boot_uint32_t timeout_ms) {
    CONNECTION* c = (CONNECTION*)0;
    int handle = -1;
    boot_uint64_t started;

    if (!net_configured() || !address || !port) return -1;

    for (int index = 0; index < TCP_CONNECTIONS; index++)
        if (connections[index].state == STATE_FREE) {
            c = &connections[index];
            handle = index;
            break;
        }
    if (!c) return -1;

    memset(c, 0, sizeof(*c));
    c->remote_address = address;
    c->remote_port = port;
    c->local_port = next_local_port++;
    if (next_local_port < 49152) next_local_port = 49152;
    if (!net_hardware_for(address, c->remote_mac)) {
        c->state = STATE_FREE;
        return -1;
    }

    /* A starting sequence number that is not zero and not the same every
       time. The clock is not a good random number and does not have to be:
       what this prevents is a segment from a previous conversation on the
       same pair of ports being believed, and a number that moves is enough
       for that. */
    c->send_next = (boot_uint32_t)(timer_ticks() * 2654435761U) | 1;
    c->send_unacknowledged = c->send_next;
    c->state = STATE_SYN_SENT;

    if (!send_and_hold(c, TCP_SYN, (const boot_uint8_t*)0, 0)) {
        c->state = STATE_FREE;
        return -1;
    }

    started = timer_ticks();
    while (c->state == STATE_SYN_SENT) {
        pump(c);
        task_yield();
        if (timer_expired(started, timeout_ms)) {
            log("TCP: no answer to the connection\n");
            c->state = STATE_FREE;
            return -1;
        }
    }
    if (c->state != STATE_ESTABLISHED) {
        c->state = STATE_FREE;
        return -1;
    }
    return handle;
}



static CONNECTION* connection_of(int handle) {
    if (handle < 0 || handle >= TCP_CONNECTIONS) return (CONNECTION*)0;
    if (connections[handle].state == STATE_FREE) return (CONNECTION*)0;
    return &connections[handle];
}

int tcp_send(int handle, const void* data, boot_uint32_t length,
             boot_uint32_t timeout_ms) {
    int answer;

    gate_enter(&network_gate);
    answer = tcp_send_locked(handle, data, length, timeout_ms);
    gate_leave(&network_gate);
    return answer;
}

static int tcp_send_locked(int handle, const void* data, boot_uint32_t length,
                           boot_uint32_t timeout_ms) {
    CONNECTION* c = connection_of(handle);
    const boot_uint8_t* at = (const boot_uint8_t*)data;
    boot_uint32_t sent = 0;

    if (!c || !data) return -1;
    if (c->state != STATE_ESTABLISHED && c->state != STATE_THEIRS_CLOSED)
        return -1;

    while (sent < length) {
        boot_uint32_t piece = length - sent;
        boot_uint64_t started;

        if (piece > TCP_SEND_MAX) piece = TCP_SEND_MAX;
        if (!send_and_hold(c, TCP_ACK | TCP_PSH, at + sent, piece)) return -1;

        /* One in flight at a time: wait for it to be taken before sending
           the next. See the note at the top about why. */
        started = timer_ticks();
        while (c->pending_length) {
            pump(c);
            if (c->reset) return -1;
            if (timer_expired(started, timeout_ms)) return (int)sent;
        }
        sent += piece;
    }
    return (int)sent;
}

int tcp_receive(int handle, void* buffer, boot_uint32_t size,
                boot_uint32_t timeout_ms) {
    int answer;

    gate_enter(&network_gate);
    answer = tcp_receive_locked(handle, buffer, size, timeout_ms);
    gate_leave(&network_gate);
    return answer;
}

static int tcp_receive_locked(int handle, void* buffer, boot_uint32_t size,
                              boot_uint32_t timeout_ms) {
    CONNECTION* c = connection_of(handle);
    boot_uint8_t* out = (boot_uint8_t*)buffer;
    boot_uint64_t started;

    if (!c || !buffer || !size) return -1;

    started = timer_ticks();
    for (;;) {
        boot_uint32_t have = ring_used(c);

        if (have) {
            boot_uint32_t take = have < size ? have : size;

            for (boot_uint32_t index = 0; index < take; index++)
                out[index] = c->ring[(c->ring_head + index) % TCP_RECEIVE_RING];
            c->ring_head += take;
            /* Room again, and the other end is waiting to hear it: our window
               was what stopped them. */
            if (c->state == STATE_ESTABLISHED) send_ack(c);
            return (int)take;
        }
        /* Nothing waiting, and nothing more coming. */
        if (c->state == STATE_THEIRS_CLOSED || c->state == STATE_CLOSED)
            return 0;
        if (c->reset) return -1;
        pump(c);
        /* Nothing of this connection is half done at this point, so the
           processor can go to somebody else until the next packet. This one
           line is the difference between a desktop that keeps drawing while a
           page loads and a machine that stops until it has. */
        task_yield();
        if (timer_expired(started, timeout_ms)) return -1;
    }
}

int tcp_close(int handle, boot_uint32_t timeout_ms) {
    int answer;

    gate_enter(&network_gate);
    answer = tcp_close_locked(handle, timeout_ms);
    gate_leave(&network_gate);
    return answer;
}

static int tcp_close_locked(int handle, boot_uint32_t timeout_ms) {
    CONNECTION* c = connection_of(handle);
    boot_uint64_t started;

    if (!c) return -1;
    if (c->state == STATE_ESTABLISHED || c->state == STATE_THEIRS_CLOSED) {
        send_and_hold(c, TCP_ACK | TCP_FIN, (const boot_uint8_t*)0, 0);
        c->state = c->state == STATE_THEIRS_CLOSED ? STATE_CLOSED
                                                   : STATE_FIN_SENT;
        started = timer_ticks();
        while (c->state == STATE_FIN_SENT) {
            pump(c);
            task_yield();
            if (timer_expired(started, timeout_ms)) break;
        }
    }
    c->state = STATE_FREE;
    return 0;
}

int tcp_is_open(int handle) {
    CONNECTION* c = connection_of(handle);

    if (!c) return 0;
    if (c->reset) return 0;
    /* Bytes already here count as open: a server that answers and hangs up
       has still answered, and a caller that stopped reading at the close
       would lose the last of the page. */
    return c->state == STATE_ESTABLISHED || c->state == STATE_THEIRS_CLOSED ||
           ring_used(c) != 0;
}

void tcp_report(int handle) {
    CONNECTION* c = connection_of(handle);

    if (!c) { log("TCP: no such connection\n"); return; }
    log("TCP: state ");
    log_dec((boot_uint64_t)c->state);
    log(", waiting ");
    log_dec(ring_used(c));
    log(" bytes\n");
}
