#ifndef COMMANDS_H
#define COMMANDS_H

#include "../include/efi.h"

void cmd_help(void);
void cmd_ver(void);
void cmd_echo(CHAR16* input);
void cmd_color(CHAR16* input);
void cmd_quit(CHAR16* input);

#endif
