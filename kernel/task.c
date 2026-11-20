/* task.c — tasks (kernel threads) and the scheduler.
 *
 * How a context switch works
 * --------------------------
 * Every interrupt saves the full CPU state on the *current task's* stack
 * (see isr_common). The stack pointer at that moment is a pointer to a
 * regs_t. Switching tasks is therefore just:
 *
 *     current->esp = (uint32_t)r;    // remember where our state is
 *     current = next;
 *     return next->esp;              // isr_common loads ESP, pops, irets
 *
 * A brand-new task gets a hand-made regs_t at the top of its stack whose
 * EIP points at task_trampoline, so "returning from an interrupt" into it
 * starts it running.
 */
#include "task.h"
#include "memory.h"
#include "console.h"
#include "drivers.h"
#include "syscall.h"
#include "fs.h"

#define AGING_INTERVAL 100        /* ticks of waiting that earn +1 priority (PRIORITY policy) */

static task_t        tasks[MAX_TASKS];
static task_t       *current;
static task_t       *idle_task;
static wait_queue_t  ready_q;
static wait_queue_t  exit_waiters;      /* tasks inside task_wait() */
static int           next_pid = 1;

static sched_policy_t policy = SCHED_RR;
static uint32_t       quantum = 5;      /* ticks (50 ms) */
static bool           aging_enabled = true;
static uint32_t       total_switches;

static uint8_t       *trace_buf;        /* optional per-tick log of the running pid */
static uint32_t       trace_len, trace_cap;

void sched_trace_start(uint8_t *buf, uint32_t cap) {
    uint32_t f = irq_save();
    trace_buf = buf; trace_cap = cap; trace_len = 0;
    irq_restore(f);
}
uint32_t sched_trace_stop(void) {
    uint32_t f = irq_save();
    trace_buf = NULL;
    irq_restore(f);
    return trace_len;
}

/* ------------------------------------------------------------------ */
/*  FIFO queue helpers                                                 */
/* ------------------------------------------------------------------ */
static void q_push(wait_queue_t *q, task_t *t) {
    t->next = NULL;
    if (q->tail) q->tail->next = t; else q->head = t;
    q->tail = t;
}

static task_t *q_pop(wait_queue_t *q) {
    task_t *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head) q->tail = NULL;
        t->next = NULL;
    }
    return t;
}

static bool q_remove(wait_queue_t *q, task_t *t) {
    task_t *prev = NULL;
    for (task_t *c = q->head; c; prev = c, c = c->next) {
        if (c != t) continue;
        if (prev) prev->next = c->next; else q->head = c->next;
        if (q->tail == c) q->tail = prev;
        c->next = NULL;
        return true;
    }
    return false;
}

static void make_ready(task_t *t) {
    t->state = T_READY;
    t->waiting_on = NULL;
    t->age = 0;
    q_push(&ready_q, t);
}

/* ------------------------------------------------------------------ */
/*  Task lifecycle                                                     */
/* ------------------------------------------------------------------ */
void tasking_init(void) {
    memset(tasks, 0, sizeof tasks);
    /* The code that is running right now (kmain, on the boot stack)
     * becomes task 0: the idle task. It runs only when nothing else can. */
    idle_task = &tasks[0];
    idle_task->pid = 0;
    strncpy(idle_task->name, "idle", TASK_NAME_LEN);
    idle_task->state = T_RUNNING;
    idle_task->priority = PRIO_MIN;
    idle_task->page_dir = paging_kernel_dir();
    idle_task->cwd = ROOT_INO;
    current = idle_task;
}

static void task_trampoline(void) {
    task_t *self = current;
    self->entry(self->arg);
    task_exit(0);                      /* fn returned normally */
}

/* Common part of creating any task: slot, kernel stack, bookkeeping.
 * Returns the task with a zeroed regs_t at the top of its kernel stack. */
static task_t *task_alloc(const char *name, int priority, regs_t **frame) {
    task_t *t = NULL;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == T_UNUSED) { t = &tasks[i]; break; }
    if (!t) return NULL;

    void *stack = kmalloc(TASK_STACK_SIZE);
    if (!stack) return NULL;

    memset(t, 0, sizeof *t);
    t->pid = next_pid++;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->priority = priority < PRIO_MIN ? PRIO_MIN : priority > PRIO_MAX ? PRIO_MAX : priority;
    t->stack = stack;
    t->created_tick = timer_ticks();
    t->page_dir = paging_kernel_dir();
    t->cwd = current->cwd ? current->cwd : ROOT_INO;    /* inherit working directory */
    t->parent = current->pid;
    fd_init_std(t);

    uint32_t top = (uint32_t)stack + TASK_STACK_SIZE - 16;
    regs_t *r = (regs_t *)(top - sizeof(regs_t));
    memset(r, 0, sizeof *r);
    t->esp = (uint32_t)r;
    *frame = r;
    return t;
}

