/* syscall.c — the int 0x80 system call interface.
 *
 * A program loads EAX with the call number and EBX/ECX/EDX with arguments,
 * executes "int $0x80", and finds the result in EAX. This is the ONLY way
 * a user-mode program can get anything done: it cannot touch hardware or
 * kernel memory itself. Every pointer it passes is checked before use. */
#include "syscall.h"
#include "cpu.h"
#include "task.h"
#include "fs.h"
#include "process.h"
#include "memory.h"
#include "console.h"
#include "drivers.h"

/* Copy a path argument into kernel memory (validating it on the way) */
#define GET_PATH(var, uptr)                                   \
    char var[PATH_MAX];                                       \
    do { int _e = copy_user_str(var, (const char *)(uptr), PATH_MAX); \
         if (_e < 0) return _e; } while (0)

static int sys_spawn_impl(const char *upath, char *const *uargv) {
    GET_PATH(path, upath);
    char *kargv[MAX_ARGS_EXEC];
    char *strings = kmalloc(MAX_ARGS_EXEC * 64);
    if (!strings) return -E_NOMEM;
    int argc = 0, r = 0;
    if (!uargv) {
        kargv[argc++] = path;
    } else {
        while (argc < MAX_ARGS_EXEC) {
            if (!user_ok(&uargv[argc], 4)) { r = -E_FAULT; break; }
            const char *ua = uargv[argc];
            if (!ua) break;
            char *dst = strings + argc * 64;
            int e = copy_user_str(dst, ua, 64);
            if (e == -E_FAULT) { r = e; break; }
            kargv[argc++] = dst;
        }
    }
    if (!r) r = process_spawn(path, argc, kargv, task_current()->priority);
    kfree(strings);
    return r;
}

static int dispatch(regs_t *r, uint32_t n, uint32_t a, uint32_t b, uint32_t c) {
    (void)r;
    switch (n) {
    case SYS_EXIT:   task_exit((int)a);
    case SYS_SLEEP:  task_sleep(a); return 0;
    case SYS_GETPID: return task_current()->pid;
    case SYS_TICKS:  return timer_ticks();

    case SYS_READ:
        if (!user_ok((void *)b, c)) return -E_FAULT;
        return fd_read((int)a, (void *)b, c);
    case SYS_WRITE:
        if (!user_ok((void *)b, c)) return -E_FAULT;
        return fd_write((int)a, (const void *)b, c);
    case SYS_OPEN: {
        GET_PATH(path, a);
        return fd_open(path, (int)b);
    }
    case SYS_CLOSE:  return fd_close((int)a);
    case SYS_LSEEK:  return fd_lseek((int)a, (int)b, (int)c);
    case SYS_UNLINK: { GET_PATH(path, a); return fs_unlink(path); }
    case SYS_MKDIR:  { GET_PATH(path, a); int ino = fs_create(path, FT_DIR); return ino < 0 ? ino : 0; }
    case SYS_CHDIR:  { GET_PATH(path, a); return fs_chdir(path); }
    case SYS_READDIR:
        if (!user_ok((void *)b, sizeof(dirent_t))) return -E_FAULT;
        return fd_readdir((int)a, (dirent_t *)b);
    case SYS_STAT: {
        GET_PATH(path, a);
        if (!user_ok((void *)b, sizeof(stat_t))) return -E_FAULT;
        return fs_stat(path, (stat_t *)b);
    }
    case SYS_GETCWD:
        if (!user_ok((void *)a, b)) return -E_FAULT;
        return fs_getcwd((char *)a, b);
    case SYS_SPAWN:  return sys_spawn_impl((const char *)a, (char *const *)b);
    case SYS_WAIT:   return task_wait((int)a);
    default:
        kprintf("[syscall] unknown call %u from pid %d\n", n, task_current()->pid);
        return -E_INVAL;
    }
}

static uint32_t syscall_handler(regs_t *r) {
    if (r->eax == SYS_YIELD) {
        /* int 0x80 is a trap gate (interrupts stay on), but the scheduler
         * must run with interrupts off. iret restores the caller's IF. */
        __asm__ volatile("cli");
        return schedule(r);
    }
    r->eax = (uint32_t)dispatch(r, r->eax, r->ebx, r->ecx, r->edx);
    return 0;
}

void syscall_init(void) {
    register_handler(0x80, syscall_handler);
}
