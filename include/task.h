#ifndef TASK_H
#define TASK_H

#include "common.h"
#include "cpu.h"
#include "fs.h"

#define MAX_TASKS        32
#define TASK_STACK_SIZE  8192
#define TASK_NAME_LEN    16

#define PRIO_MIN 0
#define PRIO_MAX 9
#define PRIO_DEFAULT 5

typedef enum {
    T_UNUSED = 0,
    T_READY,        /* in the ready queue, waiting for the CPU            */
    T_RUNNING,      /* currently on the CPU                               */
    T_SLEEPING,     /* waiting for a timer deadline                       */
    T_BLOCKED,      /* waiting on a wait queue (semaphore, keyboard, ...) */
    T_ZOMBIE        /* finished; resources freed at the next switch       */
} task_state_t;

typedef enum {
    SCHED_RR = 0,   /* round robin, fixed time quantum, preemptive          */
    SCHED_PRIORITY, /* highest priority first, preemptive, with aging       */
    SCHED_FCFS      /* first come first served, non-preemptive              */
} sched_policy_t;

struct wait_queue;

typedef struct task {
    int           pid;
    char          name[TASK_NAME_LEN];
    task_state_t  state;
    int           priority;          /* 0 (lowest) .. 9 (highest)            */
    uint32_t      age;               /* ticks spent waiting in ready queue   */

    uint32_t      esp;               /* saved stack pointer (points at regs_t) */
    void         *stack;             /* kmalloc'd kernel stack               */

    void        (*entry)(void *);
    void         *arg;

    uint32_t      wake_tick;         /* for T_SLEEPING                        */
    struct wait_queue *waiting_on;   /* for T_BLOCKED                         */

    /* statistics */
    uint32_t      created_tick;
    uint32_t      cpu_ticks;
    uint32_t      switches;
    uint32_t      quantum_left;
    int           exit_code;

    /* process resources */
    bool          is_user;           /* runs in ring 3 with its own address space */
    uint32_t      page_dir;          /* physical address of its page directory */
    uint32_t      cwd;               /* current directory inode               */
    file_t       *fds[MAX_FDS];      /* open file descriptors                 */
    int           parent;
    volatile bool kill_pending;      /* kill requested while it was running   */

    struct task  *next;              /* link in ready queue / wait queue      */
} task_t;

/* A FIFO of tasks; used for the ready queue and for every blocking primitive */
typedef struct wait_queue {
    task_t *head, *tail;
} wait_queue_t;

void      tasking_init(void);
int       task_create(const char *name, void (*fn)(void *), void *arg, int priority);
int       task_create_user(const char *name, uint32_t page_dir, uint32_t entry,
                           uint32_t user_esp, int priority);
void      task_exit(int code) __attribute__((noreturn));
int       task_kill(int pid);
void      task_detach(int pid);
void      task_yield(void);
void      task_sleep(uint32_t ms);
int       task_wait(int pid);            /* block until pid exits; returns exit code */
bool      task_set_priority(int pid, int prio);
task_t   *task_current(void);
task_t   *task_get(int pid);
task_t   *task_table(void);

/* Blocking primitives (call with interrupts disabled) */
void      wq_block(wait_queue_t *wq);    /* put current task to sleep on wq */
bool      wq_wake_one(wait_queue_t *wq);
void      wq_wake_all(wait_queue_t *wq);

/* Scheduler interface */
uint32_t  schedule(regs_t *r);           /* returns ESP of next task */
uint32_t  scheduler_tick(regs_t *r);     /* called by timer IRQ every tick */
void      sched_set_policy(sched_policy_t p);
sched_policy_t sched_get_policy(void);
const char *sched_policy_name(sched_policy_t p);
void      sched_set_quantum(uint32_t ticks);
uint32_t  sched_get_quantum(void);
void      sched_set_aging(bool on);
bool      sched_get_aging(void);
uint32_t  sched_context_switches(void);
void      sched_trace_start(uint8_t *buf, uint32_t cap);
uint32_t  sched_trace_stop(void);        /* returns number of ticks recorded */
const char *task_state_name(task_state_t s);

#endif
