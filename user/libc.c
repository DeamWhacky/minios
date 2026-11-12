/* libc.c — MiniOS user-space C library. */
#include "libc.h"

/* ---------------- system call stubs ----------------
 * "int $0x80" switches the CPU to ring 0 and into the kernel's handler.
 * Nothing else in a user program can enter the kernel. */
static inline int sc(int n, uint32_t a, uint32_t b, uint32_t c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

void exit(int code)                         { sc(SYS_EXIT, code, 0, 0); for (;;) ; }
void yield(void)                            { sc(SYS_YIELD, 0, 0, 0); }
void sleep_ms(uint32_t ms)                  { sc(SYS_SLEEP, ms, 0, 0); }
int  getpid(void)                           { return sc(SYS_GETPID, 0, 0, 0); }
uint32_t ticks(void)                        { return sc(SYS_TICKS, 0, 0, 0); }
int  read(int fd, void *b, uint32_t n)      { return sc(SYS_READ, fd, (uint32_t)b, n); }
int  write(int fd, const void *b, uint32_t n){ return sc(SYS_WRITE, fd, (uint32_t)b, n); }
int  open(const char *p, int flags)         { return sc(SYS_OPEN, (uint32_t)p, flags, 0); }
int  close(int fd)                          { return sc(SYS_CLOSE, fd, 0, 0); }
int  lseek(int fd, int off, int whence)     { return sc(SYS_LSEEK, fd, off, whence); }
int  unlink(const char *p)                  { return sc(SYS_UNLINK, (uint32_t)p, 0, 0); }
int  mkdir(const char *p)                   { return sc(SYS_MKDIR, (uint32_t)p, 0, 0); }
int  readdir(int fd, dirent_t *out)         { return sc(SYS_READDIR, fd, (uint32_t)out, 0); }
int  stat(const char *p, stat_t *st)        { return sc(SYS_STAT, (uint32_t)p, (uint32_t)st, 0); }
int  chdir(const char *p)                   { return sc(SYS_CHDIR, (uint32_t)p, 0, 0); }
int  getcwd(char *b, uint32_t n)            { return sc(SYS_GETCWD, (uint32_t)b, n, 0); }
int  spawn(const char *p, char **argv)      { return sc(SYS_SPAWN, (uint32_t)p, (uint32_t)argv, 0); }
int  wait(int pid)                          { return sc(SYS_WAIT, pid, 0, 0); }

/* ---------------- strings ---------------- */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
int atoi(const char *s) {
    int sign = 1, v = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}

/* ---------------- formatting ---------------- */
typedef struct { char *buf; size_t pos, cap; } out_t;
static void put(out_t *o, char c) { if (o->pos + 1 < o->cap) o->buf[o->pos] = c; o->pos++; }

static void num(out_t *o, uint32_t v, int base, bool neg, int width, char pad, bool left) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    if (neg) tmp[n++] = '-';
    int padn = width > n ? width - n : 0;
    if (!left) while (padn-- > 0) put(o, pad);
    while (n) put(o, tmp[--n]);
    if (left) while (padn-- > 0) put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    out_t o = { buf, 0, size };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        bool left = false; char pad = ' '; int width = 0;
        if (*fmt == '-') { left = true; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'd': { int v = va_arg(ap, int); num(&o, v < 0 ? -(uint32_t)v : (uint32_t)v, 10, v < 0, width, pad, left); break; }
        case 'u': num(&o, va_arg(ap, uint32_t), 10, false, width, pad, left); break;
        case 'x': num(&o, va_arg(ap, uint32_t), 16, false, width, pad, left); break;
        case 'p': put(&o, '0'); put(&o, 'x'); num(&o, va_arg(ap, uint32_t), 16, false, 8, '0', false); break;
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
                    int len = strlen(s), padn = width > len ? width - len : 0;
                    if (!left) while (padn-- > 0) put(&o, ' ');
                    while (*s) put(&o, *s++);
                    if (left) while (padn-- > 0) put(&o, ' ');
                    break; }
        case '%': put(&o, '%'); break;
        default:  put(&o, '%'); put(&o, *fmt);
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static int vdprintf(int fd, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    return write(fd, buf, n);
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vdprintf(STDOUT, fmt, ap);
    va_end(ap);
    return n;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s) { int n = write(STDOUT, s, strlen(s)); write(STDOUT, "\n", 1); return n + 1; }

int getline(char *buf, int max) {
    int n = read(STDIN, buf, max - 1);
    if (n < 0) n = 0;
    if (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = 0;
    return n;
}
