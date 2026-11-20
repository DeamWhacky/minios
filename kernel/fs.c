/* fs.c — MiniFS, an inode file system on a RAM disk.
 *
 * Layers, bottom to top:
 *   blocks      balloc / bfree on a bitmap
 *   inodes      ialloc / ifree, bmap (file block -> disk block), read/write/truncate
 *   dirs        lookup / add / remove entries inside a directory inode
 *   paths       "a/b/c" -> inode, walking from root or the task's cwd
 *   public API  fs_* functions, serialised by one mutex
 */
#include "fs.h"
#include "memory.h"
#include "sync.h"
#include "task.h"
#include "drivers.h"
#include "console.h"

typedef struct {
    uint16_t type;                 /* 0 = free, FT_FILE, FT_DIR */
    uint16_t open_count;           /* open file descriptors referring to it */
    uint32_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;
    uint32_t ctime, mtime;
} inode_t;

static uint8_t  *disk;                             /* NUM_BLOCKS * BLOCK_SIZE bytes */
static uint32_t  block_bitmap[NUM_BLOCKS / 32];
static inode_t   inodes[MAX_INODES];
static mutex_t   fs_lock;

#define BLOCK(n) (disk + (uint32_t)(n) * BLOCK_SIZE)

/* ======================= blocks ======================= */
static uint32_t balloc(void) {
    for (uint32_t b = 1; b < NUM_BLOCKS; b++) {    /* block 0 means "none" */
        if (!(block_bitmap[b / 32] & (1u << (b % 32)))) {
            block_bitmap[b / 32] |= 1u << (b % 32);
            memset(BLOCK(b), 0, BLOCK_SIZE);
            return b;
        }
    }
    return 0;
}

static void bfree(uint32_t b) {
    if (b) block_bitmap[b / 32] &= ~(1u << (b % 32));
}

/* ======================= inodes ======================= */
static uint32_t ialloc(int type) {
    for (uint32_t i = 1; i < MAX_INODES; i++) {    /* inode 0 means "none" */
        if (!inodes[i].type) {
            memset(&inodes[i], 0, sizeof inodes[i]);
            inodes[i].type = type;
            inodes[i].ctime = inodes[i].mtime = timer_ticks();
            return i;
        }
    }
    return 0;
}

/* Translate block number 'fb' within a file to a disk block, optionally allocating */
static uint32_t bmap(inode_t *ip, uint32_t fb, bool alloc) {
    if (fb < NDIRECT) {
        if (!ip->direct[fb] && alloc) ip->direct[fb] = balloc();
        return ip->direct[fb];
    }
    fb -= NDIRECT;
    if (fb >= NINDIRECT) return 0;
    if (!ip->indirect) {
        if (!alloc) return 0;
        if (!(ip->indirect = balloc())) return 0;
    }
    uint32_t *table = (uint32_t *)BLOCK(ip->indirect);
    if (!table[fb] && alloc) table[fb] = balloc();
    return table[fb];
}

static int iread(uint32_t ino, uint32_t off, void *buf, uint32_t n) {
    inode_t *ip = &inodes[ino];
    if (off >= ip->size) return 0;
    if (n > ip->size - off) n = ip->size - off;
    uint32_t done = 0;
    while (done < n) {
        uint32_t b = bmap(ip, (off + done) / BLOCK_SIZE, false);
        uint32_t boff = (off + done) % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - boff;
        if (chunk > n - done) chunk = n - done;
        if (b) memcpy((uint8_t *)buf + done, BLOCK(b) + boff, chunk);
        else   memset((uint8_t *)buf + done, 0, chunk);      /* hole */
        done += chunk;
    }
    return done;
}

static int iwrite(uint32_t ino, uint32_t off, const void *buf, uint32_t n) {
    inode_t *ip = &inodes[ino];
    if (n && off >= MAX_FILE_SIZE) return -E_FBIG;
    if (n > MAX_FILE_SIZE - off) n = MAX_FILE_SIZE - off;
    uint32_t done = 0;
    while (done < n) {
        uint32_t b = bmap(ip, (off + done) / BLOCK_SIZE, true);
        if (!b) break;                                        /* disk full */
        uint32_t boff = (off + done) % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - boff;
        if (chunk > n - done) chunk = n - done;
        memcpy(BLOCK(b) + boff, (const uint8_t *)buf + done, chunk);
        done += chunk;
    }
    if (off + done > ip->size) ip->size = off + done;
    ip->mtime = timer_ticks();
    if (done == 0 && n > 0) return -E_NOSPC;
    return done;
}

