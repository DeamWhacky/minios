/* memory.c — physical frame allocator, paging, and the kernel heap. */
#include "memory.h"
#include "console.h"

/* =====================================================================
 *  Physical Memory Manager — bitmap, 1 bit per 4 KiB frame (1 = used)
 * ===================================================================== */
#define MAX_FRAMES (1024 * 1024 * 1024 / PAGE_SIZE)     /* supports up to 1 GiB RAM */
static uint32_t frame_bitmap[MAX_FRAMES / 32];
static uint32_t total_frames, free_frames, search_hint;

static inline void frame_set(uint32_t f)   { frame_bitmap[f / 32] |=  (1u << (f % 32)); }
static inline void frame_clear(uint32_t f) { frame_bitmap[f / 32] &= ~(1u << (f % 32)); }
static inline bool frame_used(uint32_t f)  { return frame_bitmap[f / 32] & (1u << (f % 32)); }

void pmm_init(uint32_t mem_upper_kb, uint32_t reserved_end) {
    uint32_t mem_kb = 1024 + mem_upper_kb;
    total_frames = mem_kb / 4;
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;

    memset(frame_bitmap, 0xFF, sizeof frame_bitmap);   /* everything used ... */
    free_frames = 0;
    uint32_t first_free = (reserved_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t f = first_free; f < total_frames; f++) { /* ... except RAM above the kernel */
        frame_clear(f);
        free_frames++;
    }
    search_hint = first_free;
}

uint32_t pmm_alloc_frame(void) {
    uint32_t fl = irq_save();
    for (uint32_t i = 0; i < total_frames; i++) {
        uint32_t f = (search_hint + i) % total_frames;
        if (!frame_used(f)) {
            frame_set(f);
            free_frames--;
            search_hint = f + 1;
            irq_restore(fl);
            return f * PAGE_SIZE;
        }
    }
    irq_restore(fl);
    return 0;                                           /* out of memory */
}

/* A frame inside the identity-mapped region, so the kernel can reach it
 * directly by its physical address no matter which address space is active.
 * Used for page tables, page directories and user process pages. */
uint32_t pmm_alloc_frame_low(void) {
    uint32_t fl = irq_save();
    uint32_t limit = IDENTITY_MAP_MB * 1024 * 1024 / PAGE_SIZE;
    if (limit > total_frames) limit = total_frames;
    for (uint32_t f = 1; f < limit; f++) {
        if (!frame_used(f)) {
            frame_set(f);
            free_frames--;
            irq_restore(fl);
            return f * PAGE_SIZE;
        }
    }
    irq_restore(fl);
    return 0;
}

void pmm_free_frame(uint32_t addr) {
    uint32_t fl = irq_save();
    uint32_t f = addr / PAGE_SIZE;
    if (f < total_frames && frame_used(f)) {
        frame_clear(f);
        free_frames++;
        if (f < search_hint) search_hint = f;
    }
    irq_restore(fl);
}

uint32_t pmm_free_count(void)  { return free_frames; }
uint32_t pmm_total_count(void) { return total_frames; }

/* =====================================================================
 *  Paging — two-level x86 page tables (directory -> table -> 4 KiB page)
 *
 *  Every process has its own page directory. The kernel half (identity
 *  map + heap, PDEs below USER_BASE) points at the SAME page tables in all
 *  directories, so kernel memory looks identical whichever process runs.
 *  The user half (USER_BASE..USER_TOP) is private to each process.
 * ===================================================================== */
static uint32_t kernel_dir;          /* physical address of the kernel's directory */
static uint32_t active_dir;

#define KERNEL_PDES (USER_BASE >> 22)  /* PDE slots shared by all address spaces */

static uint32_t *get_table(uint32_t dir, uint32_t virt, bool create, uint32_t pde_flags) {
    uint32_t *pd = (uint32_t *)dir;
    uint32_t pdi = virt >> 22;
    if (pd[pdi] & PG_PRESENT) return (uint32_t *)(pd[pdi] & ~0xFFF);
    if (!create) return NULL;
    uint32_t t = pmm_alloc_frame_low();
    if (!t) return NULL;
    memset((void *)t, 0, PAGE_SIZE);
    pd[pdi] = t | pde_flags;
    return (uint32_t *)t;
}

