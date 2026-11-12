#ifndef DRIVERS_H
#define DRIVERS_H

#include "common.h"
#include "cpu.h"

/* ---- PIT timer ---- */
#define TIMER_HZ 100                       /* 1 tick = 10 ms */
void     timer_init(void);
uint32_t timer_ticks(void);
uint32_t timer_handler(regs_t *r);

/* ---- Keyboard (PS/2) + serial input share one input queue ---- */
void keyboard_init(void);
char input_getchar(void);                  /* blocks the calling task */
void input_push(char c);                   /* called from interrupt context */
void input_poll_serial(void);              /* called from the timer tick */
void input_set_foreground(int pid);        /* Ctrl+C kills this pid (0 = none) */

/* ---- Power ---- */
void system_reboot(void) __attribute__((noreturn));
void system_shutdown(void) __attribute__((noreturn));

#endif
