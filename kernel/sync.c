/* sync.c — blocking semaphores and mutexes.
 *
 * On a single CPU, disabling interrupts makes a code region atomic
 * (no timer interrupt -> no preemption). Each operation is short, and
 * waiting tasks sleep on a wait queue instead of burning CPU. */
#include "sync.h"
#include "console.h"

void sem_init(semaphore_t *s, int initial) {
    s->count = initial;
    s->waiters.head = s->waiters.tail = NULL;
}

void sem_wait(semaphore_t *s) {
    uint32_t f = irq_save();
    while (s->count <= 0)
        wq_block(&s->waiters);         /* sleep until a sem_signal wakes us, then re-check */
    s->count--;
    irq_restore(f);
}

bool sem_trywait(semaphore_t *s) {
    uint32_t f = irq_save();
    bool ok = s->count > 0;
    if (ok) s->count--;
    irq_restore(f);
    return ok;
}

void sem_signal(semaphore_t *s) {
    uint32_t f = irq_save();
    s->count++;
    wq_wake_one(&s->waiters);
    irq_restore(f);
}

void mutex_init(mutex_t *m) {
    sem_init(&m->sem, 1);
    m->owner_pid = -1;
}

/* Ownership and the per-task lock count are updated with interrupts off, so
 * there is no moment where the mutex is taken but has no owner recorded. */
void mutex_lock(mutex_t *m) {
    uint32_t f = irq_save();
    sem_wait(&m->sem);
    task_t *me = task_current();
    m->owner_pid = me->pid;
    me->locks_held++;
    irq_restore(f);
}

/* A task is never killed while it holds a mutex: the data the mutex protects
 * could be half-updated (see task_kill). The kill is remembered instead, and
 * carried out here once the task's last mutex is released. User processes
 * already die safely on their way back to user mode, so this is for kernel
 * threads. */
void mutex_unlock(mutex_t *m) {
    uint32_t f = irq_save();
    task_t *me = task_current();
    if (m->owner_pid != me->pid)
        kprintf("[mutex] warning: pid %d unlocking a mutex owned by %d\n",
                me->pid, m->owner_pid);
    else if (me->locks_held > 0)
        me->locks_held--;
    m->owner_pid = -1;
    sem_signal(&m->sem);
    bool die = !me->is_user && me->kill_pending && me->locks_held == 0;
    irq_restore(f);
    if (die) task_exit(-9);                /* the deferred kill */
}
