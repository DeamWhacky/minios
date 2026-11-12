/* User programs are linked at USER_BASE (see include/abi.h). Every process
 * uses the same virtual addresses; paging gives each its own physical pages. */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;
    .text   : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(COMMON) *(.bss*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