static void itruncate(uint32_t ino) {
    inode_t *ip = &inodes[ino];
    for (int i = 0; i < NDIRECT; i++) { bfree(ip->direct[i]); ip->direct[i] = 0; }
    if (ip->indirect) {
        uint32_t *table = (uint32_t *)BLOCK(ip->indirect);
        for (int i = 0; i < NINDIRECT; i++) bfree(table[i]);
        bfree(ip->indirect);
        ip->indirect = 0;
    }
    ip->size = 0;
    ip->mtime = timer_ticks();
}

static uint32_t iblocks(inode_t *ip) {
    uint32_t n = 0;
    for (int i = 0; i < NDIRECT; i++) if (ip->direct[i]) n++;
    if (ip->indirect) {
        n++;
        uint32_t *table = (uint32_t *)BLOCK(ip->indirect);
        for (int i = 0; i < NINDIRECT; i++) if (table[i]) n++;
    }
    return n;
}

/* ======================= directories ======================= */
static uint32_t dir_lookup(uint32_t dir, const char *name) {
    dirent_t de;
    for (uint32_t off = 0; off < inodes[dir].size; off += sizeof de) {
        iread(dir, off, &de, sizeof de);
        if (de.ino && !strcmp(de.name, name)) return de.ino;
    }
    return 0;
}

static int dir_add(uint32_t dir, const char *name, uint32_t ino) {
    dirent_t de;
    uint32_t off;
    for (off = 0; off < inodes[dir].size; off += sizeof de) {   /* reuse a free slot */
        iread(dir, off, &de, sizeof de);
        if (!de.ino) break;
    }
    memset(&de, 0, sizeof de);
    strncpy(de.name, name, NAME_MAX);
    de.ino = ino;
    int r = iwrite(dir, off, &de, sizeof de);
    return r == (int)sizeof de ? 0 : -E_NOSPC;
}

static void dir_remove(uint32_t dir, const char *name) {
    dirent_t de;
    for (uint32_t off = 0; off < inodes[dir].size; off += sizeof de) {
        iread(dir, off, &de, sizeof de);
        if (de.ino && !strcmp(de.name, name)) {
            de.ino = 0;
            iwrite(dir, off, &de, sizeof de);
            return;
        }
    }
}

static bool dir_is_empty(uint32_t dir) {
    dirent_t de;
    for (uint32_t off = 0; off < inodes[dir].size; off += sizeof de) {
        iread(dir, off, &de, sizeof de);
        if (de.ino && strcmp(de.name, ".") && strcmp(de.name, "..")) return false;
    }
    return true;
}

/* ======================= path resolution ======================= */
/* Copy the next path element into 'name'. Returns the rest of the path,
 * NULL if there are no more elements. "a//bb/c" -> "a", then "bb", then "c". */
static const char *next_elem(const char *path, char *name, bool *too_long) {
    while (*path == '/') path++;
    if (!*path) return NULL;
    const char *start = path;
    while (*path && *path != '/') path++;
    uint32_t len = path - start;
    if (len > NAME_MAX) { *too_long = true; len = NAME_MAX; }
    memcpy(name, start, len);
    name[len] = 0;
    while (*path == '/') path++;
    return path;
}

/* Resolve a path. With want_parent, stop one level early and return the
 * parent directory, leaving the final component in 'last'. */
static int resolve(const char *path, bool want_parent, char *last) {
    if (!path || !*path) return -E_NOENT;
    uint32_t ino = (path[0] == '/') ? ROOT_INO : task_current()->cwd;
    char name[NAME_MAX + 1];
    bool too_long = false;

    while ((path = next_elem(path, name, &too_long)) != NULL) {
        if (too_long) return -E_NAMETOOLONG;
        if (inodes[ino].type != FT_DIR) return -E_NOTDIR;
        if (want_parent && *path == 0) {          /* 'name' is the last component */
            strncpy(last, name, NAME_MAX + 1);
            return ino;
        }
        uint32_t next = dir_lookup(ino, name);
        if (!next) return -E_NOENT;
        ino = next;
    }
    return want_parent ? -E_INVAL : (int)ino;     /* e.g. mkdir "/" */
}

