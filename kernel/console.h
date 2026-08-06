#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

#include "../include/bootinfo.h"

/* The 16-colour DOS palette, in the traditional index order. Colours 8-15 are
   the bright half of 0-7. */
#define COLOR_BLACK 0
#define COLOR_BLUE 1
#define COLOR_GREEN 2
#define COLOR_CYAN 3
#define COLOR_RED 4
#define COLOR_MAGENTA 5
#define COLOR_BROWN 6
#define COLOR_LIGHT_GRAY 7
#define COLOR_DARK_GRAY 8
#define COLOR_LIGHT_BLUE 9
#define COLOR_LIGHT_GREEN 10
#define COLOR_LIGHT_CYAN 11
#define COLOR_LIGHT_RED 12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW 14
#define COLOR_WHITE 15

void console_init(const BOOT_INFO* info);

/* The colours the shell paints itself in.
 *
 * These used to be constants scattered through command.c, which made them
 * unchangeable without a rebuild. Holding them in one place is what lets a
 * program - and the configuration file it writes - change how the system
 * looks. The defaults are the DOS ones and stay that way unless something
 * overrides them. */
typedef struct {
    boot_uint8_t foreground;
    boot_uint8_t background;
    boot_uint8_t prompt;      /* the drive and path before the > */
    boot_uint8_t error;       /* "Bad command or file name" and friends */
} CONSOLE_THEME;

void console_set_theme(const CONSOLE_THEME* theme);
const CONSOLE_THEME* console_theme(void);

/* Set the current drawing colour back to the theme's normal text. */
void console_use_theme(void);

/* Erase the screen with the current background colour and home the cursor. */
void console_clear(void);

void console_set_color(boot_uint8_t foreground, boot_uint8_t background);
void console_putchar(char character);
void console_write(const char* text);
void console_write_dec(boot_uint64_t value);

/* Fixed 16 digits, no 0x prefix - register dumps line up in columns. */
void console_write_hex(boot_uint64_t value);

/* Cursor position in character cells. */
void console_set_cursor(boot_uint32_t column, boot_uint32_t row);
boot_uint32_t console_column(void);
boot_uint32_t console_row(void);
/* The framebuffer's size in pixels, and the console's in characters. */
boot_uint32_t console_width(void);
boot_uint32_t console_height(void);
boot_uint32_t console_columns(void);
boot_uint32_t console_rows(void);

/* Draw or erase the cursor block. The keyboard read loop calls this so the
   caret is only visible while input is actually being waited on. */
void console_show_cursor(int visible);

#endif
