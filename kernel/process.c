/* process.c — user processes: ELF loading, address spaces, argument passing,
 * and validation of pointers that user programs hand to the kernel.
 *
 * Address space of every user process:
 *
 *   0x00000000 - 0x03FFFFFF  kernel identity map   (supervisor only)
 *   0x10000000 - 0x13FFFFFF  kernel heap           (supervisor only)
 *   0x40000000 - ...         program code + data   (user, private)
 *   0x4FFFC000 - 0x4FFFFFFF  user stack, 16 KiB    (user, private)
 */
#include "process.h"
#include "task.h"
#include "memory.h"
#include "fs.h"
#include "console.h"

/* ---- ELF32 structures (just what a loader needs) ---- */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf_hdr_t;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed)) elf_phdr_t;

#define PT_LOAD 1
#define USER_IMAGE_TOP (USER_STACK_TOP - USER_STACK_SIZE)

/* Make sure every page in [start, end) exists in 'dir', zero-filled */
static bool map_user_range(uint32_t dir, uint32_t start, uint32_t end) {
    for (uint32_t va = start & ~0xFFF; va < end; va += PAGE_SIZE) {
        if (paging_get_entry(dir, va) & PG_PRESENT) continue;
        uint32_t frame = pmm_alloc_frame_low();
        if (!frame) return false;
        memset((void *)frame, 0, PAGE_SIZE);           /* never leak old data */
        if (!paging_map_in(dir, va, frame, PG_PRESENT | PG_WRITE | PG_USER)) return false;
    }
    return true;
}

/* Copy into another (not active) address space. Its frames are in the
 * identity map, so we write through their physical addresses. */
static void copy_into(uint32_t dir, uint32_t va, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) {
        uint32_t phys = (paging_get_entry(dir, va) & ~0xFFF) | (va & 0xFFF);
        uint32_t chunk = PAGE_SIZE - (va & 0xFFF);
        if (chunk > n) chunk = n;
        memcpy((void *)phys, s, chunk);
        va += chunk; s += chunk; n -= chunk;
    }
}

static const char *basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' && p[1]) b = p + 1;
    return b;
}

int process_spawn(const char *path, int argc, char *const argv[], int priority) {
    stat_t st;
    int ino = fs_lookup(path);
    if (ino < 0) return ino;
    fs_stat_ino(ino, &st);
    if (st.type != FT_FILE) return -E_NOEXEC;
    if (st.size < sizeof(elf_hdr_t)) return -E_NOEXEC;

    /* 1. read the whole executable from the file system */
    uint8_t *image = kmalloc(st.size);
    if (!image) return -E_NOMEM;
    if (fs_read(ino, 0, image, st.size) != (int)st.size) { kfree(image); return -E_NOEXEC; }

    /* 2. check it is a 32-bit x86 ELF executable */
    elf_hdr_t *eh = (elf_hdr_t *)image;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F'
        || eh->ident[4] != 1 || eh->machine != 3 || eh->type != 2
        || eh->phoff + (uint32_t)eh->phnum * sizeof(elf_phdr_t) > st.size) {
        kfree(image);
        return -E_NOEXEC;
    }

    /* 3. fresh address space; load each PT_LOAD segment */
    uint32_t dir = paging_new_dir();
    if (!dir) { kfree(image); return -E_NOMEM; }
    int err = 0;
    elf_phdr_t *ph = (elf_phdr_t *)(image + eh->phoff);
    for (int i = 0; i < eh->phnum && !err; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        uint32_t va = ph[i].vaddr, end = va + ph[i].memsz;
        if (va < USER_BASE || end > USER_IMAGE_TOP || end < va || ph[i].filesz > ph[i].memsz
            || ph[i].offset + ph[i].filesz > st.size) { err = -E_NOEXEC; break; }
        if (!map_user_range(dir, va, end)) { err = -E_NOMEM; break; }
        copy_into(dir, va, image + ph[i].offset, ph[i].filesz);   /* .bss stays zero */
    }
    uint32_t entry = eh->entry;
    kfree(image);
    if (!err && (entry < USER_BASE || entry >= USER_IMAGE_TOP)) err = -E_NOEXEC;

    /* 4. user stack, with argc/argv laid out like a C runtime expects:
     *      [argc][argv*] -> argv[0..argc-1], NULL -> the strings themselves */
    if (!err && !map_user_range(dir, USER_IMAGE_TOP, USER_STACK_TOP)) err = -E_NOMEM;
    uint32_t sp = USER_STACK_TOP;
    if (!err) {
        uint32_t uargv[MAX_ARGS_EXEC + 1];
        if (argc > MAX_ARGS_EXEC) argc = MAX_ARGS_EXEC;
        for (int i = argc - 1; i >= 0; i--) {
            uint32_t len = strlen(argv[i]) + 1;
            if (len > 256) len = 256;
            sp -= len;
            copy_into(dir, sp, argv[i], len);
            uint8_t zero = 0;
            copy_into(dir, sp + len - 1, &zero, 1);
            uargv[i] = sp;
        }
        uargv[argc] = 0;
        sp &= ~0xF;
        sp -= 4 * (argc + 1);
        copy_into(dir, sp, uargv, 4 * (argc + 1));
        uint32_t argv_ptr = sp;
        sp -= 4; copy_into(dir, sp, &argv_ptr, 4);
        sp -= 4; copy_into(dir, sp, &argc, 4);
    }
    if (err) { paging_free_dir(dir); return err; }

    /* 5. a task whose first "return from interrupt" lands in ring 3 at entry */
    int pid = task_create_user(basename(path), dir, entry, sp, priority);
    if (pid < 0) { paging_free_dir(dir); return -E_NOMEM; }
    return pid;
}

/* ------------------------------------------------------------------
 * Pointer validation. A malicious program could pass the kernel a
 * pointer into kernel memory ("please read() into 0x00100000") and the
 * kernel, running in ring 0, would happily overwrite itself. So every
 * pointer from user space is checked against the caller's page tables.
 * ------------------------------------------------------------------ */
static bool user_page_ok(uint32_t dir, uint32_t va) {
    uint32_t e = paging_get_entry(dir, va);
    return (e & PG_PRESENT) && (e & PG_USER);
}

bool user_ok(const void *p, uint32_t n) {
    task_t *t = task_current();
    if (!t->is_user) return true;                      /* kernel tasks are trusted */
    uint32_t a = (uint32_t)p;
    if (a < USER_BASE || a >= USER_TOP || n > USER_TOP - a) return false;
    if (n == 0) return true;
    for (uint32_t va = a & ~0xFFF; va < a + n; va += PAGE_SIZE)
        if (!user_page_ok(t->page_dir, va)) return false;
    return true;
}

int copy_user_str(char *dst, const char *src, uint32_t max) {
    task_t *t = task_current();
    uint32_t a = (uint32_t)src;
    for (uint32_t i = 0; i < max; i++, a++) {
        if (t->is_user) {
            if (a < USER_BASE || a >= USER_TOP) return -E_FAULT;
            if ((i == 0 || (a & 0xFFF) == 0) && !user_page_ok(t->page_dir, a)) return -E_FAULT;
        }
        dst[i] = *(const char *)a;
        if (!dst[i]) return i;
    }
    dst[max - 1] = 0;
    return -E_NAMETOOLONG;
}
