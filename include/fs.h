#ifndef FS_H
#define FS_H

#include "common.h"
#include "abi.h"

/* ---------------------------------------------------------------
 * MiniFS: an inode-based file system on a RAM disk.
 *
 *   disk:   NUM_BLOCKS blocks of BLOCK_SIZE bytes + a free-block bitmap
 *   inode:  type, size, link count, 12 direct block pointers,
 *           1 indirect block (128 more block numbers) and 1 double-
 *           indirect block (128 x 128 more), like ext2
 *   dirs:   files whose data is an array of 32-byte dirent_t entries;
 *           every directory contains "." and ".."
 * --------------------------------------------------------------- */
#define BLOCK_SIZE   512
#define NUM_BLOCKS   8192                       /* 4 MiB disk */
#define MAX_INODES   256
#define NDIRECT      12
#define NINDIRECT    (BLOCK_SIZE / 4)
#define NDINDIRECT   (NINDIRECT * NINDIRECT)
#define MAX_FILE_SIZE ((NDIRECT + NINDIRECT + NDINDIRECT) * BLOCK_SIZE)   /* ~8 MiB */
#define ROOT_INO     1

void fs_init(void);

/* Path-based operations. Relative paths start at the calling task's cwd.
 * All return >= 0 on success or a negative E_* error code. */
int  fs_lookup(const char *path);                       /* -> inode number */
int  fs_create(const char *path, int type);             /* -> new inode    */
int  fs_unlink(const char *path);
int  fs_rename(const char *from, const char *to);
int  fs_link(const char *existing, const char *newpath);  /* hard link */
int  fs_open(const char *path, int flags);              /* -> inode, counted as open */
int  fs_stat(const char *path, stat_t *st);
int  fs_chdir(const char *path);
int  fs_getcwd(char *buf, uint32_t size);
int  fs_write_file(const char *path, const void *data, uint32_t len);  /* create/overwrite */

/* Inode-level operations (used by the file-descriptor layer) */
int  fs_read(uint32_t ino, uint32_t off, void *buf, uint32_t n);
int  fs_write(uint32_t ino, uint32_t off, const void *buf, uint32_t n);
int  fs_truncate(uint32_t ino);
int  fs_stat_ino(uint32_t ino, stat_t *st);
int  fs_readdir(uint32_t dir_ino, uint32_t *offset, dirent_t *out);    /* 1 = entry, 0 = end */
void fs_open_ref(uint32_t ino, int delta);              /* open-file counting */
void fs_usage(uint32_t *used_blocks, uint32_t *total_blocks,
              uint32_t *used_inodes, uint32_t *total_inodes);

/* ---------------------------------------------------------------
 * File descriptors: per-task table of pointers into a global
 * open-file table (so offsets can be shared, as in UNIX).
 * --------------------------------------------------------------- */
#define MAX_FDS        16
#define MAX_OPEN_FILES 64

typedef enum { FILE_NONE = 0, FILE_CONSOLE, FILE_INODE } file_kind_t;

typedef struct file {
    file_kind_t kind;
    uint32_t    ino;
    uint32_t    offset;
    int         flags;
    int         refs;
} file_t;

struct task;
void fd_init_std(struct task *t);                       /* fds 0,1,2 -> console */
void fd_close_all(struct task *t);
int  fd_open(const char *path, int flags);
int  fd_close(int fd);
int  fd_read(int fd, void *buf, uint32_t n);
int  fd_write(int fd, const void *buf, uint32_t n);
int  fd_lseek(int fd, int off, int whence);
int  fd_readdir(int fd, dirent_t *out);

#endif
