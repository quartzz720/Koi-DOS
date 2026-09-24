#include "http.h"
#include "tcp.h"
#include "net.h"
#include "string.h"
#include "serial.h"

/* Fetching a file over HTTP, for the package manager.
 *
 * TFTP is what dosget used, and it worked; what it could not do is go fast.
 * The protocol sends one block and waits for it to be acknowledged before
 * sending the next, so a server 61 ms away delivers one block per 61 ms
 * whatever the wire is capable of - twenty-one kilobytes a second, measured
 * twice, and no amount of tuning moves it because the limit is the speed of
 * light. Windowed TFTP with pacing at both ends reached 124 KiB/s and needed
 * both ends changed to do it.
 *
 * TCP does not have that shape: the far end sends a window's worth and keeps
 * sending. So this is the same job over the protocol the machine now has.
 *
 * HTTP/1.0 with `Connection: close`, which makes the reply end when the
 * connection does - no chunked encoding to unpick, no keep-alive to manage,
 * and the whole client is one request and one read loop. A package server is
 * not a browser and does not need to be.
 */

#define HTTP_PORT 80
#define HTTP_TIMEOUT 10000

static void (*report_total)(boot_uint32_t);
static void (*report_received)(boot_uint32_t);

void http_progress(void (*total)(boot_uint32_t),
                   void (*received)(boot_uint32_t)) {
    report_total = total;
    report_received = received;
}

static int same_ignoring_case(const char* a, const char* b, int length) {
    for (int at = 0; at < length; at++) {
        char left = a[at];
        char right = b[at];

        if (left >= 'A' && left <= 'Z') left = (char)(left + 32);
        if (right >= 'A' && right <= 'Z') right = (char)(right + 32);
        if (left != right) return 0;
    }
    return 1;
}

static void append(char* into, boot_uint32_t size, const char* text) {
    boot_uint32_t at = 0;

    while (into[at] && at + 1 < size) at++;
    while (*text && at + 1 < size) into[at++] = *text++;
    into[at] = 0;
}

int http_fetch(boot_uint32_t server, const char* path, void* buffer,
               boot_uint32_t size, const char** why) {
    char request[512];
    char host[24];
    boot_uint8_t* out = (boot_uint8_t*)buffer;
    boot_uint32_t have = 0;
    int connection;
    int headers_done = 0;
    boot_uint32_t body = 0;
    int status = 0;

    if (why) *why = (const char*)0;
    if (!server || !path || !buffer || !size) {
        if (why) *why = "nothing to fetch";
        return -1;
    }
    if (!net_configured()) {
        if (why) *why = "this machine has no address";
        return -1;
    }

    connection = tcp_connect(server, HTTP_PORT, HTTP_TIMEOUT);
    if (connection < 0) {
        if (why) *why = "nothing answered on port 80";
        return -1;
    }

    /* The Host header carries the address as written, which is what a server
       with one site does not need and a server behind a name does. Written
       as digits rather than a name because that is what dosget has. */
    net_format_address(server, host);

    request[0] = 0;
    append(request, sizeof(request), "GET /");
    append(request, sizeof(request), path);
    append(request, sizeof(request), " HTTP/1.0\r\nHost: ");
    append(request, sizeof(request), host);
    append(request, sizeof(request),
           "\r\nUser-Agent: Koi-DOS dosget\r\nConnection: close\r\n\r\n");

    {
        boot_uint32_t length = (boot_uint32_t)strlen(request);

        if (tcp_send(connection, request, length, HTTP_TIMEOUT) !=
            (int)length) {
            tcp_close(connection, 1000);
            if (why) *why = "the request could not be sent";
            return -1;
        }
    }

    /* Headers and body arrive in the same stream and the split is a blank
     * line. Rather than buffer the headers separately, everything is read
     * into the caller's buffer and the body is moved down over them once the
     * split is found - one copy of at most a few hundred bytes, and no second
     * buffer to size. */
    for (;;) {
        int got;

        if (have >= size) break;
        got = tcp_receive(connection, out + have, size - have, HTTP_TIMEOUT);
        if (got <= 0) break;
        have += (boot_uint32_t)got;

        if (!headers_done) {
            for (boot_uint32_t at = 0; at + 3 < have; at++) {
                if (out[at] == 13 && out[at + 1] == 10 && out[at + 2] == 13 &&
                    out[at + 3] == 10) {
                    body = at + 4;
                    headers_done = 1;
                    break;
                }
            }
            if (headers_done) {
                /* "HTTP/1.x NNN": the three digits are all this needs. */
                if (have > 12 && same_ignoring_case((const char*)out, "http/", 5))
                    status = (out[9] - '0') * 100 + (out[10] - '0') * 10 +
                             (out[11] - '0');
                /* And how long it says it will be, for the progress line. */
                if (report_total) {
                    for (boot_uint32_t at = 0; at + 16 < body; at++) {
                        if (!same_ignoring_case((const char*)out + at,
                                                "content-length:", 15)) continue;
                        {
                            boot_uint32_t scan = at + 15;
                            boot_uint32_t value = 0;

                            while (scan < body && out[scan] == ' ') scan++;
                            while (scan < body && out[scan] >= '0' &&
                                   out[scan] <= '9')
                                value = value * 10 + (boot_uint32_t)(out[scan++] - '0');
                            if (value) report_total(value);
                        }
                        break;
                    }
                }
                /* The headers go, and everything after them slides down. */
                for (boot_uint32_t at = 0; at + body < have; at++)
                    out[at] = out[at + body];
                have -= body;
            }
        }
        if (headers_done && report_received) report_received(have);
    }

    tcp_close(connection, 2000);

    if (!headers_done) {
        if (why) *why = "the answer was not HTTP";
        return -1;
    }
    if (status && status != 200) {
        /* Said as a number rather than a sentence: 404 means the package is
           not there and 500 means the server is unwell, and a caller that
           prints the number can be told which by anybody. */
        if (why) *why = status == 404 ? "the server has no such file"
                                      : "the server refused";
        return -1;
    }
    return (int)have;
}
