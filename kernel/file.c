/* file.c — file descriptors.
 *
 *   task->fds[fd]  -->  file_t (global open-file table)  -->  inode / console
 *
 * Two descriptors can share one file_t (and so one offset); a file_t is
 * freed when its reference count drops to zero. fds 0/1/2 of every user
 * process point at the console. */
#include "fs.h"
#include "task.h"
#include "console.h"
#include "drivers.h"

static file_t open_files[MAX_OPEN_FILES];

static file_t *file_alloc(void) {
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].refs) {
            memset(&open_files[i], 0, sizeof open_files[i]);
            open_files[i].refs = 1;
            irq_restore(f);
            return &open_files[i];
        }
    }
    irq_restore(f);
    return NULL;
}

static void file_put(file_t *fp) {
    uint32_t f = irq_save();
    if (--fp->refs == 0) {
        if (fp->kind == FILE_INODE) fs_open_ref(fp->ino, -1);
        fp->kind = FILE_NONE;
    }
    irq_restore(f);
}

static file_t *console_file;

void fd_init_std(task_t *t) {
    if (!console_file) {
        console_file = file_alloc();
        console_file->kind = FILE_CONSOLE;
        console_file->flags = O_RDWR;
        console_file->refs = 1;          /* permanent reference held by the kernel */
    }
    uint32_t f = irq_save();
    for (int fd = 0; fd < 3; fd++) { t->fds[fd] = console_file; console_file->refs++; }
    irq_restore(f);
}

void fd_close_all(task_t *t) {
    for (int fd = 0; fd < MAX_FDS; fd++)
        if (t->fds[fd]) { file_put(t->fds[fd]); t->fds[fd] = NULL; }
}

static file_t *get(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return task_current()->fds[fd];
}

int fd_open(const char *path, int flags) {
    task_t *t = task_current();
    int fd;
    for (fd = 0; fd < MAX_FDS && t->fds[fd]; fd++) ;
    if (fd == MAX_FDS) return -E_MFILE;

    int ino = fs_lookup(path);
    if (ino == -E_NOENT && (flags & O_CREAT)) ino = fs_create(path, FT_FILE);
    if (ino < 0) return ino;

    stat_t st;
    fs_stat_ino(ino, &st);
    if (st.type == FT_DIR && (flags & O_ACCMODE) != O_RDONLY) return -E_ISDIR;

    file_t *fp = file_alloc();
    if (!fp) return -E_MFILE;

    if ((flags & O_TRUNC) && st.type == FT_FILE && (flags & O_ACCMODE) != O_RDONLY)
        fs_truncate(ino);

    fp->kind = FILE_INODE;
    fp->ino = ino;
    fp->flags = flags;
    fp->offset = 0;
    fs_open_ref(ino, +1);
    t->fds[fd] = fp;
    return fd;
}

int fd_close(int fd) {
    file_t *fp = get(fd);
    if (!fp) return -E_BADF;
    task_current()->fds[fd] = NULL;
    file_put(fp);
    return 0;
}

/* Console input is line-buffered with echo, like a terminal in "cooked" mode */
static int console_read(char *buf, uint32_t n) {
    uint32_t len = 0;
    while (len < n) {
        char c = input_getchar();
        if (c == '\b') { if (len) { len--; console_backspace(); } continue; }
        if (c == '\n') { console_putc('\n'); buf[len++] = '\n'; break; }
        if (c == 4) break;                                  /* Ctrl+D = end of input */
        if (c >= 32 && c < 127) { console_putc(c); buf[len++] = c; }
    }
    return len;
}

int fd_read(int fd, void *buf, uint32_t n) {
    file_t *fp = get(fd);
    if (!fp) return -E_BADF;
    if ((fp->flags & O_ACCMODE) == O_WRONLY) return -E_BADF;
    if (fp->kind == FILE_CONSOLE) return console_read(buf, n);

    stat_t st;
    fs_stat_ino(fp->ino, &st);
    if (st.type == FT_DIR) return -E_ISDIR;
    int r = fs_read(fp->ino, fp->offset, buf, n);
    if (r > 0) fp->offset += r;
    return r;
}

int fd_write(int fd, const void *buf, uint32_t n) {
    file_t *fp = get(fd);
    if (!fp) return -E_BADF;
    if ((fp->flags & O_ACCMODE) == O_RDONLY) return -E_BADF;
    if (fp->kind == FILE_CONSOLE) {
        console_write_n(buf, n);
        return n;
    }
    if (fp->flags & O_APPEND) {
        stat_t st; fs_stat_ino(fp->ino, &st);
        fp->offset = st.size;
    }
    int r = fs_write(fp->ino, fp->offset, buf, n);
    if (r > 0) fp->offset += r;
    return r;
}

int fd_lseek(int fd, int off, int whence) {
    file_t *fp = get(fd);
    if (!fp) return -E_BADF;
    if (fp->kind != FILE_INODE) return -E_INVAL;
    stat_t st; fs_stat_ino(fp->ino, &st);
    int base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int)fp->offset
             : whence == SEEK_END ? (int)st.size : -1;
    if (base < 0 || base + off < 0) return -E_INVAL;
    fp->offset = base + off;
    return fp->offset;
}

int fd_readdir(int fd, dirent_t *out) {
    file_t *fp = get(fd);
    if (!fp) return -E_BADF;
    if (fp->kind != FILE_INODE) return -E_NOTDIR;
    return fs_readdir(fp->ino, &fp->offset, out);
}
