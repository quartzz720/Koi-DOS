#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include "partition.h"

/* Kernel side of the system call interface. The ABI itself - vector numbers,
   function numbers, register convention - lives in include/syscall.h, which
   programs include too. */

/* The drive a program's relative paths are resolved against. Set by the shell
   before starting one. */
void syscall_set_volume(VOLUME* volume);

/* Drop every open handle. Called when a program ends, so a program that exits
   without closing its files does not leak them into the next one. */
void syscall_close_all(void);

#endif
