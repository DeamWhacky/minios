#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Port I/O ---- */
static inline void outb(uint16_t port, uint8_t v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

/* ---- Interrupt control ----
 * irq_save() disables interrupts and returns the previous EFLAGS so that
 * critical sections nest correctly. This is our "big kernel lock" on a
 * single-CPU machine: nothing can preempt us while IF=0. */
static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    if (f & (1 << 9)) __asm__ volatile("sti" ::: "memory");
}
static inline void cpu_halt(void) { __asm__ volatile("hlt"); }

/* ---- String helpers (kernel/string.c) ---- */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *d, const char *s, size_t n);
int    atoi(const char *s);
char  *strchr(const char *s, int c);

/* ---- Kernel panic ---- */
void panic(const char *msg) __attribute__((noreturn));

#endif