static int create_locked(const char *path, int type) {
    char name[NAME_MAX + 1];
    int parent = resolve(path, true, name);
    if (parent < 0) return parent;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -E_EXIST;
    if (dir_lookup(parent, name)) return -E_EXIST;

    uint32_t ino = ialloc(type);
    if (!ino) return -E_NOSPC;
    if (type == FT_DIR) {
        if (dir_add(ino, ".", ino) < 0 || dir_add(ino, "..", parent) < 0) {
            itruncate(ino); inodes[ino].type = 0; return -E_NOSPC;
        }
    }
    if (dir_add(parent, name, ino) < 0) {
        itruncate(ino); inodes[ino].type = 0; return -E_NOSPC;
    }
    return ino;
}

/* ======================= public API ======================= */
void fs_init(void) {
    disk = kcalloc(NUM_BLOCKS * BLOCK_SIZE);
    if (!disk) panic("fs: cannot allocate RAM disk");
    memset(block_bitmap, 0, sizeof block_bitmap);
    memset(inodes, 0, sizeof inodes);
    mutex_init(&fs_lock);

    /* mkfs: root directory is inode 1, its parent is itself */
    uint32_t root = ialloc(FT_DIR);
    dir_add(root, ".", root);
    dir_add(root, "..", root);
}

/* Called when a task is killed. If it died inside a file system call it
 * still owns fs_lock; release it so other tasks do not hang forever. */
void fs_task_died(int pid) {
    uint32_t f = irq_save();
    if (fs_lock.owner_pid == pid) {
        fs_lock.owner_pid = -1;
        sem_signal(&fs_lock.sem);
    }
    irq_restore(f);
}

#define LOCKED(expr) ({ mutex_lock(&fs_lock); int _r = (expr); mutex_unlock(&fs_lock); _r; })

int fs_lookup(const char *path)            { return LOCKED(resolve(path, false, NULL)); }
int fs_create(const char *path, int type)  { return LOCKED(create_locked(path, type)); }

static int unlink_locked(const char *path) {
    char name[NAME_MAX + 1];
    int parent = resolve(path, true, name);
    if (parent < 0) return parent;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -E_INVAL;
    uint32_t ino = dir_lookup(parent, name);
    if (!ino) return -E_NOENT;
    if (ino == ROOT_INO) return -E_PERM;
    if (inodes[ino].open_count) return -E_BUSY;
    if (inodes[ino].type == FT_DIR) {
        if (!dir_is_empty(ino)) return -E_NOTEMPTY;
        task_t *tab = task_table();
        for (int i = 0; i < MAX_TASKS; i++)          /* someone's working directory? */
            if (tab[i].state != T_UNUSED && tab[i].cwd == ino) return -E_BUSY;
    }
    dir_remove(parent, name);
    itruncate(ino);
    inodes[ino].type = 0;
    return 0;
}
int fs_unlink(const char *path) { return LOCKED(unlink_locked(path)); }

static int rename_locked(const char *from, const char *to) {
    char oname[NAME_MAX + 1], nname[NAME_MAX + 1];
    int op = resolve(from, true, oname);
    if (op < 0) return op;
    int np = resolve(to, true, nname);
    if (np < 0) return np;
    uint32_t ino = dir_lookup(op, oname);
    if (!ino || !strcmp(oname, ".") || !strcmp(oname, "..")) return -E_NOENT;
    uint32_t target = dir_lookup(np, nname);
    if (target && inodes[target].type == FT_DIR) {       /* "mv file dir" -> into dir */
        np = target;
        strncpy(nname, oname, NAME_MAX + 1);
        if (dir_lookup(np, nname)) return -E_EXIST;
    } else if (target) return -E_EXIST;
    /* Moving directories between parents would need cycle checks: keep it simple */
    if (inodes[ino].type == FT_DIR && np != op) return -E_INVAL;
    int r = dir_add(np, nname, ino);
    if (r < 0) return r;
    dir_remove(op, oname);
    return 0;
}
int fs_rename(const char *from, const char *to) { return LOCKED(rename_locked(from, to)); }

