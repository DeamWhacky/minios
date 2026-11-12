/* Kernel is loaded at 1 MiB by the multiboot loader (QEMU -kernel / GRUB) */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    .text ALIGN(4K) : {
        *(.multiboot)          /* must be within the first 8 KiB */
        *(.text*)
    }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data   ALIGN(4K) : { *(.data*) }
    .bss    ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
    }

    kernel_end = ALIGN(4K);
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
