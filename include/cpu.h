#ifndef CPU_H
#define CPU_H

#include "common.h"

/* Register snapshot pushed by isr_common (boot/interrupts.asm).
 * Field order must exactly mirror the push order (reversed). */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;   /* pusha */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;                                /* pushed by CPU */
    uint32_t useresp, ss;            /* pushed by CPU ONLY when coming from ring 3 */
} __attribute__((packed)) regs_t;

/* Segment selectors (index * 8 | requested privilege level) */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE (0x18 | 3)
#define SEL_UDATA (0x20 | 3)
#define SEL_TSS   0x28

void tss_set_kernel_stack(uint32_t esp0);

/* A handler may return a different stack pointer to switch tasks.
 * Returning 0 means "keep running the current task". */
typedef uint32_t (*irq_handler_t)(regs_t *r);

void gdt_init(void);
void idt_init(void);
void pic_init(void);
void register_handler(uint8_t vector, irq_handler_t h);
void irq_unmask(uint8_t irq);

#define IRQ_BASE 32
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

#endif
