/* libc.h — the tiny C library for MiniOS user programs.
 * Everything that touches the outside world goes through a system call. */
#ifndef LIBC_H
#define LIBC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "abi.h"

/* system calls */
void     exit(int code) __attribute__((noreturn));
void     yield(void);
void     sleep_ms(uint32_t ms);
int      getpid(void);
uint32_t ticks(void);                  /* 1 tick = 10 ms */
int      read(int fd, void *buf, uint32_t n);
int      write(int fd, const void *buf, uint32_t n);
int      open(const char *path, int flags);
int      close(int fd);
int      lseek(int fd, int off, int whence);
int      unlink(const char *path);
int      mkdir(const char *path);
int      readdir(int fd, dirent_t *out);
int      stat(const char *path, stat_t *st);
int      chdir(const char *path);
int      getcwd(char *buf, uint32_t size);
int      spawn(const char *path, char **argv);
int      wait(int pid);

/* strings & memory */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *d, const char *s);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    atoi(const char *s);

/* formatted output: %d %u %x %s %c %p %%, widths like %5d %-10s %08x */
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...);
int  printf(const char *fmt, ...);
int  dprintf(int fd, const char *fmt, ...);
int  puts(const char *s);
int  getline(char *buf, int max);       /* read a line from stdin, without '\n' */

#endif
