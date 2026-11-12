; interrupts.asm — GDT/IDT loading and interrupt entry stubs.
;
; Every interrupt (CPU exception, hardware IRQ, or the int 0x80 syscall)
; funnels into isr_common, which saves the full register state on the
; current task's stack and calls the C dispatcher:
;
;     uint32_t interrupt_dispatch(regs_t *r);
;
; The dispatcher returns the stack pointer to resume. If the scheduler
; picked a different task, that is a different stack — which is the entire
; context switch: we just load ESP from EAX and pop the other task's state.

section .text

global gdt_flush
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10                ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload_cs         ; far jump reloads CS with kernel code selector
.reload_cs:
    ret

global tss_flush
tss_flush:
    mov ax, 0x28                ; GDT entry 5 = TSS
    ltr ax
    ret

global idt_flush
idt_flush:
    mov eax, [esp + 4]
    lidt [eax]
    ret

; Exceptions that do NOT push an error code: push a dummy 0 so the frame is uniform
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQs 0-15 remapped to vectors 32-47
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

ISR_NOERR 128                   ; int 0x80 — system calls

extern interrupt_dispatch
global isr_common
isr_common:
    pusha                       ; eax, ecx, edx, ebx, esp, ebp, esi, edi
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld

    push esp                    ; regs_t* argument
    call interrupt_dispatch
    mov esp, eax                ; <-- context switch happens here (maybe)

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                  ; drop int_no + err_code
    iret


section .note.GNU-stack noalloc noexec nowrite progbits
