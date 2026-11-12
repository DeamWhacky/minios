#ifndef DEMOS_H
#define DEMOS_H

void demo_sched(void);
void demo_prodcons(void);
void demo_philosophers(void);
void demo_race(void);
void demo_syscalls(void);
void demo_crash(int kind);     /* 0 = NULL write, 1 = unmapped read, 2 = divide by zero */
void demo_heap(void);
int  spawn_spinner(int prio);

void shell_main(void *arg);

#endif
