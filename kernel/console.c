/* console.c — VGA text mode (80x25) + COM1 serial, and kprintf.
 * Everything printed goes to both, so the OS can be driven from a
 * terminal with "make run-nographic" as well as from the QEMU window. */
#include "console.h"
#include <stdarg.h>

#define VGA_W 80
#define VGA_H 25
#define COM1  0x3F8

static volatile uint16_t *const vga = (uint16_t *)0xB8000;
static int cur_x, cur_y;
static uint8_t color = (C_BLACK << 4) | C_LGREY;

/* ---------------- Serial (COM1, 38400 8N1) ---------------- */
void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* disable UART interrupts (we poll) */
    outb(COM1 + 3, 0x80);    /* DLAB on */
    outb(COM1 + 0, 0x03);    /* divisor 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);    /* FIFO */
    outb(COM1 + 4, 0x0B);
}
void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) ;
    outb(COM1, (uint8_t)c);
}
bool serial_received(void) { return inb(COM1 + 5) & 1; }
char serial_getc(void)     { return (char)inb(COM1); }

/* ---------------- VGA ---------------- */
static void vga_update_cursor(void) {
    uint16_t pos = cur_y * VGA_W + cur_x;
    outb(0x3D4, 14); outb(0x3D5, pos >> 8);
    outb(0x3D4, 15); outb(0x3D5, pos & 0xFF);
}

static void vga_scroll(void) {
    if (cur_y < VGA_H) return;
    for (int i = 0; i < (VGA_H - 1) * VGA_W; i++) vga[i] = vga[i + VGA_W];
    for (int i = (VGA_H - 1) * VGA_W; i < VGA_H * VGA_W; i++) vga[i] = (color << 8) | ' ';
    cur_y = VGA_H - 1;
}

static void vga_putc(char c) {
    if (c == '\n')      { cur_x = 0; cur_y++; }
    else if (c == '\r') { cur_x = 0; }
    else if (c == '\t') { cur_x = (cur_x + 8) & ~7; }
    else if (c == '\b') { if (cur_x > 0) cur_x--; }
    else {
        vga[cur_y * VGA_W + cur_x] = (color << 8) | (uint8_t)c;
        if (++cur_x >= VGA_W) { cur_x = 0; cur_y++; }
    }
    if (cur_x >= VGA_W) { cur_x = 0; cur_y++; }
    vga_scroll();
}

void console_set_color(uint8_t fg, uint8_t bg) { color = (bg << 4) | (fg & 0x0F); }

void console_clear(void) {
    uint32_t f = irq_save();
    for (int i = 0; i < VGA_W * VGA_H; i++) vga[i] = (color << 8) | ' ';
    cur_x = cur_y = 0;
    vga_update_cursor();
    serial_putc('\033'); serial_putc('['); serial_putc('2'); serial_putc('J');
    serial_putc('\033'); serial_putc('['); serial_putc('H');
    irq_restore(f);
}

void console_init(void) {
    serial_init();
    console_clear();
}

void console_putc(char c) {
    uint32_t f = irq_save();
    vga_putc(c);
    serial_putc(c);
    vga_update_cursor();
    irq_restore(f);
}

void console_backspace(void) {
    uint32_t f = irq_save();
    vga_putc('\b'); vga_putc(' '); vga_putc('\b');
    serial_putc('\b'); serial_putc(' '); serial_putc('\b');
    vga_update_cursor();
    irq_restore(f);
}

void console_write(const char *s) {
    uint32_t f = irq_save();          /* whole string is atomic w.r.t. other tasks */
    while (*s) { vga_putc(*s); serial_putc(*s); s++; }
    vga_update_cursor();
    irq_restore(f);
}

void console_write_n(const char *s, uint32_t n) {
    uint32_t f = irq_save();
    for (uint32_t i = 0; i < n; i++) { vga_putc(s[i]); serial_putc(s[i]); }
    vga_update_cursor();
    irq_restore(f);
}

/* ---------------- kprintf ---------------- */
typedef struct { char *buf; int pos, cap; } out_t;

static void emit(out_t *o, char c) { if (o->pos < o->cap - 1) o->buf[o->pos++] = c; }

static void emit_num(out_t *o, uint32_t v, int base, bool neg, int width, char pad, bool left) {
    char tmp[12]; int n = 0;
    const char *digits = "0123456789abcdef";
    do { tmp[n++] = digits[v % base]; v /= base; } while (v);
    if (neg) tmp[n++] = '-';
    int padn = width > n ? width - n : 0;
    if (!left) while (padn--) emit(o, pad);
    while (n) emit(o, tmp[--n]);
    if (left) while (padn-- > 0) emit(o, ' ');
}

static void vformat(out_t *o, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(o, *fmt); continue; }
        fmt++;
        bool left = false; char pad = ' '; int width = 0;
        if (*fmt == '-') { left = true; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'd': { int v = va_arg(ap, int);
                    emit_num(o, v < 0 ? -(uint32_t)v : (uint32_t)v, 10, v < 0, width, pad, left); break; }
        case 'u': emit_num(o, va_arg(ap, uint32_t), 10, false, width, pad, left); break;
        case 'x': emit_num(o, va_arg(ap, uint32_t), 16, false, width, pad, left); break;
        case 'p': emit(o, '0'); emit(o, 'x');
                  emit_num(o, va_arg(ap, uint32_t), 16, false, 8, '0', false); break;
        case 'c': emit(o, (char)va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
                    int len = (int)strlen(s), padn = width > len ? width - len : 0;
                    if (!left) while (padn--) emit(o, ' ');
                    while (*s) emit(o, *s++);
                    if (left) while (padn-- > 0) emit(o, ' ');
                    break; }
        case '%': emit(o, '%'); break;
        default:  emit(o, '%'); emit(o, *fmt); break;
        }
    }
    o->buf[o->pos] = 0;
}

void kprintf(const char *fmt, ...) {
    char buf[512];
    out_t o = { buf, 0, sizeof buf };
    va_list ap;
    va_start(ap, fmt);
    vformat(&o, fmt, ap);
    va_end(ap);
    console_write(buf);
}

void panic(const char *msg) {
    __asm__ volatile("cli");
    console_set_color(C_WHITE, C_RED);
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    for (;;) cpu_halt();
}
