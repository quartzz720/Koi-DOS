#ifndef KERNEL_COMMAND_H
#define KERNEL_COMMAND_H

/* The command interpreter. Named after the file it will eventually be, once
   programs can be loaded from disk and this stops living inside the kernel. */
__attribute__((noreturn)) void command_run(void);

#endif
