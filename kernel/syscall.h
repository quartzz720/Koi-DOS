#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include "partition.h"

/* Kernel side of the system call interface. The ABI itself - vector numbers,
   function numbers, register convention - lives in include/syscall.h, which
   programs include too. */

/* Where a program's relative paths are resolved from: the drive, and the
   directory on it the user was standing in. Set by the shell before starting
   one. Passing the volume without the directory would leave every program
   working from the root however deep the user had navigated. */
void syscall_set_location(VOLUME* volume, const char* directory);

/* Drop every open handle. Called when a program ends, so a program that exits
   without closing its files does not leak them into the next one. */
void syscall_close_all(void);

#endif