/* Kernel thread: runs fn(arg) in ring 0 */
int task_create(const char *name, void (*fn)(void *), void *arg, int priority) {
    uint32_t f = irq_save();
    regs_t *r;
    task_t *t = task_alloc(name, priority, &r);
    if (!t) { irq_restore(f); return -1; }
    t->entry = fn;
    t->arg = arg;

    /* Fake interrupt frame: when "returned to", starts task_trampoline
     * in ring 0 with interrupts enabled (EFLAGS.IF = 1). */
    r->gs = r->fs = r->es = r->ds = SEL_KDATA;
    r->cs = SEL_KCODE;
    r->eip = (uint32_t)task_trampoline;
    r->eflags = 0x202;

    make_ready(t);
    int pid = t->pid;
    irq_restore(f);
    return pid;
}

/* User process: the fake frame "returns" into ring 3. Because the
 * privilege level changes, iret also pops the user stack pointer and
 * stack segment (useresp, ss) — that is how the CPU enters user mode. */
int task_create_user(const char *name, uint32_t page_dir, uint32_t entry,
                     uint32_t user_esp, int priority) {
    uint32_t f = irq_save();
    regs_t *r;
    task_t *t = task_alloc(name, priority, &r);
    if (!t) { irq_restore(f); return -1; }
    t->is_user = true;
    t->page_dir = page_dir;

    r->gs = r->fs = r->es = r->ds = SEL_UDATA;
    r->cs = SEL_UCODE;
    r->ss = SEL_UDATA;
    r->useresp = user_esp;
    r->eip = entry;
    r->eflags = 0x202;                     /* IF=1, IOPL=0 */

    make_ready(t);
    int pid = t->pid;
    irq_restore(f);
    return pid;
}

void task_exit(int code) {
    __asm__ volatile("cli");
    current->exit_code = code;
    current->state = T_ZOMBIE;             /* stack is freed by schedule() later */
    fs_task_died(current->pid);
    wq_wake_all(&exit_waiters);
    task_yield();
    panic("zombie task was scheduled");
}

/* Kill the running task from interrupt context and switch away.
 * Used for CPU exceptions (code -11) and pending kills (code -9). */
uint32_t task_kill_current(regs_t *r, int code) {
    current->exit_code = code;
    current->state = T_ZOMBIE;
    wq_wake_all(&exit_waiters);
    fs_task_died(current->pid);            /* don't leave fs_lock held forever */
    return schedule(r);
}

int task_kill(int pid) {
    if (pid <= 0) return -1;               /* never kill idle */
    uint32_t f = irq_save();
    task_t *t = task_get(pid);
    if (!t || t->state == T_ZOMBIE) { irq_restore(f); return -1; }
    if (t == current) {                    /* e.g. Ctrl+C from the keyboard IRQ */
        t->kill_pending = true;            /* acted on when it returns to user mode */
        irq_restore(f);
        return 0;
    }
    /* A user process preempted in the middle of a system call might hold a
     * kernel lock (e.g. the file system mutex). Killing it there would leave
     * the lock taken forever, so let it finish the call first. */
    if (t->is_user && t->state == T_READY && (((regs_t *)t->esp)->cs & 3) == 0) {
        t->kill_pending = true;
        irq_restore(f);
        return 0;
    }

    if (t->state == T_READY)                       q_remove(&ready_q, t);
    else if (t->state == T_BLOCKED && t->waiting_on) q_remove(t->waiting_on, t);
    t->exit_code = -9;
    t->state = T_ZOMBIE;
    fs_task_died(pid);
    wq_wake_all(&exit_waiters);
    irq_restore(f);
    return 0;
}

int task_wait(int pid) {
    uint32_t f = irq_save();
    for (;;) {
        task_t *t = NULL;
        for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].pid == pid) { t = &tasks[i]; break; }
        if (!t || pid == 0) { irq_restore(f); return -1; }
        if (t->state == T_ZOMBIE || t->state == T_UNUSED) {   /* exited (maybe already reaped) */
            int code = t->exit_code;
            irq_restore(f);
            return code;
        }
        wq_block(&exit_waiters);
    }
}

void task_yield(void) {
    __asm__ volatile("int $0x80" :: "a"(SYS_YIELD) : "memory");
}

void task_sleep(uint32_t ms) {
    uint32_t t = ms * TIMER_HZ / 1000;
    if (t == 0) t = 1;
    uint32_t f = irq_save();
    current->wake_tick = timer_ticks() + t;
    current->state = T_SLEEPING;
    task_yield();
    irq_restore(f);
}

bool task_set_priority(int pid, int prio) {
    task_t *t = task_get(pid);
    if (!t || prio < PRIO_MIN || prio > PRIO_MAX) return false;
    t->priority = prio;
    return true;
}

task_t *task_current(void) { return current; }
task_t *task_table(void)   { return tasks; }

