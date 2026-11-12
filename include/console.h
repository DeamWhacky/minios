#ifndef CONSOLE_H
#define CONSOLE_H

#include "common.h"

/* VGA colours */
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGREY,
    C_DGREY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};

void serial_init(void);
void serial_putc(char c);
bool serial_received(void);
char serial_getc(void);

void console_init(void);
void console_putc(char c);              /* VGA + serial */
void console_write(const char *s);
void console_write_n(const char *s, uint32_t n);
void console_clear(void);
void console_set_color(uint8_t fg, uint8_t bg);
void console_backspace(void);

void kprintf(const char *fmt, ...);     /* %d %u %x %s %c %p %%, width e.g. %5d %-8s %08x */

#endif
