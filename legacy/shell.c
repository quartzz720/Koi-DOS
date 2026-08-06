#include "../include/efi.h"
#include "console.h"
#include "commands.h"
#include "fs.h"
#include "shell.h"

// ============================================================================
// Command Table
// ============================================================================

typedef struct {
    CHAR16* name;
    void (*handler)(CHAR16* args);
} Command;

static Command commands[] = {
    { L"cls",    (void*)clear },
    { L"help",   (void*)cmd_help },
    { L"ver",    (void*)cmd_ver },
    { L"dir",    (void*)cmd_dir },
    { L"ls",     (void*)cmd_dir },
    { L"echo",   cmd_echo },
    { L"type",   cmd_type },
    { L"mkdir",  cmd_mkdir },
    { L"write",  cmd_write },
    { L"cd",     cmd_cd },
    { L"del",    cmd_del },
    { L"ren",    cmd_ren },
    { L"rmdir",  cmd_rmdir },
    { L"copy",   cmd_copy },
    { L"move",   cmd_move },
    { L"more",   cmd_more },
    { L"tree",   cmd_tree },
    { L"edit",   cmd_edit },
    { L"color",   cmd_color },
    { L"quit",   cmd_quit },
    { L"exit",   cmd_quit },
    { NULL, NULL }
};

// ============================================================================
// Command Lookup
// ============================================================================

static Command* find_command(CHAR16* input) {
    if (!input || input[0] == 0) return NULL;
    
    for (int i = 0; commands[i].name != NULL; i++) {
        if (streq(input, commands[i].name)) {
            return &commands[i];
        }
        // Also check for commands with arguments (e.g., "echo hello")
        UINTN cmd_len = strlen16(commands[i].name);
        if (startswith(input, commands[i].name) && 
            input[cmd_len] == L' ') {
            return &commands[i];
        }
    }
    return NULL;
}

// ============================================================================
// Shell Main Loop
// ============================================================================

void shell(void) {
    CHAR16 input[256];
    while (1) {
        set_color(EFI_GREEN, EFI_BGBLACK);
        print(L"Z:"); print(cwd); print(L"> ");
        set_color(EFI_WHITE, EFI_BGBLACK);
        read_line(input, 256);
        
        if (input[0] == 0) {
            // Empty command, do nothing
            continue;
        }
        
        Command* cmd = find_command(input);
        if (cmd) {
            // Special handling for commands with no arguments
            if (streq(input, L"cls")) {
                clear();
            } else if (streq(input, L"help")) {
                cmd_help();
            } else if (streq(input, L"ver")) {
                cmd_ver();
            } else if (streq(input, L"dir") || streq(input, L"ls")) {
                cmd_dir();
            } else if (streq(input, L"quit") || streq(input, L"exit")) {
                cmd_quit(input);
            
            } else {
                // Commands with arguments
                cmd->handler(input);
            }
        } else {
            set_color(EFI_RED, EFI_BGBLACK);
            print(L"Unknown command: "); println(input);
            set_color(EFI_WHITE, EFI_BGBLACK);
        }
    }
}
