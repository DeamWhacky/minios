/* fs.c — MiniFS, an inode file system on a RAM disk.
 *
 * Layers, bottom to top:
 *   blocks      balloc / bfree on a bitmap (with a next-fit hint and a free count)
 *   inodes      ialloc, bmap (file block -> disk block), read/write/truncate
 *   dirs        lookup / add / remove / update entries inside a directory inode
 *   paths       "a/b/c" -> inode, walking from root or the task's cwd
 *   public API  fs_* functions, serialised by one mutex
 *
 * A file's blocks are found through 12 direct pointers, one indirect block
 * (128 more pointers) and one double-indirect block (128 indirect blocks of
 * 128 pointers each), like ext2. Files can have several names (hard links):
 * the inode counts them in nlink and is freed when the last name goes.
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
    uint16_t nlink;                /* directory entries (names) pointing at it */
    uint16_t _pad;
    uint32_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;             /* block of NINDIRECT block numbers */
    uint32_t dindirect;            /* block of NINDIRECT indirect blocks */
    uint32_t ctime, mtime;
} inode_t;

static uint8_t  *disk;                             /* NUM_BLOCKS * BLOCK_SIZE bytes */
static uint32_t  block_bitmap[NUM_BLOCKS / 32];
static uint32_t  free_blocks;                      /* kept up to date: df is O(1) */
static uint32_t  alloc_hint = 1;                   /* where the next search starts */
static inode_t   inodes[MAX_INODES];
static mutex_t   fs_lock;

#define BLOCK(n) (disk + (uint32_t)(n) * BLOCK_SIZE)
#define BIT_USED(b) (block_bitmap[(b) / 32] & (1u << ((b) % 32)))

/* ======================= blocks ======================= */
/* Next fit: start where the last allocation stopped instead of at block 1,
 * so a nearly full disk isn't rescanned from the start every time. */
static uint32_t balloc(void) {
    for (uint32_t i = 0; i < NUM_BLOCKS - 1; i++) {   /* block 0 means "none" */
        uint32_t b = 1 + (alloc_hint - 1 + i) % (NUM_BLOCKS - 1);
        if (!BIT_USED(b)) {
            block_bitmap[b / 32] |= 1u << (b % 32);
            free_blocks--;
            alloc_hint = b + 1 < NUM_BLOCKS ? b + 1 : 1;
            memset(BLOCK(b), 0, BLOCK_SIZE);
            return b;
        }
    }
    return 0;
}

static void bfree(uint32_t b) {
    if (!b || b >= NUM_BLOCKS || !BIT_USED(b)) return;
    block_bitmap[b / 32] &= ~(1u << (b % 32));
    free_blocks++;
    if (b < alloc_hint) alloc_hint = b;
}

/* ======================= inodes ======================= */
static uint32_t ialloc(int type) {
    for (uint32_t i = 1; i < MAX_INODES; i++) {    /* inode 0 means "none" */
        if (!inodes[i].type) {
            memset(&inodes[i], 0, sizeof inodes[i]);
            inodes[i].type = type;
            inodes[i].nlink = 1;
            inodes[i].ctime = inodes[i].mtime = timer_ticks();
            return i;
        }
    }
    return 0;
}

/* Entry 'idx' of the pointer block '*tbl', allocating the pointer block
 * and/or the entry if asked to. Returns 0 if absent (or the disk is full). */
static uint32_t table_entry(uint32_t *tbl, uint32_t idx, bool alloc) {
    if (!*tbl) {
        if (!alloc || !(*tbl = balloc())) return 0;
    }
    uint32_t *t = (uint32_t *)BLOCK(*tbl);
    if (!t[idx] && alloc) t[idx] = balloc();
    return t[idx];
}

