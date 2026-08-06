#include "../include/efi.h"
#include "console.h"
#include "commands.h"
#include "hal.h"

void cmd_help(void) {
    set_color(EFI_YELLOW, EFI_BGBLACK);
    println(L"Available commands:");
    set_color(EFI_WHITE, EFI_BGBLACK);
    println(L"  help            - this help");
    println(L"  cls             - clear screen");
    println(L"  echo [msg]      - print text");
    println(L"  ver             - OS version");
    println(L"  dir / ls        - list files");
    println(L"  type [file]     - print file contents");
    println(L"  mkdir [name]    - create directory");
    println(L"  cd [dir]        - change directory");
    println(L"  cd ..           - go to parent directory");
    println(L"  write [f] [msg] - write text to file");
    println(L"  del [file]      - delete file");
    println(L"  ren [old] [new] - rename file");
    println(L"  rmdir [dir]     - delete directory");
    println(L"  rmdir /s [dir]  - recursively delete directory");
    println(L"  copy [src] [dst]- copy file");
    println(L"  move [src] [dst]- move file");
    println(L"  more [file]     - view file (paged)");
    println(L"  tree            - display directory tree");
    println(L"  edit [file]     - edit file (Up/Down=nav, Ctrl+S=save, Ctrl+Q=quit, Del=erase)");
    println(L"");
    println(L"  quit            - shutdown system (VERY IMPORTANT AND I FORGOT ABOUT THIS ONE)");
}

void cmd_ver(void) {
    set_color(EFI_CYAN, EFI_BGBLACK);
    println(L"TestOS 0.2 - fuck you, I'm reserved!");
    set_color(EFI_WHITE, EFI_BGBLACK);
}

void cmd_echo(CHAR16* input) {
    CHAR16* msg = input + 5;
    if (strlen16(msg) > 0) println(msg);
}

void cmd_color(CHAR16* input) {
    UINTN fg = EFI_WHITE, bg = EFI_BGBLACK;
    CHAR16* p = input + 5;
    if (*p == L' ') p++;
    if (*p >= L'0' && *p <= L'9') {
        fg = *p - L'0';
        p++;
        if (*p == L' ') p++;
        if (*p >= L'0' && *p <= L'9') {
            bg = *p - L'0';
        }
    }
    set_color(fg, bg);
    print(L"Color set to fg="); print_uint(fg); print(L" bg="); print_uint(bg); println(L"");
}

// Heh, I forgot about this one
void cmd_quit(CHAR16* input) {
    (void)input;
    hal_quit();
}
