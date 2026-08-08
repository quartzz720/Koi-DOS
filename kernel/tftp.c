#include "tftp.h"
#include "net.h"
#include "string.h"
#include "serial.h"
#include "timer.h"

/* TFTP, which is what this system can speak today.
 *
 * Not a choice of taste. There is UDP, ARP and IP here, all of it proved
 * against real hardware; there is no TCP, so HTTP is a stack away and this is
 * a hundred lines. It is also the protocol that gets a failing USB stick out
 * of the loop this week rather than next month, which is the whole reason it
 * exists.
 *
 * It is meant to be thrown away. When TCP arrives this is replaced by an
 * ordinary HTTP client and one setting changes to point at an ordinary web
 * server - which is why that setting is a setting.
 *
 * The protocol, in full: ask for a file, and the server sends it in numbered
 * blocks of 512 bytes, each acknowledged before the next arrives. A block
 * shorter than 512 is the last one. A file that is an exact multiple of the
 * block size therefore ends with an empty block, which is the one detail
 * everybody gets wrong the first time.
 */

#define TFTP_PORT 69
#define TFTP_BLOCK 512

#define OPCODE_READ 1
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5

/* Our end of the conversation. TFTP calls it a transfer identifier and it is
   an ordinary source port; the server replies from a fresh port of its own and
   every later packet goes there, not back to 69. */
#define TFTP_CLIENT_PORT 20069

static void log(const char* text) { serial_write(text); }
static void log_dec(boot_uint64_t value) { serial_write_dec(value); }

static void put_be16(boot_uint8_t* at, boot_uint16_t value) {
    at[0] = (boot_uint8_t)(value >> 8);
    at[1] = (boot_uint8_t)value;
}

static boot_uint16_t get_be16(const boot_uint8_t* at) {
    return (boot_uint16_t)((at[0] << 8) | at[1]);
}

static boot_uint32_t append(boot_uint8_t* out, boot_uint32_t at,
                            const char* text) {
    while (*text) out[at++] = (boot_uint8_t)*text++;
    out[at++] = 0;
    return at;
}

int tftp_fetch(boot_uint32_t server, const char* name, void* buffer,
               boot_uint32_t size, const char** why) {
    boot_uint8_t request[128];
    boot_uint8_t packet[TFTP_BLOCK + 4];
    boot_uint8_t* out = (boot_uint8_t*)buffer;
    boot_uint32_t at = 0;
    boot_uint32_t received = 0;
    boot_uint16_t expected = 1;
    boot_uint16_t server_port = TFTP_PORT;
    int first = 1;

    if (why) *why = (const char*)0;
    if (!server || !name || !buffer) {
        if (why) *why = "nothing to ask for";
        return -1;
    }

    put_be16(request, OPCODE_READ);
    at = 2;
    at = append(request, at, name);
    /* Binary, always. The alternative translates line endings, which is
       exactly wrong for the things this fetches. */
    at = append(request, at, "octet");

    net_listen(TFTP_CLIENT_PORT);

    for (;;) {
        int got;
        boot_uint32_t from = 0;
        boot_uint16_t port = 0;
        int attempt;

        /* The request goes to port 69; every acknowledgement afterwards goes
           to whichever port the server answered from. */
        for (attempt = 0; attempt < 4; attempt++) {
            if (first) {
                if (!net_send_from(TFTP_CLIENT_PORT, server, TFTP_PORT,
                                   request, at))
                    break;
            } else {
                boot_uint8_t ack[4];
                put_be16(ack, OPCODE_ACK);
                put_be16(ack + 2, (boot_uint16_t)(expected - 1));
                if (!net_send_from(TFTP_CLIENT_PORT, server, server_port,
                                   ack, 4))
                    break;
            }

            got = net_receive_from(packet, sizeof(packet), 2000, &from, &port);
            if (got >= 4) break;
        }

        if (attempt >= 4 || got < 4) {
            net_listen(0);
            if (why) *why = "the server stopped answering";
            return -1;
        }

        if (get_be16(packet) == OPCODE_ERROR) {
            net_listen(0);
            /* The server's own words follow the code, and they are more use
               than anything this could say instead. */
            if (why) *why = "the server refused the file";
            return -1;
        }
        if (get_be16(packet) != OPCODE_DATA) continue;
        if (get_be16(packet + 2) != expected) {
            /* A block we have already taken, sent again because our
               acknowledgement went missing. Acknowledged, not stored. */
            continue;
        }

        server_port = port;
        first = 0;

        {
            boot_uint32_t payload = (boot_uint32_t)got - 4;

            if (received + payload > size) {
                net_listen(0);
                if (why) *why = "the file is larger than there is room for";
                return -1;
            }
            memcpy(out + received, packet + 4, payload);
            received += payload;

            if (payload < TFTP_BLOCK) {
                /* The last block. One final acknowledgement, unretried: if it
                   is lost the server repeats the block and we are gone, which
                   costs the server a timeout and costs us nothing. */
                boot_uint8_t ack[4];
                put_be16(ack, OPCODE_ACK);
                put_be16(ack + 2, expected);
                (void)net_send_from(TFTP_CLIENT_PORT, server, server_port,
                                    ack, 4);
                net_listen(0);
                return (int)received;
            }
        }
        expected++;
    }
}

void tftp_report(boot_uint32_t bytes) {
    log("TFTP: ");
    log_dec(bytes);
    log(" bytes\n");
}
