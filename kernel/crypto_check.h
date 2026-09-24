#ifndef KERNEL_CRYPTO_CHECK_H
#define KERNEL_CRYPTO_CHECK_H

/* Run the published test vectors against this machine's own build of the
   cryptography, printing each one. Returns the number that failed. */
/* `write` is how each line is reported - the caller's own printer, so the
   report lands wherever that caller's output lands. */
int crypto_check(void (*write)(const char*));

#endif
