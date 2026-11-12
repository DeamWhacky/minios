/* drivers.c — PIT timer, PS/2 keyboard, input queue, power control. */
#include "drivers.h"
#include "console.h"
#include "task.h"

/* ================= PIT (8253/8254) =================
 * Channel 0 fires IRQ0 at TIMER_HZ. Each tick drives the scheduler. */
static volatile uint32_t ticks;

uint32_t timer_ticks(void) { return ticks; }

uint32_t timer_handler(regs_t *r) {
    ticks++;
    input_poll_serial();
    return scheduler_tick(r);          /* may return another task's ESP */
}

void timer_init(void) {
    uint32_t divisor = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);                  /* channel 0, lo/hi byte, mode 3 (square wave) */
    outb(0x40, divisor & 0xFF);
    outb(0x40, divisor >> 8);
    register_handler(IRQ_BASE + IRQ_TIMER, timer_handler);
    irq_unmask(IRQ_TIMER);
}

/* ================= Input queue =================
 * A classic producer/consumer ring buffer: interrupt handlers produce,
 * the shell consumes. A reader with nothing to read BLOCKS (no busy wait). */
#define INBUF 256
static char inbuf[INBUF];
static volatile uint32_t in_head, in_tail;
static wait_queue_t input_waiters;
static volatile int foreground_pid;        /* program the shell is waiting on */

void input_set_foreground(int pid) { foreground_pid = pid; }

void input_push(char c) {
    if (c == 3 && foreground_pid > 0) {    /* Ctrl+C: kill the foreground program */
        task_kill(foreground_pid);
        return;
    }
    uint32_t next = (in_head + 1) % INBUF;
    if (next == in_tail) return;       /* full: drop */
    inbuf[in_head] = c;
    in_head = next;
    wq_wake_one(&input_waiters);
}

char input_getchar(void) {
    uint32_t f = irq_save();
    while (in_head == in_tail)
        wq_block(&input_waiters);
    char c = inbuf[in_tail];
    in_tail = (in_tail + 1) % INBUF;
    irq_restore(f);
    return c;
}

void input_poll_serial(void) {
    while (serial_received()) {
        char c = serial_getc();
        if (c == '\r') c = '\n';
        if (c == 0x7F) c = '\b';
        input_push(c);
    }
}

/* ================= PS/2 keyboard (scancode set 1) ================= */
static const char keymap[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
static const char keymap_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};
static bool shift, ctrl;

static uint32_t keyboard_handler(regs_t *r) {
    (void)r;
    uint8_t sc = inb(0x60);
    bool released = sc & 0x80;
    sc &= 0x7F;

    if (sc == 0x2A || sc == 0x36) { shift = !released; return 0; }   /* shifts */
    if (sc == 0x1D)               { ctrl  = !released; return 0; }   /* ctrl   */
    if (released) return 0;

    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (ctrl && c >= 'a' && c <= 'z') c = c - 'a' + 1;   /* Ctrl+C = 3 etc. */
    if (c) input_push(c);
    return 0;
}

void keyboard_init(void) {
    while (inb(0x64) & 1) inb(0x60);   /* flush stale bytes */
    register_handler(IRQ_BASE + IRQ_KEYBOARD, keyboard_handler);
    irq_unmask(IRQ_KEYBOARD);
}

/* ================= Power ================= */
void system_reboot(void) {
    __asm__ volatile("cli");
    while (inb(0x64) & 2) ;
    outb(0x64, 0xFE);                  /* pulse CPU reset line via keyboard controller */
    for (;;) cpu_halt();
}

void system_shutdown(void) {
    __asm__ volatile("cli");
    outw(0x604, 0x2000);               /* QEMU (q35 / modern i440fx) ACPI poweroff */
    outw(0xB004, 0x2000);              /* older QEMU / Bochs */
    outw(0x4004, 0x3400);              /* VirtualBox */
    kprintf("It is now safe to turn off your computer.\n");
    for (;;) cpu_halt();
}
