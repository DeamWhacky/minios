; boot.asm — Multiboot v1 header and kernel entry point.
; QEMU's "-kernel" option (and GRUB) load us in 32-bit protected mode,
; paging off, interrupts off, EAX = magic, EBX = multiboot info pointer.

MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ (1 << 0) | (1 << 1)     ; align modules + give us memory map
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

section .bss
align 16
stack_bottom:
    resb 16384                  ; 16 KiB boot stack (becomes the "init" task stack)
stack_top:

section .text
global _start
extern kmain

_start:
    mov esp, stack_top
    xor ebp, ebp                ; terminate stack traces
    push ebx                    ; multiboot_info_t*
    push eax                    ; magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
