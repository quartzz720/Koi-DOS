#ifndef KERNEL_COMMAND_H
#define KERNEL_COMMAND_H

/* The command interpreter. Named after the file it will eventually be, once
   programs can be loaded from disk and this stops living inside the kernel. */
__attribute__((noreturn)) void command_run(void);

/* Run one command line, as though it had been typed, and return when it has
   finished. This is what SYS_RUN calls: a program asking the shell to do
   something is asking for exactly what a person typing would get, including
   the built-in commands and the search path. */
/* Run one command line, and give back what it exited with -
   KOI_EXIT_NOT_FOUND when there was no such command. */
int command_execute_line(const char* line);

#endif
