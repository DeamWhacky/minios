#ifndef MEMORY_H
#define MEMORY_H

#include "common.h"
#include "abi.h"

#define PAGE_SIZE 4096

/* ---- Physical memory manager: one bit per 4 KiB frame ---- */
void     pmm_init(uint32_t mem_upper_kb, uint32_t reserved_end);
uint32_t pmm_alloc_frame(void);            /* returns physical address, 0 on OOM */
uint32_t pmm_alloc_frame_low(void);        /* frame inside the identity map */
void     pmm_free_frame(uint32_t addr);
uint32_t pmm_free_count(void);
uint32_t pmm_total_count(void);

/* ---- Paging ----
 * Kernel half of every address space:
 *   0 .. IDENTITY_MAP_MB   identity mapped (virt == phys), supervisor only
 *   HEAP_START..HEAP_MAX   kernel heap, frames mapped on demand
 * User half (USER_BASE .. USER_TOP, see abi.h): private to each process.
 * Page 0 is unmapped so NULL dereferences fault. */
#define IDENTITY_MAP_MB 64
#define HEAP_START      0x10000000         /* 256 MiB virtual */
#define HEAP_MAX        0x14000000         /* 64 MiB max heap */

#define PG_PRESENT 0x1
#define PG_WRITE   0x2
#define PG_USER    0x4

void     paging_init(void);
bool     paging_map(uint32_t virt, uint32_t phys, uint32_t flags);          /* kernel dir */
bool     paging_map_in(uint32_t dir, uint32_t virt, uint32_t phys, uint32_t flags);
uint32_t paging_get_entry(uint32_t dir, uint32_t virt);                     /* raw PTE or 0 */
uint32_t paging_get_phys(uint32_t virt);                                    /* active dir */
uint32_t paging_kernel_dir(void);
uint32_t paging_new_dir(void);
void     paging_free_dir(uint32_t dir);
void     paging_switch(uint32_t dir);
uint32_t paging_count_user_pages(uint32_t dir);

/* ---- Kernel heap: first-fit free list with block splitting & coalescing ---- */
void   heap_init(void);
void  *kmalloc(size_t size);
void  *kcalloc(size_t size);
void   kfree(void *p);
void   heap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks, uint32_t *free_blocks);

#endif
