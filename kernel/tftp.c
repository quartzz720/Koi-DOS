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

/* What this asks for, and the most it will accept.
 *
 * 1428 fits inside a 1500-byte path with room for the IP and UDP headers and
 * some to spare for a tunnel - a phone tethering over USB is often behind one,
 * and a block that has to be fragmented is a block that arrives more slowly
 * than a smaller one would have. The maximum is what the frame buffer can
 * actually hold: 1500 less 20 for IP and 8 for UDP, less TFTP's own 4. */
#define TFTP_BLOCK_TEXT "1428"
#define TFTP_BLOCK_MAX 1468
/* Eight blocks may be in flight before one has to be acknowledged.
 *
 * The measurement, on the day this changed: 332208 bytes from the package
 * server, ping 61 ms. One block at a time, 15.0 s - 21.6 KiB/s. Four, 4.7 s.
 * Eight, 2.5 s - 131 KiB/s, six times faster over the same line, because the
 * line was never what was slow. A window of one spends its whole life waiting
 * for the far end to say "yes, go on".
 *
 * A window was tried once before and made things worse, and the difference is
 * the server: it now leaves two milliseconds between the packets of a window
 * instead of firing them off as fast as it can. The window bounds what is
 * unacknowledged, the pacing bounds how fast it arrives, and it is the second
 * one that stops a burst overrunning something narrow on the way. See
 * koi-tftpd.py, where the same measurement is written down. */
#define TFTP_WINDOW_TEXT "8"
#define TFTP_WINDOW_MAX 8

#define OPCODE_READ 1
#define OPCODE_DATA 3
#define OPCODE_ACK 4
#define OPCODE_ERROR 5
#define OPCODE_OACK 6

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

static void tftp_progress_total(boot_uint32_t total);
static void tftp_progress_bytes(boot_uint32_t received);

/* The next transfer identifier to speak from. Anything above the well-known
   range and below the top; it only has to differ from the one before. */
static boot_uint16_t next_client_port(void) {
    static boot_uint16_t rotating;

    if (!rotating) rotating = TFTP_CLIENT_PORT;
    if (++rotating < TFTP_CLIENT_PORT) rotating = TFTP_CLIENT_PORT;
    return rotating;
}

/* An option name, as the server spelled it back. Case-insensitive, because
   the standard says the names are and servers vary. */
static int same_option(const char* left, const char* right) {
    while (*left && *right) {
        char a = *left, b = *right;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
        left++;
        right++;
    }
    return !*left && !*right;
}

static boot_uint32_t append(boot_uint8_t* out, boot_uint32_t at,
                            const char* text) {
    while (*text) out[at++] = (boot_uint8_t)*text++;
    out[at++] = 0;
    return at;
}

