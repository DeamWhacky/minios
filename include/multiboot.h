#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "common.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;     /* KiB below 1 MiB  (valid if flags bit 0) */
    uint32_t mem_upper;     /* KiB above 1 MiB  (valid if flags bit 0) */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed)) multiboot_info_t;

#endif
