/* abi.h — the contract between the kernel and user programs.
 * Shared by both sides: system call numbers, flags, structs, error codes. */
#ifndef ABI_H
#define ABI_H

#include <stdint.h>

/* System calls: EAX = number, EBX/ECX/EDX = arguments, result in EAX.
 * Negative results are error codes (E_*). */
enum {
    SYS_EXIT    = 0,   /* (int code)                               */
    SYS_YIELD   = 1,   /* ()                                       */
    SYS_SLEEP   = 2,   /* (uint32_t ms)                            */
    SYS_GETPID  = 3,   /* () -> pid                                */
    SYS_TICKS   = 4,   /* () -> timer ticks since boot (10 ms)     */
    SYS_READ    = 5,   /* (int fd, void *buf, uint32_t n) -> bytes */
    SYS_WRITE   = 6,   /* (int fd, const void *buf, uint32_t n)    */
    SYS_OPEN    = 7,   /* (const char *path, int flags) -> fd      */
    SYS_CLOSE   = 8,   /* (int fd)                                 */
    SYS_LSEEK   = 9,   /* (int fd, int offset, int whence) -> pos  */
    SYS_UNLINK  = 10,  /* (const char *path)  file or empty dir    */
    SYS_MKDIR   = 11,  /* (const char *path)                       */
    SYS_READDIR = 12,  /* (int fd, dirent_t *out) -> 1 entry, 0 end */
    SYS_STAT    = 13,  /* (const char *path, stat_t *out)          */
    SYS_CHDIR   = 14,  /* (const char *path)                       */
    SYS_GETCWD  = 15,  /* (char *buf, uint32_t size)               */
    SYS_SPAWN   = 16,  /* (const char *path, char **argv) -> pid   */
    SYS_WAIT    = 17,  /* (int pid) -> exit code                   */
    SYS_LINK    = 18,  /* (const char *existing, const char *new)  */
    SYS_COUNT
};

/* open() flags */
#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR    0x2
#define O_ACCMODE 0x3
#define O_CREAT   0x10
#define O_TRUNC   0x20
#define O_APPEND  0x40

/* lseek() whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define STDIN  0
#define STDOUT 1
#define STDERR 2

#define NAME_MAX 27
#define PATH_MAX 128

#define FT_FILE 1
#define FT_DIR  2

typedef struct {
    char     name[NAME_MAX + 1];
    uint32_t ino;
} dirent_t;                       /* exactly 32 bytes: also the on-disk directory entry */

typedef struct {
    uint32_t ino;
    uint32_t type;                /* FT_FILE / FT_DIR */
    uint32_t size;                /* bytes */
    uint32_t blocks;              /* data blocks in use */
    uint32_t ctime, mtime;        /* ticks since boot */
    uint32_t nlink;               /* number of names (hard links) */
} stat_t;

/* Error codes (returned negated) */
#define E_PERM        1
#define E_NOENT       2
#define E_EXIST       3
#define E_NOTDIR      4
#define E_ISDIR       5
#define E_NOSPC       6
#define E_BADF        7
#define E_FAULT       8
#define E_INVAL       9
#define E_NOTEMPTY   10
#define E_BUSY       11
#define E_NOMEM      12
#define E_NAMETOOLONG 13
#define E_MFILE      14
#define E_NOEXEC     15
#define E_FBIG       16

static inline const char *strerror(int e) {
    if (e < 0) e = -e;
    switch (e) {
    case 0:            return "success";
    case E_PERM:       return "operation not permitted";
    case E_NOENT:      return "no such file or directory";
    case E_EXIST:      return "file exists";
    case E_NOTDIR:     return "not a directory";
    case E_ISDIR:      return "is a directory";
    case E_NOSPC:      return "no space left on device";
    case E_BADF:       return "bad file descriptor";
    case E_FAULT:      return "bad address";
    case E_INVAL:      return "invalid argument";
    case E_NOTEMPTY:   return "directory not empty";
    case E_BUSY:       return "file is busy (open)";
    case E_NOMEM:      return "out of memory";
    case E_NAMETOOLONG:return "name too long";
    case E_MFILE:      return "too many open files";
    case E_NOEXEC:     return "not an executable";
    case E_FBIG:       return "file too large";
    default:           return "unknown error";
    }
}

/* User address space layout (identical in every process) */
#define USER_BASE       0x40000000      /* programs are linked here         */
#define USER_STACK_TOP  0x50000000
#define USER_STACK_SIZE (16 * 1024)
#define USER_TOP        0x50000000

#endif