/* Translate block number 'fb' within a file to a disk block, optionally allocating */
static uint32_t bmap(inode_t *ip, uint32_t fb, bool alloc) {
    if (fb < NDIRECT) {
        if (!ip->direct[fb] && alloc) ip->direct[fb] = balloc();
        return ip->direct[fb];
    }
    fb -= NDIRECT;
    if (fb < NINDIRECT) return table_entry(&ip->indirect, fb, alloc);
    fb -= NINDIRECT;
    if (fb >= NINDIRECT * NINDIRECT) return 0;
    uint32_t mid = table_entry(&ip->dindirect, fb / NINDIRECT, alloc);
    if (!mid) return 0;
    return table_entry(&mid, fb % NINDIRECT, alloc);
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

/* Shrink a file to 'newsize' bytes, freeing every block past the end
 * (including pointer blocks that become empty). */
static void itrunc(uint32_t ino, uint32_t newsize) {
    inode_t *ip = &inodes[ino];
    if (newsize >= ip->size) return;
    uint32_t keep = (newsize + BLOCK_SIZE - 1) / BLOCK_SIZE;   /* blocks still needed */

    for (uint32_t i = keep; i < NDIRECT; i++) { bfree(ip->direct[i]); ip->direct[i] = 0; }

    if (ip->indirect) {
        uint32_t *t = (uint32_t *)BLOCK(ip->indirect);
        for (uint32_t i = 0; i < NINDIRECT; i++)
            if (NDIRECT + i >= keep) { bfree(t[i]); t[i] = 0; }
        if (keep <= NDIRECT) { bfree(ip->indirect); ip->indirect = 0; }
    }

    if (ip->dindirect) {
        uint32_t *d = (uint32_t *)BLOCK(ip->dindirect);
        for (uint32_t j = 0; j < NINDIRECT; j++) {
            if (!d[j]) continue;
            uint32_t first = NDIRECT + NINDIRECT + j * NINDIRECT;   /* file block of m[0] */
            uint32_t *m = (uint32_t *)BLOCK(d[j]);
            for (uint32_t k = 0; k < NINDIRECT; k++)
                if (first + k >= keep) { bfree(m[k]); m[k] = 0; }
            if (first >= keep) { bfree(d[j]); d[j] = 0; }
        }
        if (keep <= NDIRECT + NINDIRECT) { bfree(ip->dindirect); ip->dindirect = 0; }
    }

    /* Zero the rest of the last kept block, so growing the file again
     * later reads zeros there and not old data. */
    if (newsize % BLOCK_SIZE) {
        uint32_t b = bmap(ip, newsize / BLOCK_SIZE, false);
        if (b) memset(BLOCK(b) + newsize % BLOCK_SIZE, 0, BLOCK_SIZE - newsize % BLOCK_SIZE);
    }
    ip->size = newsize;
    ip->mtime = timer_ticks();
}

static void itruncate(uint32_t ino) {
    itrunc(ino, 0);
    inodes[ino].size = 0;          /* also when it was already empty */
}

static uint32_t count_table(uint32_t tbl) {
    uint32_t n = 0, *t = (uint32_t *)BLOCK(tbl);
    for (uint32_t i = 0; i < NINDIRECT; i++) if (t[i]) n++;
    return n;
}

/* Disk blocks used by a file, pointer blocks included */
static uint32_t iblocks(inode_t *ip) {
    uint32_t n = 0;
    for (int i = 0; i < NDIRECT; i++) if (ip->direct[i]) n++;
    if (ip->indirect) n += 1 + count_table(ip->indirect);
    if (ip->dindirect) {
        n++;
        uint32_t *d = (uint32_t *)BLOCK(ip->dindirect);
        for (uint32_t j = 0; j < NINDIRECT; j++)
            if (d[j]) n += 1 + count_table(d[j]);
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

/* Point an existing entry at another inode (used to fix ".." after a move) */
static void dir_set(uint32_t dir, const char *name, uint32_t ino) {
    dirent_t de;
    for (uint32_t off = 0; off < inodes[dir].size; off += sizeof de) {
        iread(dir, off, &de, sizeof de);
        if (de.ino && !strcmp(de.name, name)) {
            de.ino = ino;
            iwrite(dir, off, &de, sizeof de);
            return;
        }
    }
}

/* Remove an entry. Free slots at the end are cut off, so a directory that
 * had many files and lost them gives its blocks back. */
static void dir_remove(uint32_t dir, const char *name) {
    dirent_t de;
    for (uint32_t off = 0; off < inodes[dir].size; off += sizeof de) {
        iread(dir, off, &de, sizeof de);
        if (de.ino && !strcmp(de.name, name)) {
            de.ino = 0;
            iwrite(dir, off, &de, sizeof de);
            break;
        }
    }
    uint32_t size = inodes[dir].size;
    while (size >= sizeof de) {
        iread(dir, size - sizeof de, &de, sizeof de);
        if (de.ino) break;                       /* "." and ".." always stop this */
        size -= sizeof de;
    }
    itrunc(dir, size);
}

static bool dir_is_empty(uint32_t dir) {
    dirent_t de;
    for (uint32_t off = 0; off < inodes[dir].size; off += sizeof de) {
        iread(dir, off, &de, sizeof de);
        if (de.ino && strcmp(de.name, ".") && strcmp(de.name, "..")) return false;
    }
    return true;
}

/* Is directory 'a' the same as 'd' or one of its ancestors? Walks up ".." */
static bool is_ancestor(uint32_t a, uint32_t d) {
    for (int guard = 0; guard < MAX_INODES; guard++) {
        if (d == a) return true;
        if (d == ROOT_INO) return false;
        d = dir_lookup(d, "..");
        if (!d) return false;
    }
    return true;                                 /* a loop: refuse to be safe */
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

static void ifree(uint32_t ino) {
    itruncate(ino);
    inodes[ino].type = 0;
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
            ifree(ino); return -E_NOSPC;
        }
    }
    if (dir_add(parent, name, ino) < 0) { ifree(ino); return -E_NOSPC; }
    return ino;
}

/* ======================= public API ======================= */
void fs_init(void) {
    disk = kcalloc(NUM_BLOCKS * BLOCK_SIZE);
    if (!disk) panic("fs: cannot allocate RAM disk");
    memset(block_bitmap, 0, sizeof block_bitmap);
    memset(inodes, 0, sizeof inodes);
    free_blocks = NUM_BLOCKS - 1;                  /* block 0 is never used */
    alloc_hint = 1;
    mutex_init(&fs_lock);

    /* mkfs: root directory is inode 1, its parent is itself */
    uint32_t root = ialloc(FT_DIR);
    dir_add(root, ".", root);
    dir_add(root, "..", root);
}

#define LOCKED(expr) ({ mutex_lock(&fs_lock); int _r = (expr); mutex_unlock(&fs_lock); _r; })

int fs_lookup(const char *path)            { return LOCKED(resolve(path, false, NULL)); }
int fs_create(const char *path, int type)  { return LOCKED(create_locked(path, type)); }

/* Look up (and maybe create / truncate) a file AND count it as open, all
 * under the lock, so nobody can delete it between the lookup and the count. */
static int open_locked(const char *path, int flags) {
    int ino = resolve(path, false, NULL);
    if (ino == -E_NOENT && (flags & O_CREAT)) ino = create_locked(path, FT_FILE);
    if (ino < 0) return ino;
    bool writing = (flags & O_ACCMODE) != O_RDONLY;
    if (inodes[ino].type == FT_DIR && writing) return -E_ISDIR;
    if ((flags & O_TRUNC) && inodes[ino].type == FT_FILE && writing) itruncate(ino);
    fs_open_ref(ino, +1);
    return ino;
}
int fs_open(const char *path, int flags) { return LOCKED(open_locked(path, flags)); }

static int unlink_locked(const char *path) {
    char name[NAME_MAX + 1];
    int parent = resolve(path, true, name);
    if (parent < 0) return parent;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -E_INVAL;
    uint32_t ino = dir_lookup(parent, name);
    if (!ino) return -E_NOENT;
    if (ino == ROOT_INO) return -E_PERM;
    inode_t *ip = &inodes[ino];

    if (ip->type == FT_FILE && ip->nlink > 1) {     /* other names remain: just drop this one */
        dir_remove(parent, name);
        ip->nlink--;
        return 0;
    }
    if (ip->open_count) return -E_BUSY;
    if (ip->type == FT_DIR) {
        if (!dir_is_empty(ino)) return -E_NOTEMPTY;
        task_t *tab = task_table();
        for (int i = 0; i < MAX_TASKS; i++)          /* someone's working directory? */
            if (tab[i].state != T_UNUSED && tab[i].cwd == ino) return -E_BUSY;
    }
    dir_remove(parent, name);
    ifree(ino);
    return 0;
}
int fs_unlink(const char *path) { return LOCKED(unlink_locked(path)); }

/* Hard link: a second name for an existing file (not for directories,
 * which would allow loops in the tree). */
static int link_locked(const char *existing, const char *newpath) {
    int ino = resolve(existing, false, NULL);
    if (ino < 0) return ino;
    if (inodes[ino].type == FT_DIR) return -E_ISDIR;
    if (inodes[ino].nlink == 0xFFFF) return -E_NOSPC;
    char name[NAME_MAX + 1];
    int parent = resolve(newpath, true, name);
    if (parent < 0) return parent;
    if (!strcmp(name, ".") || !strcmp(name, "..") || dir_lookup(parent, name)) return -E_EXIST;
    int r = dir_add(parent, name, ino);
    if (r < 0) return r;
    inodes[ino].nlink++;
    return 0;
}
int fs_link(const char *existing, const char *newpath) { return LOCKED(link_locked(existing, newpath)); }

static int rename_locked(const char *from, const char *to) {
    char oname[NAME_MAX + 1], nname[NAME_MAX + 1];
    int op = resolve(from, true, oname);
    if (op < 0) return op;
    int np = resolve(to, true, nname);
    if (np < 0) return np;
    uint32_t ino = dir_lookup(op, oname);
    if (!ino || !strcmp(oname, ".") || !strcmp(oname, "..")) return -E_NOENT;
    if (!strcmp(nname, ".") || !strcmp(nname, "..")) return -E_INVAL;
    uint32_t target = dir_lookup(np, nname);
    if (target && inodes[target].type == FT_DIR) {       /* "mv x dir" -> into dir */
        np = target;
        strncpy(nname, oname, NAME_MAX + 1);
        if (dir_lookup(np, nname)) return -E_EXIST;
    } else if (target) return -E_EXIST;

    bool is_dir = inodes[ino].type == FT_DIR;
    /* A directory may move anywhere except into itself or below itself,
     * which would cut it off from the tree in a loop. */
    if (is_dir && (uint32_t)np != (uint32_t)op && is_ancestor(ino, np)) return -E_INVAL;

    int r = dir_add(np, nname, ino);
    if (r < 0) return r;
    dir_remove(op, oname);
    if (is_dir && np != op) dir_set(ino, "..", np);      /* new parent */
    return 0;
}
int fs_rename(const char *from, const char *to) { return LOCKED(rename_locked(from, to)); }

static int stat_locked(uint32_t ino, stat_t *st) {
    if (!ino || ino >= MAX_INODES || !inodes[ino].type) return -E_NOENT;
    inode_t *ip = &inodes[ino];
    st->ino = ino; st->type = ip->type; st->size = ip->size;
    st->blocks = iblocks(ip); st->ctime = ip->ctime; st->mtime = ip->mtime;
    st->nlink = ip->nlink;
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
    *tb = NUM_BLOCKS - 1;
    *ub = *tb - free_blocks;
    *ui = 0;
    for (uint32_t i = 1; i < MAX_INODES; i++) if (inodes[i].type) (*ui)++;
    *ti = MAX_INODES - 1;
}
