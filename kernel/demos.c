/* demos.c — experiments that show the OS concepts in action.
 * Each demo runs from the shell; the shell blocks in task_wait() until done. */
#include "demos.h"
#include "task.h"
#include "sync.h"
#include "memory.h"
#include "console.h"
#include "drivers.h"
#include "syscall.h"

static void busy(uint32_t n) { for (volatile uint32_t i = 0; i < n; i++) ; }

/* cpu_ticks is updated by the timer interrupt, so read it through volatile
 * or the compiler may hoist the load out of the loop. */
static uint32_t cpu_used(task_t *t) { return *(volatile uint32_t *)&t->cpu_ticks; }

/* =====================================================================
 *  1. Scheduling comparison: CPU-bound jobs with different bursts and
 *     priorities. Prints a Gantt chart plus turnaround / waiting /
 *     response times so RR, PRIORITY and FCFS can be compared.
 * ===================================================================== */
typedef struct {
    const char *name;
    int prio;
    uint32_t burst;                       /* CPU ticks needed */
    int pid;
    uint32_t arrival, start, finish;
    volatile bool started;
} job_t;

static void job_fn(void *arg) {
    job_t *j = arg;
    task_t *self = task_current();
    j->start = timer_ticks();
    j->started = true;
    while (cpu_used(self) < j->burst)    /* the scheduler charges ticks to us */
        busy(1000);
    j->finish = timer_ticks();
}

void demo_sched(void) {
    static job_t jobs[] = {
        { "P1", 3, 60, 0, 0, 0, 0, false },
        { "P2", 8, 20, 0, 0, 0, 0, false },
        { "P3", 5, 40, 0, 0, 0, 0, false },
        { "P4", 5, 30, 0, 0, 0, 0, false },
    };
    const int n = 4;
    static uint8_t trace[600];

    kprintf("Policy: %s, quantum %u ticks (1 tick = %u ms)%s\n",
            sched_policy_name(sched_get_policy()), sched_get_quantum(), 1000 / TIMER_HZ,
            sched_get_policy() == SCHED_PRIORITY ? (sched_get_aging() ? ", aging on" : ", aging off") : "");
    kprintf("Starting 4 CPU-bound jobs (all arrive at t=0)...\n");

    uint32_t sw0 = sched_context_switches();
    uint32_t f = irq_save();              /* create all jobs atomically: same arrival time */
    uint32_t t0 = timer_ticks();
    for (int i = 0; i < n; i++) {
        jobs[i].started = false;
        jobs[i].arrival = t0;
        jobs[i].pid = task_create(jobs[i].name, job_fn, &jobs[i], jobs[i].prio);
    }
    sched_trace_start(trace, sizeof trace);
    irq_restore(f);

    for (int i = 0; i < n; i++) task_wait(jobs[i].pid);
    uint32_t len = sched_trace_stop();
    uint32_t switches = sched_context_switches() - sw0;

    /* Gantt chart: one character per 2 ticks */
    kprintf("\nGantt chart (each char = 20 ms):\n  ");
    int col = 0;
    for (uint32_t t = 0; t + 1 < len; t += 2) {
        char c = '.';
        for (int i = 0; i < n; i++)
            if (trace[t] == jobs[i].pid || trace[t + 1] == jobs[i].pid) { c = '1' + i; break; }
        console_putc(c);
        if (++col == 70) { kprintf("\n  "); col = 0; }
    }
    kprintf("\n  (1..4 = P1..P4, '.' = shell/idle)\n\n");

    kprintf("Job  Prio  Burst  Response  Finish  Turnaround  Waiting   (ms)\n");
    uint32_t sum_tat = 0, sum_wait = 0, sum_resp = 0;
    for (int i = 0; i < n; i++) {
        uint32_t tat  = (jobs[i].finish - jobs[i].arrival) * 10;
        uint32_t resp = (jobs[i].start  - jobs[i].arrival) * 10;
        uint32_t burst = jobs[i].burst * 10;
        uint32_t wait = tat > burst ? tat - burst : 0;
        sum_tat += tat; sum_wait += wait; sum_resp += resp;
        kprintf("%-4s %4d %6u %9u %7u %11u %8u\n", jobs[i].name, jobs[i].prio, burst,
                resp, (jobs[i].finish - t0) * 10, tat, wait);
    }
    kprintf("Average                 resp %u   turnaround %u   waiting %u\n",
            sum_resp / n, sum_tat / n, sum_wait / n);
    kprintf("Context switches during demo: %u\n", switches);
    kprintf("Tip: try 'sched rr', 'sched prio', 'sched fcfs', 'quantum 2' and run again.\n");
}

/* =====================================================================
 *  2. Producer / consumer with a bounded buffer
 *     empty = free slots, full = filled slots, mutex guards the buffer.
 * ===================================================================== */
