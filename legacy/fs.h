#ifndef FS_H
#define FS_H

#include "../include/efi.h"

extern CHAR16 cwd[256];

EFI_FILE_PROTOCOL* open_root(void);
void cmd_dir(void);
void cmd_cd(CHAR16* input);
void cmd_type(CHAR16* input);
void cmd_mkdir(CHAR16* input);
void cmd_write(CHAR16* input);
void cmd_del(CHAR16* input);
void cmd_ren(CHAR16* input);
void cmd_rmdir(CHAR16* input);
void cmd_copy(CHAR16* input);
void cmd_move(CHAR16* input);
void cmd_more(CHAR16* input);
void cmd_tree(CHAR16* input);
void cmd_edit(CHAR16* input);

#endif
