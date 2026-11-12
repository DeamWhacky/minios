; crt0.asm — C runtime start-up for MiniOS user programs.
; The kernel enters here in ring 3 with the stack laid out as
;     [esp] = argc, [esp+4] = argv
; so a plain "call main" passes them as main's arguments.

section .text.start
global _start
extern main
extern exit

_start:
    xor ebp, ebp
    call main               ; main(argc, argv)
    push eax
    call exit               ; exit(main's return value) - never returns
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