static int stat_locked(uint32_t ino, stat_t *st) {
    if (!ino || ino >= MAX_INODES || !inodes[ino].type) return -E_NOENT;
    inode_t *ip = &inodes[ino];
    st->ino = ino; st->type = ip->type; st->size = ip->size;
    st->blocks = iblocks(ip); st->ctime = ip->ctime; st->mtime = ip->mtime;
    return 0;
}
int fs_stat_ino(uint32_t ino, stat_t *st) { return LOCKED(stat_locked(ino, st)); }

int fs_stat(const char *path, stat_t *st) {
    mutex_lock(&fs_lock);
    int ino = resolve(path, false, NULL);
    int r = ino < 0 ? ino : stat_locked(ino, st);
    mutex_unlock(&fs_lock);
    return r;
}

int fs_chdir(const char *path) {
    mutex_lock(&fs_lock);
    int ino = resolve(path, false, NULL);
    if (ino >= 0 && inodes[ino].type != FT_DIR) ino = -E_NOTDIR;
    if (ino >= 0) task_current()->cwd = ino;
    mutex_unlock(&fs_lock);
    return ino < 0 ? ino : 0;
}

/* Build the absolute path of the cwd by following ".." up to the root
 * and looking up each directory's name in its parent. */
int fs_getcwd(char *buf, uint32_t size) {
    mutex_lock(&fs_lock);
    char tmp[PATH_MAX];
    int pos = PATH_MAX - 1;
    tmp[pos] = 0;
    uint32_t ino = task_current()->cwd;
    while (ino != ROOT_INO) {
        uint32_t parent = dir_lookup(ino, "..");
        dirent_t de;
        bool found = false;
        for (uint32_t off = 0; off < inodes[parent].size; off += sizeof de) {
            iread(parent, off, &de, sizeof de);
            if (de.ino == ino && strcmp(de.name, ".") && strcmp(de.name, "..")) { found = true; break; }
        }
        if (!found) break;
        int len = strlen(de.name);
        if (pos - len - 1 < 0) break;
        pos -= len; memcpy(tmp + pos, de.name, len);
        tmp[--pos] = '/';
        ino = parent;
    }
    if (tmp[pos] == 0) tmp[--pos] = '/';
    mutex_unlock(&fs_lock);
    uint32_t len = strlen(tmp + pos);
    if (len + 1 > size) return -E_INVAL;
    memcpy(buf, tmp + pos, len + 1);
    return len;
}

int fs_read(uint32_t ino, uint32_t off, void *buf, uint32_t n)        { return LOCKED(iread(ino, off, buf, n)); }
int fs_write(uint32_t ino, uint32_t off, const void *buf, uint32_t n) { return LOCKED(iwrite(ino, off, buf, n)); }
int fs_truncate(uint32_t ino)                                         { return LOCKED((itruncate(ino), 0)); }

int fs_readdir(uint32_t dir, uint32_t *offset, dirent_t *out) {
    mutex_lock(&fs_lock);
    int r = 0;
    if (inodes[dir].type != FT_DIR) r = -E_NOTDIR;
    else while (*offset < inodes[dir].size) {
        iread(dir, *offset, out, sizeof *out);
        *offset += sizeof *out;
        if (out->ino) { r = 1; break; }              /* skip deleted slots */
    }
    mutex_unlock(&fs_lock);
    return r;
}

int fs_write_file(const char *path, const void *data, uint32_t len) {
    mutex_lock(&fs_lock);
    int ino = resolve(path, false, NULL);
    if (ino == -E_NOENT) ino = create_locked(path, FT_FILE);
    if (ino >= 0 && inodes[ino].type != FT_FILE) ino = -E_ISDIR;
    int r = ino;
    if (ino >= 0) { itruncate(ino); r = iwrite(ino, 0, data, len); }
    mutex_unlock(&fs_lock);
    return r;
}

void fs_open_ref(uint32_t ino, int delta) {
    uint32_t f = irq_save();
    inodes[ino].open_count += delta;
    irq_restore(f);
}

void fs_usage(uint32_t *ub, uint32_t *tb, uint32_t *ui, uint32_t *ti) {
    *ub = 0; *ui = 0;
    for (uint32_t b = 1; b < NUM_BLOCKS; b++) if (block_bitmap[b / 32] & (1u << (b % 32))) (*ub)++;
    for (uint32_t i = 1; i < MAX_INODES; i++) if (inodes[i].type) (*ui)++;
    *tb = NUM_BLOCKS - 1; *ti = MAX_INODES - 1;
}