#define BUF_SLOTS 4
#define ITEMS_PER_PRODUCER 6
static int buffer[BUF_SLOTS];
static int buf_in, buf_out, buf_count;
static semaphore_t sem_empty, sem_full;
static mutex_t buf_lock;

static void producer(void *arg) {
    int id = (int)arg;
    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        int item = id * 100 + i;
        task_sleep(60 + id * 40);                     /* "produce" takes time */
        sem_wait(&sem_empty);
        mutex_lock(&buf_lock);
        buffer[buf_in] = item;
        buf_in = (buf_in + 1) % BUF_SLOTS;
        buf_count++;
        kprintf("  producer %d  put %3d   [buffer %d/%d]\n", id, item, buf_count, BUF_SLOTS);
        mutex_unlock(&buf_lock);
        sem_signal(&sem_full);
    }
}

static void consumer(void *arg) {
    int id = (int)arg;
    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        sem_wait(&sem_full);
        mutex_lock(&buf_lock);
        int item = buffer[buf_out];
        buf_out = (buf_out + 1) % BUF_SLOTS;
        buf_count--;
        kprintf("  consumer %d  got %3d   [buffer %d/%d]\n", id, item, buf_count, BUF_SLOTS);
        mutex_unlock(&buf_lock);
        sem_signal(&sem_empty);
        task_sleep(150 + id * 50);                    /* "consume" takes time */
    }
}

void demo_prodcons(void) {
    sem_init(&sem_empty, BUF_SLOTS);
    sem_init(&sem_full, 0);
    mutex_init(&buf_lock);
    buf_in = buf_out = buf_count = 0;
    kprintf("2 producers, 2 consumers, buffer of %d slots (semaphores + mutex)\n", BUF_SLOTS);
    int p[4];
    p[0] = task_create("prod1", producer, (void *)1, 5);
    p[1] = task_create("prod2", producer, (void *)2, 5);
    p[2] = task_create("cons1", consumer, (void *)1, 5);
    p[3] = task_create("cons2", consumer, (void *)2, 5);
    for (int i = 0; i < 4; i++) task_wait(p[i]);
    kprintf("Done: all %d items produced and consumed, buffer count = %d\n",
            2 * ITEMS_PER_PRODUCER, buf_count);
}

/* =====================================================================
 *  3. Dining philosophers — deadlock avoided with a "waiter" semaphore
 *     that lets at most N-1 philosophers try to pick up forks.
 * ===================================================================== */
#define PHILS 5
static semaphore_t forks[PHILS];
static semaphore_t waiter;
static int meals[PHILS];

static void philosopher(void *arg) {
    int id = (int)arg;
    int left = id, right = (id + 1) % PHILS;
    for (int round = 0; round < 3; round++) {
        task_sleep(50 + id * 30);                      /* think */
        sem_wait(&waiter);
        sem_wait(&forks[left]);
        sem_wait(&forks[right]);
        meals[id]++;
        kprintf("  philosopher %d eats (forks %d,%d)  meal #%d\n", id, left, right, meals[id]);
        task_sleep(100);                               /* eat */
        sem_signal(&forks[right]);
        sem_signal(&forks[left]);
        sem_signal(&waiter);
    }
}

void demo_philosophers(void) {
    sem_init(&waiter, PHILS - 1);
    for (int i = 0; i < PHILS; i++) { sem_init(&forks[i], 1); meals[i] = 0; }
    kprintf("%d philosophers, 3 meals each, waiter allows %d at the table\n", PHILS, PHILS - 1);
    int pid[PHILS];
    for (int i = 0; i < PHILS; i++) {
        char name[] = "phil0";
        name[4] = '0' + i;
        pid[i] = task_create(name, philosopher, (void *)i, 5);
    }
    for (int i = 0; i < PHILS; i++) task_wait(pid[i]);
    kprintf("Everyone ate 3 times. No deadlock.\n");
}

/* =====================================================================
 *  4. Race condition: 3 tasks increment a shared counter.
 *     Without a mutex, preemption between read and write loses updates.
 * ===================================================================== */
static volatile uint32_t shared_counter;
static mutex_t counter_lock;
static bool use_lock;

static void racer(void *arg) {
    volatile uint32_t *my_count = arg;
    task_t *self = task_current();
    while (cpu_used(self) < 25) {
        if (use_lock) mutex_lock(&counter_lock);
        uint32_t tmp = shared_counter;     /* read  */
        busy(2000);                        /* ...a preemption here is fatal... */
        shared_counter = tmp + 1;          /* write */
        if (use_lock) mutex_unlock(&counter_lock);
        (*my_count)++;
    }
}

