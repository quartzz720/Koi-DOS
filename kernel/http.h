#ifndef KERNEL_HTTP_H
#define KERNEL_HTTP_H

#include "../include/bootinfo.h"

/* Fetch one file over HTTP into memory.
 *
 * The same job tftp_fetch does, over the protocol this machine gained when it
 * learned TCP - and for the reason that matters: TFTP sends one block per
 * round trip, so a server 61 ms away is a ceiling of about twenty kilobytes a
 * second no matter what the wire can do. TCP has no such shape.
 *
 * `path` is what follows the slash: "INDEX", "packages/MIZU/MANIFEST".
 * Returns the number of bytes, or -1 with `why` set to a printable sentence.
 */
int http_fetch(boot_uint32_t server, const char* path, void* buffer,
               boot_uint32_t size, const char** why);

/* Where to report progress, for as long as it is set. `total` is called once
   if the server said how long the file is; `received` as it arrives. */
void http_progress(void (*total)(boot_uint32_t),
                   void (*received)(boot_uint32_t));

#endif
