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

/* Everything one program was holding - files, searches, memory, sound. Called
   when that program's slot is cleared away, whether or not anybody was
   waiting for it. */
void syscall_close_owner(int owner);

/* Whether a program is collecting output rather than it reaching a screen.
   Anything that would stop and wait for a keystroke has to ask: in a captured
   command there is nobody able to press one. */
int syscall_capturing(void);

/* Whether any file or directory search is open. A disk rescan is only safe
   when nothing is. */
int syscall_files_open(void);

#endif