bool paging_map_in(uint32_t dir, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pde_flags = PG_PRESENT | PG_WRITE | (flags & PG_USER);
    uint32_t *table = get_table(dir, virt, true, pde_flags);
    if (!table) return false;
    table[(virt >> 12) & 0x3FF] = (phys & ~0xFFF) | flags;
    if (dir == active_dir || virt < USER_BASE)       /* kernel half is shared by all dirs */
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");   /* flush TLB entry */
    return true;
}

bool paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    return paging_map_in(kernel_dir, virt, phys, flags);
}

uint32_t paging_get_entry(uint32_t dir, uint32_t virt) {
    uint32_t *table = get_table(dir, virt, false, 0);
    return table ? table[(virt >> 12) & 0x3FF] : 0;
}

uint32_t paging_get_phys(uint32_t virt) {
    uint32_t e = paging_get_entry(active_dir, virt);
    return (e & PG_PRESENT) ? (e & ~0xFFF) | (virt & 0xFFF) : 0;
}

uint32_t paging_kernel_dir(void) { return kernel_dir; }

void paging_switch(uint32_t dir) {
    if (dir == active_dir) return;
    active_dir = dir;
    __asm__ volatile("mov %0, %%cr3" :: "r"(dir) : "memory");   /* also flushes the TLB */
}

/* New address space: kernel half shared, user half empty */
uint32_t paging_new_dir(void) {
    uint32_t dir = pmm_alloc_frame_low();
    if (!dir) return 0;
    memset((void *)dir, 0, PAGE_SIZE);
    memcpy((void *)dir, (void *)kernel_dir, KERNEL_PDES * 4);
    return dir;
}

/* Free every user page, the user page tables and the directory itself */
void paging_free_dir(uint32_t dir) {
    if (!dir || dir == kernel_dir) return;
    uint32_t *pd = (uint32_t *)dir;
    for (uint32_t i = KERNEL_PDES; i < 1024; i++) {
        if (!(pd[i] & PG_PRESENT)) continue;
        uint32_t *table = (uint32_t *)(pd[i] & ~0xFFF);
        for (int j = 0; j < 1024; j++)
            if (table[j] & PG_PRESENT) pmm_free_frame(table[j] & ~0xFFF);
        pmm_free_frame((uint32_t)table);
    }
    pmm_free_frame(dir);
}

uint32_t paging_count_user_pages(uint32_t dir) {
    uint32_t n = 0, *pd = (uint32_t *)dir;
    if (!dir || dir == kernel_dir) return 0;
    for (uint32_t i = KERNEL_PDES; i < 1024; i++) {
        if (!(pd[i] & PG_PRESENT)) continue;
        uint32_t *table = (uint32_t *)(pd[i] & ~0xFFF);
        for (int j = 0; j < 1024; j++) if (table[j] & PG_PRESENT) n++;
    }
    return n;
}

void paging_init(void) {
    kernel_dir = pmm_alloc_frame_low();
    memset((void *)kernel_dir, 0, PAGE_SIZE);
    active_dir = kernel_dir;

    /* Identity-map low memory (kernel, VGA buffer at 0xB8000, page tables).
     * Supervisor-only: user code touching it gets a page fault.
     * Page 0 is left unmapped so NULL dereferences fault. */
    for (uint32_t a = PAGE_SIZE; a < IDENTITY_MAP_MB * 1024 * 1024; a += PAGE_SIZE)
        if (!paging_map(a, a, PG_PRESENT | PG_WRITE)) panic("paging: out of frames");

    /* Pre-create the heap's page tables so every process directory, which
     * copies the kernel PDEs once, sees heap growth that happens later. */
    for (uint32_t a = HEAP_START; a < HEAP_MAX; a += 1024 * PAGE_SIZE)
        get_table(kernel_dir, a, true, PG_PRESENT | PG_WRITE);

    __asm__ volatile(
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"       /* CR0.PG = 1 : paging on */
        "mov %%eax, %%cr0\n"
        :: "r"(kernel_dir) : "eax", "memory");
}

