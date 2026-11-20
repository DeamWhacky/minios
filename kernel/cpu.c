/* cpu.c — GDT, IDT, 8259 PIC and the central interrupt dispatcher. */
#include "cpu.h"
#include "console.h"
#include "task.h"

extern uint32_t task_kill_current(regs_t *r, int code);

/* ================= GDT =================
 * Flat memory model: all segments cover 0..4 GiB, so segmentation is
 * effectively disabled. What the segments DO still decide is the privilege
 * level: code running from the ring-3 code segment is "user mode" and the
 * CPU refuses privileged instructions (cli, hlt, in/out, mov cr3, ...).
 * Protection of memory comes from paging (the U/S bit in page entries). */
typedef struct { uint16_t limit_lo, base_lo; uint8_t base_mid, access, gran, base_hi; } __attribute__((packed)) gdt_entry_t;
typedef struct { uint16_t limit; uint32_t base; } __attribute__((packed)) table_ptr_t;

static gdt_entry_t gdt[6];
static table_ptr_t gdt_ptr;
extern void gdt_flush(table_ptr_t *);
extern void tss_flush(void);

/* Task State Segment. We don't use hardware task switching; the TSS exists
 * only to tell the CPU which stack to use when an interrupt arrives while
 * running in ring 3 (esp0/ss0). The scheduler updates esp0 on every switch. */
typedef struct {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed)) tss_t;

static tss_t tss;

void tss_set_kernel_stack(uint32_t esp0) { tss.esp0 = esp0; }

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_lo  = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_hi  = (base >> 24) & 0xFF;
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].gran     = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access   = access;
}

void gdt_init(void) {
    gdt_set(0, 0, 0, 0, 0);                 /* null descriptor          */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);     /* 0x08: kernel code, ring 0 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);     /* 0x10: kernel data, ring 0 */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);     /* 0x18: user code,   ring 3 */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);     /* 0x20: user data,   ring 3 */
    memset(&tss, 0, sizeof tss);
    tss.ss0 = SEL_KDATA;
    tss.iomap_base = sizeof tss;            /* no I/O bitmap: ring 3 may not use in/out */
    gdt_set(5, (uint32_t)&tss, sizeof tss - 1, 0x89, 0x00);   /* 0x28: TSS */
    gdt_ptr.limit = sizeof gdt - 1;
    gdt_ptr.base  = (uint32_t)&gdt;
    gdt_flush(&gdt_ptr);
    tss_flush();
}

/* ================= IDT ================= */
typedef struct { uint16_t off_lo, sel; uint8_t zero, flags; uint16_t off_hi; } __attribute__((packed)) idt_entry_t;

static idt_entry_t idt[256];
static table_ptr_t idt_ptr;
static irq_handler_t handlers[256];
extern void idt_flush(table_ptr_t *);

#define STUB(n) extern void isr##n(void);
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9)
STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15) STUB(16) STUB(17) STUB(18) STUB(19)
STUB(20) STUB(21) STUB(22) STUB(23) STUB(24) STUB(25) STUB(26) STUB(27) STUB(28) STUB(29)
STUB(30) STUB(31) STUB(32) STUB(33) STUB(34) STUB(35) STUB(36) STUB(37) STUB(38) STUB(39)
STUB(40) STUB(41) STUB(42) STUB(43) STUB(44) STUB(45) STUB(46) STUB(47) STUB(128)

static void (*const stubs[48])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39, isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

static void idt_set(int n, void (*fn)(void), uint8_t flags) {
    uint32_t a = (uint32_t)fn;
    idt[n].off_lo = a & 0xFFFF;
    idt[n].off_hi = a >> 16;
    idt[n].sel    = 0x08;
    idt[n].zero   = 0;
    idt[n].flags  = flags;              /* 0x8E = present, ring 0, 32-bit interrupt gate */
}

