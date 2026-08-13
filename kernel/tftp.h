#ifndef KERNEL_TFTP_H
#define KERNEL_TFTP_H

#include "../include/bootinfo.h"

/* Fetch one file into memory.
 *
 * The protocol this system can speak today: UDP, which is proved, rather than
 * TCP, which does not exist here yet. Deliberately temporary - when there is an
 * HTTP client this goes, and the package manager's `source` setting changes to
 * point at an ordinary web server instead. The setting exists so that the day
 * this is thrown away costs one line.
 *
 * Returns the number of bytes fetched, or -1 with `why` set to a sentence that
 * can be printed. */
int tftp_fetch(boot_uint32_t server, const char* name, void* buffer,
               boot_uint32_t size, const char** why);

/* Where to report progress to, for as long as it is set.
 *
 * `total` is called once when the server says how big the file is - which it
 * may not, in which case it is never called and the caller shows what it can
 * without a percentage. `received` is called as blocks arrive. */
void tftp_progress(void (*total)(boot_uint32_t),
                   void (*received)(boot_uint32_t));

void tftp_report(boot_uint32_t bytes);

#endif