int tftp_fetch(boot_uint32_t server, const char* name, void* buffer,
               boot_uint32_t size, const char** why) {
    boot_uint8_t request[160];
    boot_uint8_t packet[TFTP_BLOCK_MAX + 4];
    boot_uint8_t* out = (boot_uint8_t*)buffer;
    boot_uint32_t at = 0;
    boot_uint32_t plain_length = 0;
    boot_uint32_t received = 0;
    boot_uint16_t expected = 1;
    boot_uint16_t server_port = TFTP_PORT;
    boot_uint16_t client_port;
    boot_uint32_t block_size = TFTP_BLOCK;      /* until the server agrees to more */
    boot_uint32_t window = 1;                   /* blocks between acknowledgements */
    boot_uint32_t since_ack = 0;
    boot_uint32_t promised = 0;         /* the size the server said, or none */
    int asked_options = 1;
    int speak = 1;                              /* is it our turn to send? */
    /* Whether the server has already been told where to resume from.
     *
     * With a window in flight, one lost block is followed by every block
     * behind it, and each of those is "not the one expected". Answering each
     * with an acknowledgement tells the server to restart the window that
     * many times, and the duplicates multiply until the transfer is doing
     * nothing else. Ask once, then let the rest of the window go past. */
    int asked_resend = 0;
    int attempts = 0;

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
    plain_length = at;              /* the request without any options */

    /* Three options, and the middle one is the difference between a download
     * and an afternoon.
     *
     * Plain TFTP sends 512 bytes and waits for an acknowledgement before
     * sending the next: one block per round trip, forever. The rate that
     * produces has nothing to do with the speed of the link - on a phone
     * tether at 300 ms it is 1.7 KiB/s whether the connection is 1 Mbit or
     * 100, and a 300 KiB kernel takes three minutes. Bandwidth never enters
     * the arithmetic because there is never more than one packet in flight.
     *
     *   tsize      how big the file is, so a bar can be drawn      (RFC 2349)
     *   blksize    how much fits in one packet without fragmenting (RFC 2348)
     *   windowsize how many blocks may fly before an acknowledgement
     *                                                              (RFC 7440)
     *
     * The window is what makes the difference, once the server paces what it
     * sends - see the note above TFTP_WINDOW_TEXT for the numbers and for the
     * time this was tried without pacing and was slower than not trying.
     *
     * All three are optional in both directions: a server that ignores them
     * gets the old behaviour and this still works, just slowly. */
    at = append(request, at, "tsize");
    at = append(request, at, "0");
    at = append(request, at, "blksize");
    at = append(request, at, TFTP_BLOCK_TEXT);
    at = append(request, at, "windowsize");
    at = append(request, at, TFTP_WINDOW_TEXT);

    client_port = next_client_port();
    net_listen(client_port);

    for (;;) {
        int got;
        boot_uint32_t from = 0;
        boot_uint16_t port = 0;

        if (speak) {
            int sent;

            if (expected == 1 && !received && server_port == TFTP_PORT)
                sent = net_send_from(client_port, server, TFTP_PORT, request, at);
            else {
                boot_uint8_t ack[4];
                put_be16(ack, OPCODE_ACK);
                put_be16(ack + 2, (boot_uint16_t)(expected - 1));
                sent = net_send_from(client_port, server, server_port, ack, 4);
            }
            if (!sent) {
                net_listen(0);
                if (why) *why = "the frame could not be sent";
                return -1;
            }
        }

        got = net_receive_from(packet, sizeof(packet), 2000, &from, &port);
        if (got < 4) {
            if (++attempts >= 4) {
                if (asked_options && !received) {
                    /* Asked once with options and got nothing back. A server
                     * that does not know them should ignore them and answer as
                     * usual, and most do - QEMU's built-in one says nothing at
                     * all, so asking silently broke every download on the
                     * development bench while working against the real server.
                     * Ask again the old way; only the speed is lost. */
                    log("TFTP: no answer with options, asking without\n");
                    asked_options = 0;
                    at = plain_length;
                    attempts = 0;
                    speak = 1;
                    continue;
                }
                net_listen(0);
                if (why) *why = "the server stopped answering";
                return -1;
            }
            /* Say the last thing again: either the request, or the
               acknowledgement the server is waiting on. */
            speak = 1;
            since_ack = 0;
            continue;
        }
        attempts = 0;

        if (get_be16(packet) == OPCODE_ERROR) {
            net_listen(0);
            /* The server's own words follow the code, and they are more use
               than anything this could say instead. */
            if (why) *why = "the server refused the file";
            return -1;
        }

        if (get_be16(packet) == OPCODE_OACK) {
            boot_uint32_t index = 2;

            server_port = port;
            while (index < (boot_uint32_t)got) {
                const char* key = (const char*)(packet + index);
                boot_uint32_t value;
                boot_uint32_t number = 0;

                while (index < (boot_uint32_t)got && packet[index]) index++;
                index++;
                value = index;
                while (index < (boot_uint32_t)got && packet[index]) index++;
                index++;
                for (boot_uint32_t digit = value;
                     digit < (boot_uint32_t)got && packet[digit]; digit++) {
                    if (packet[digit] < '0' || packet[digit] > '9') { number = 0; break; }
                    number = number * 10 + (boot_uint32_t)(packet[digit] - '0');
                }
                /* Only what was granted, and only within what this can hold:
                   a server answering with a larger block than was asked for
                   would otherwise overrun the buffer above. */
                if (same_option(key, "tsize")) {
                    promised = number;
                    tftp_progress_total(number);
                }
                else if (same_option(key, "blksize")) {
                    if (number >= 8 && number <= TFTP_BLOCK_MAX) block_size = number;
                } else if (same_option(key, "windowsize")) {
                    if (number >= 1 && number <= TFTP_WINDOW_MAX) window = number;
                }
            }
            log("TFTP: block ");
            log_dec(block_size);
            log(", window ");
            log_dec(window);
            log("\n");

            /* Block zero acknowledges the options; the file starts after it. */
            {
                boot_uint8_t ack[4];
                put_be16(ack, OPCODE_ACK);
                put_be16(ack + 2, 0);
                (void)net_send_from(client_port, server, server_port, ack, 4);
            }
            since_ack = 0;
            speak = 0;
            continue;
        }

        if (get_be16(packet) != OPCODE_DATA) { speak = 0; continue; }

        server_port = port;

        if (get_be16(packet + 2) != expected) {
            /* Not the block we are waiting for: either one already taken, or
               one from beyond a gap. Name the last block that did arrive and
               let the server resume from there - but only once, however many
               blocks of the window are still arriving behind it. */
            if (!asked_resend) {
                asked_resend = 1;
                speak = 1;
                since_ack = 0;
            } else {
                speak = 0;
            }
            continue;
        }
        asked_resend = 0;

        {
            boot_uint32_t payload = (boot_uint32_t)got - 4;

            if (received + payload > size) {
                net_listen(0);
                if (why) *why = "the file is larger than there is room for";
                return -1;
            }
            memcpy(out + received, packet + 4, payload);
            received += payload;
            expected++;
            since_ack++;
            tftp_progress_bytes(received);

            if (payload < block_size) {
                /* The last block. One final acknowledgement, unretried: if it
                   is lost the server repeats the block and we are gone, which
                   costs the server a timeout and costs us nothing. */
                boot_uint8_t ack[4];
                put_be16(ack, OPCODE_ACK);
                put_be16(ack + 2, (boot_uint16_t)(expected - 1));
                (void)net_send_from(client_port, server, server_port, ack, 4);
                net_listen(0);

                /* And it has to be the size the server promised.
                 *
                 * This is cheap because the size was already negotiated for
                 * the progress bar, and it is here because of what happened
                 * without it: a packet left over from the previous transfer
                 * ended this one at its first block, and a kernel was written
                 * to disk at the size of the manifest fetched before it. The
                 * bytes were a perfectly good file - just not this file, and
                 * nothing anywhere noticed. A transfer that does not deliver
                 * what was promised is a failed transfer. */
                if (promised && received != promised) {
                    log("TFTP: expected ");
                    log_dec(promised);
                    log(" bytes, got ");
                    log_dec(received);
                    log("\n");
                    if (why) *why = "the file arrived incomplete";
                    return -1;
                }
                return (int)received;
            }

            /* Acknowledge once per window rather than once per block: that is
               the whole saving, and the window is how many blocks may be in
               flight while nobody is waiting. */
            if (since_ack >= window) {
                since_ack = 0;
                speak = 1;
            } else {
                speak = 0;
            }
        }
    }
}

/* Somewhere to show how it is going.
 *
 * A hook rather than printing from here: this file knows how many bytes have
 * arrived and nothing about what the screen looks like, and the shell knows
 * the opposite. The kernel log gets a line either way. */
static void (*progress_total_hook)(boot_uint32_t total);
static void (*progress_bytes_hook)(boot_uint32_t received);

void tftp_progress(void (*total)(boot_uint32_t),
                   void (*received)(boot_uint32_t)) {
    progress_total_hook = total;
    progress_bytes_hook = received;
}

static void tftp_progress_total(boot_uint32_t total) {
    if (progress_total_hook) progress_total_hook(total);
}

static void tftp_progress_bytes(boot_uint32_t received) {
    if (progress_bytes_hook) progress_bytes_hook(received);
}

void tftp_report(boot_uint32_t bytes) {
    log("TFTP: ");
    log_dec(bytes);
    log(" bytes\n");
}