void idt_init(void) {
    for (int i = 0; i < 48; i++) idt_set(i, stubs[i], 0x8E);
    /* 0xEF = present, DPL 3 (user code may execute "int 0x80"), 32-bit TRAP gate:
     * unlike interrupt gates it leaves interrupts enabled, so a long system
     * call can still be preempted by the timer. */
    idt_set(0x80, isr128, 0xEF);
    idt_ptr.limit = sizeof idt - 1;
    idt_ptr.base  = (uint32_t)&idt;
    idt_flush(&idt_ptr);
}

void register_handler(uint8_t vector, irq_handler_t h) { handlers[vector] = h; }

/* ================= 8259 PIC =================
 * The BIOS maps IRQ0-7 to vectors 8-15, which collide with CPU exceptions.
 * We remap master to 32-39 and slave to 40-47. */
#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();        /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DAT, 32);   io_wait();        /* ICW2: vector offsets   */
    outb(PIC2_DAT, 40);   io_wait();
    outb(PIC1_DAT, 4);    io_wait();        /* ICW3: slave on IRQ2    */
    outb(PIC2_DAT, 2);    io_wait();
    outb(PIC1_DAT, 0x01); io_wait();        /* ICW4: 8086 mode        */
    outb(PIC2_DAT, 0x01); io_wait();
    outb(PIC1_DAT, 0xFB);                   /* mask all except cascade */
    outb(PIC2_DAT, 0xFF);
}

void irq_unmask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

static void pic_eoi(uint8_t vector) {
    if (vector >= 40) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* ================= Exceptions ================= */
static const char *exc_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check", "Machine check",
    "SIMD FP", "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Security", "Reserved"
};


static uint32_t exception_handler(regs_t *r) {
    task_t *t = task_current();
    bool from_user = (r->cs & 3) == 3;
    console_set_color(C_LRED, C_BLACK);
    kprintf("\n[EXCEPTION] %s (#%u, err=0x%x) at EIP=%p in %s mode\n",
            exc_names[r->int_no], r->int_no, r->err_code, r->eip, from_user ? "user" : "kernel");
    if (r->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("            faulting address %p (%s, %s, from %s)\n", cr2,
                (r->err_code & 1) ? "protection violation" : "page not present",
                (r->err_code & 2) ? "write" : "read",
                (r->err_code & 4) ? "user mode" : "kernel mode");
    }
    if (r->int_no == 13 && from_user)
        kprintf("            (privileged instruction or I/O port access from ring 3)\n");
    console_set_color(C_LGREY, C_BLACK);

    /* A fault inside a normal task kills only that task: the OS survives.
     * But if the task was holding a kernel lock, it faulted halfway through
     * changing shared kernel data (e.g. the file system). Killing it would
     * leave the lock taken forever and the data inconsistent, so that is a
     * kernel bug: stop with a clear message instead of hanging later.
     * A fault in the idle task / early boot is unrecoverable too. */
    if (t && t->pid > 0 && t->locks_held > 0) {
        kprintf("            task %d '%s' holds %d kernel lock(s)\n", t->pid, t->name, t->locks_held);
        panic("fault while holding a kernel lock");
    }
    if (t && t->pid > 0) {
        kprintf("            -> killing task %d '%s'\n", t->pid, t->name);
        return task_kill_current(r, -11);
    }
    panic("unrecoverable exception in kernel context");
}

/* ================= Dispatcher (called from isr_common) ================= */
uint32_t interrupt_dispatch(regs_t *r) {
    uint32_t new_esp = 0;
    uint8_t v = (uint8_t)r->int_no;

    if (v >= 32 && v < 48) pic_eoi(v);         /* acknowledge first: we may switch away */

    if (handlers[v])        new_esp = handlers[v](r);
    else if (v < 32)        new_esp = exception_handler(r);

    /* A kill (e.g. Ctrl+C) aimed at the running task takes effect when it is
     * about to return to user mode, never in the middle of kernel code. */
    task_t *t = task_current();
    if (!new_esp && t->kill_pending && (r->cs & 3) == 3) {
        __asm__ volatile("cli");
        new_esp = task_kill_current(r, -9);
    }

    return new_esp ? new_esp : (uint32_t)r;
}