static void run_race(bool lock) {
    static volatile uint32_t counts[3];
    shared_counter = 0;
    use_lock = lock;
    mutex_init(&counter_lock);
    int pid[3];
    for (int i = 0; i < 3; i++) {
        counts[i] = 0;
        pid[i] = task_create(lock ? "racer-safe" : "racer", racer, (void *)&counts[i], 5);
    }
    for (int i = 0; i < 3; i++) task_wait(pid[i]);
    uint32_t expected = counts[0] + counts[1] + counts[2];
    kprintf("  %-13s expected %6u  got %6u  -> %s (lost %u updates)\n",
            lock ? "with mutex:" : "without lock:", expected, shared_counter,
            expected == shared_counter ? "CORRECT" : "RACE!", expected - shared_counter);
}

void demo_race(void) {
    sched_policy_t old = sched_get_policy();
    if (old == SCHED_FCFS) sched_set_policy(SCHED_RR);  /* races need preemption */
    kprintf("3 tasks each increment a shared counter for 250 ms of CPU time:\n");
    run_race(false);
    run_race(true);
    sched_set_policy(old);
}

/* =====================================================================
 *  5. System calls: a kernel thread that talks to the kernel only via
 *     int 0x80, exactly like a user program would (user/ has real ones)
 * ===================================================================== */
static void say(const char *s) { sys_write(STDOUT, s, strlen(s)); }

static void syscall_task(void *arg) {
    (void)arg;
    kprintf("  [task] SYS_GETPID -> %d\n", sys_getpid());
    uint32_t t0 = sys_ticks();
    for (int i = 0; i < 3; i++) {
        say("  [task] SYS_SLEEP 300 ms...\n");
        sys_sleep(300);
    }
    kprintf("  [task] SYS_TICKS says %u ms passed\n", (sys_ticks() - t0) * 10);

    char *argv[] = { "hello", "from", "syscall-demo", NULL };
    int child = sys_spawn("/bin/hello", argv);
    kprintf("  [task] SYS_SPAWN(/bin/hello) -> user process pid %d, SYS_WAIT...\n", child);
    int code = sys_wait(child);
    kprintf("  [task] child exited with code %d. Now SYS_EXIT(42)\n", code);
    sys_exit(42);
}

void demo_syscalls(void) {
    int pid = task_create("sysdemo", syscall_task, NULL, 5);
    int code = task_wait(pid);
    kprintf("Shell: task %d exited with code %d\n", pid, code);
}

/* =====================================================================
 *  6. Fault isolation: a buggy task crashes, the OS keeps running
 * ===================================================================== */
static void crasher(void *arg) {
    int kind = (int)arg;
    kprintf("  [crasher] about to do something terrible...\n");
    task_sleep(100);
    if (kind == 0) {
        volatile uint32_t *p = (uint32_t *)0;            /* NULL: page 0 is unmapped */
        *p = 0xDEAD;
    } else if (kind == 1) {
        volatile uint32_t *p = (uint32_t *)0x20000000;   /* never mapped */
        (void)*p;
    } else {
        volatile int zero = 0;
        kprintf("%d\n", 10 / zero);
    }
    kprintf("  [crasher] this line is never printed\n");
}

void demo_crash(int kind) {
    int pid = task_create("crasher", crasher, (void *)kind, 5);
    int code = task_wait(pid);
    kprintf("Shell: task %d was terminated (exit code %d). The OS is still alive.\n", pid, code);
}

/* =====================================================================
 *  7. Heap allocator test: allocation, splitting, freeing, coalescing
 * ===================================================================== */
static void print_heap(const char *label) {
    uint32_t total, used, blocks, free_blocks;
    heap_stats(&total, &used, &blocks, &free_blocks);
    kprintf("  %-22s heap %6u B, used %6u B, %3u blocks (%u free)\n",
            label, total, used, blocks, free_blocks);
}

void demo_heap(void) {
    print_heap("start:");
    void *p[8];
    uint32_t sizes[8] = { 16, 100, 1000, 4000, 64, 20000, 300, 128 };
    for (int i = 0; i < 8; i++) p[i] = kmalloc(sizes[i]);
    print_heap("after 8 allocs:");
    for (int i = 0; i < 8; i++)
        kprintf("    kmalloc(%5u) = %p  (phys %p)\n", sizes[i], p[i], paging_get_phys((uint32_t)p[i]));
    for (int i = 0; i < 8; i += 2) kfree(p[i]);
    print_heap("freed every 2nd:");
    for (int i = 1; i < 8; i += 2) kfree(p[i]);
    print_heap("freed all (coalesced):");
    void *big = kmalloc(200 * 1024);
    print_heap("kmalloc 200 KiB:");
    kfree(big);
    print_heap("freed it:");
}

/* =====================================================================
 *  Background CPU hog, handy for ps / nice / kill experiments
 * ===================================================================== */
static void spinner(void *arg) {
    (void)arg;
    for (;;) busy(100000);
}

/* Nobody waits for a spinner, so detach it: otherwise its task slot would
 * stay a zombie forever once it is killed. */
int spawn_spinner(int prio) {
    int pid = task_create("spinner", spinner, NULL, prio);
    if (pid > 0) task_detach(pid);
    return pid;
}
