#ifndef SYSCALL_H
#define SYSCALL_H

#include "common.h"
#include "abi.h"          /* system call numbers, flags, structs (shared with user/) */

void syscall_init(void);

/* Kernel-side wrappers, so kernel threads can exercise the same interface
 * that user programs use (see user/libc.c for the user-space version). */
static inline int syscall3(int n, uint32_t a, uint32_t b, uint32_t c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}
static inline int  sys_write(int fd, const void *b, uint32_t n) { return syscall3(SYS_WRITE, fd, (uint32_t)b, n); }
static inline void sys_sleep(uint32_t ms)  { syscall3(SYS_SLEEP, ms, 0, 0); }
static inline int  sys_getpid(void)        { return syscall3(SYS_GETPID, 0, 0, 0); }
static inline uint32_t sys_ticks(void)     { return (uint32_t)syscall3(SYS_TICKS, 0, 0, 0); }
static inline int  sys_spawn(const char *path, char **argv) { return syscall3(SYS_SPAWN, (uint32_t)path, (uint32_t)argv, 0); }
static inline int  sys_wait(int pid)       { return syscall3(SYS_WAIT, pid, 0, 0); }
static inline void sys_exit(int code)      { syscall3(SYS_EXIT, code, 0, 0); for (;;); }

#endif