task_t *task_get(int pid) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != T_UNUSED && tasks[i].pid == pid) return &tasks[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Wait queues (used by semaphores, keyboard input, task_wait)        */
/*  Must be called with interrupts disabled.                           */
/* ------------------------------------------------------------------ */
void wq_block(wait_queue_t *wq) {
    if (current == idle_task) panic("idle task tried to block");
    current->state = T_BLOCKED;
    current->waiting_on = wq;
    q_push(wq, current);
    task_yield();                          /* returns once someone wakes us */
}

bool wq_wake_one(wait_queue_t *wq) {
    task_t *t = q_pop(wq);
    if (!t) return false;
    make_ready(t);
    return true;
}

void wq_wake_all(wait_queue_t *wq) {
    while (wq_wake_one(wq)) ;
}

/* ------------------------------------------------------------------ */
/*  Scheduler                                                          */
/* ------------------------------------------------------------------ */
static int effective_priority(task_t *t) {
    int p = t->priority;
    if (aging_enabled) p += t->age / AGING_INTERVAL;   /* aging prevents starvation */
    return p;
}

/* Find (but don't remove) the task the policy would run next */
static task_t *peek_next(void) {
    if (policy != SCHED_PRIORITY) return ready_q.head;       /* RR & FCFS: FIFO order */
    task_t *best = NULL;
    for (task_t *t = ready_q.head; t; t = t->next)            /* strict '>' keeps FIFO among equals */
        if (!best || effective_priority(t) > effective_priority(best)) best = t;
    return best;
}

/* Free resources of finished tasks. Never the current one: we're on its stack. */
static void reap_zombies(void) {
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state == T_ZOMBIE && t != current) {
            fd_close_all(t);
            if (t->is_user) paging_free_dir(t->page_dir);   /* all its user memory */
            t->page_dir = 0;
            kfree(t->stack);
            t->stack = NULL;
            t->state = T_UNUSED;           /* pid & exit_code kept for task_wait() */
        }
    }
}

uint32_t schedule(regs_t *r) {
    task_t *prev = current;
    prev->esp = (uint32_t)r;

    if (prev->state == T_RUNNING) {        /* preempted or yielded: still runnable */
        if (prev == idle_task) prev->state = T_READY;
        else make_ready(prev);
    }

    task_t *next = peek_next();
    if (next) q_remove(&ready_q, next);
    else      next = idle_task;

    if (next != prev) { total_switches++; next->switches++; }
    next->state = T_RUNNING;
    next->quantum_left = quantum;
    next->age = 0;
    current = next;

    /* Switch address space, and tell the CPU which kernel stack to use if
     * this task is interrupted while in ring 3. */
    paging_switch(next->page_dir);
    if (next->stack) tss_set_kernel_stack((uint32_t)next->stack + TASK_STACK_SIZE);

    /* We are still executing on prev's stack until isr_common loads the new
     * ESP, so if prev is a zombie its stack memory must not be reused yet.
     * kfree only edits heap headers (at the bottom of the stack block) and
     * nothing can kmalloc before we switch (interrupts are off): safe. */
    reap_zombies();
    return next->esp;
}

uint32_t scheduler_tick(regs_t *r) {
    uint32_t now = timer_ticks();

    /* 1. wake up sleepers whose deadline passed */
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == T_SLEEPING && (int32_t)(now - tasks[i].wake_tick) >= 0)
            make_ready(&tasks[i]);

    /* 2. accounting */
    for (task_t *t = ready_q.head; t; t = t->next) t->age++;
    current->cpu_ticks++;
    if (trace_buf && trace_len < trace_cap)        /* record who ran this tick (Gantt chart) */
        trace_buf[trace_len++] = (uint8_t)current->pid;

    /* 3. preemption decision */
    if (current == idle_task)
        return ready_q.head ? schedule(r) : 0;

    if (current->quantum_left) current->quantum_left--;

    switch (policy) {
    case SCHED_FCFS:                       /* never preempt: run until block/yield/exit */
        return 0;
    case SCHED_RR:
        if (current->quantum_left == 0) {
            if (ready_q.head) return schedule(r);
            current->quantum_left = quantum;
        }
        return 0;
    case SCHED_PRIORITY: {
        task_t *best = peek_next();
        if (!best) { if (!current->quantum_left) current->quantum_left = quantum; return 0; }
        int bp = effective_priority(best), cp = current->priority;
        if (bp > cp) return schedule(r);                          /* higher priority ready */
        if (current->quantum_left == 0) {
            if (bp == cp) return schedule(r);                     /* round robin among equals */
            current->quantum_left = quantum;
        }
        return 0;
    }
    }
    return 0;
}

void sched_set_policy(sched_policy_t p) { policy = p; }
sched_policy_t sched_get_policy(void)   { return policy; }
void sched_set_quantum(uint32_t q)      { if (q) quantum = q; }
uint32_t sched_get_quantum(void)        { return quantum; }
void sched_set_aging(bool on)           { aging_enabled = on; }
bool sched_get_aging(void)              { return aging_enabled; }
uint32_t sched_context_switches(void)   { return total_switches; }

const char *sched_policy_name(sched_policy_t p) {
    switch (p) {
    case SCHED_RR:       return "round-robin";
    case SCHED_PRIORITY: return "priority";
    case SCHED_FCFS:     return "fcfs";
    }
    return "?";
}

const char *task_state_name(task_state_t s) {
    static const char *n[] = { "UNUSED", "READY", "RUNNING", "SLEEPING", "BLOCKED", "ZOMBIE" };
    return n[s];
}