/* =====================================================================
 *  Kernel heap — doubly linked list of blocks in address order.
 *  kmalloc: first fit, split big blocks.  kfree: merge with free neighbours.
 *  The heap grows by asking the PMM for frames and mapping them.
 * ===================================================================== */
#define HEAP_MAGIC 0xC0FFEE42
#define ALIGN16(x) (((x) + 15) & ~15u)

typedef struct block {
    uint32_t      magic;
    uint32_t      size;          /* payload bytes */
    uint32_t      free;
    struct block *prev, *next;
    uint32_t      pad[3];        /* header is 32 bytes -> payload stays 16-aligned */
} block_t;

static block_t *heap_head, *heap_tail;
static uint32_t heap_end;        /* first unmapped virtual address */

static bool heap_grow(uint32_t bytes) {
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (heap_end + pages * PAGE_SIZE > HEAP_MAX) return false;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) return false;
        paging_map(heap_end, frame, PG_PRESENT | PG_WRITE);
        heap_end += PAGE_SIZE;
    }
    return true;
}

void heap_init(void) {
    heap_end = HEAP_START;
    heap_grow(16 * PAGE_SIZE);                       /* start with 64 KiB */
    heap_head = heap_tail = (block_t *)HEAP_START;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size  = heap_end - HEAP_START - sizeof(block_t);
    heap_head->free  = 1;
    heap_head->prev  = heap_head->next = NULL;
}

static void split(block_t *b, uint32_t size) {
    if (b->size < size + sizeof(block_t) + 16) return;   /* remainder too small */
    block_t *n = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
    n->magic = HEAP_MAGIC;
    n->size  = b->size - size - sizeof(block_t);
    n->free  = 1;
    n->prev  = b;
    n->next  = b->next;
    if (b->next) b->next->prev = n; else heap_tail = n;
    b->next = n;
    b->size = size;
}

void *kmalloc(size_t req) {
    if (!req) return NULL;
    uint32_t size = ALIGN16(req);
    uint32_t f = irq_save();

    for (block_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = 0;
            irq_restore(f);
            return b + 1;
        }
    }

    /* No fit: grow the heap. If the last block is free, extend it in place. */
    uint32_t old_end = heap_end;
    uint32_t need = heap_tail->free ? size - heap_tail->size : size + sizeof(block_t);
    if (!heap_grow(need)) { irq_restore(f); return NULL; }

    block_t *b;
    if (heap_tail->free) {
        b = heap_tail;
        b->size += heap_end - old_end;
    } else {
        b = (block_t *)old_end;
        b->magic = HEAP_MAGIC;
        b->size  = heap_end - old_end - sizeof(block_t);
        b->prev  = heap_tail;
        b->next  = NULL;
        heap_tail->next = b;
        heap_tail = b;
    }
    split(b, size);
    b->free = 0;
    irq_restore(f);
    return b + 1;
}

void *kcalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)p - 1;
    if (b->magic != HEAP_MAGIC || b->free) {
        kprintf("kfree: invalid or double free of %p\n", p);
        return;
    }
    uint32_t f = irq_save();
    b->free = 1;
    if (b->next && b->next->free) {                      /* merge with right neighbour */
        block_t *n = b->next;
        b->size += sizeof(block_t) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b; else heap_tail = b;
        n->magic = 0;
    }
    if (b->prev && b->prev->free) {                      /* merge with left neighbour */
        block_t *pv = b->prev;
        pv->size += sizeof(block_t) + b->size;
        pv->next = b->next;
        if (b->next) b->next->prev = pv; else heap_tail = pv;
        b->magic = 0;
    }
    irq_restore(f);
}

void heap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks, uint32_t *free_blocks) {
    uint32_t f = irq_save();
    *total = heap_end - HEAP_START; *used = 0; *blocks = 0; *free_blocks = 0;
    for (block_t *b = heap_head; b; b = b->next) {
        (*blocks)++;
        if (b->free) (*free_blocks)++; else *used += b->size;
    }
    irq_restore(f);
}
