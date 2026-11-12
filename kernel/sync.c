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

void mutex_lock(mutex_t *m) {
    sem_wait(&m->sem);
    m->owner_pid = task_current()->pid;
}

void mutex_unlock(mutex_t *m) {
    if (m->owner_pid != task_current()->pid)
        kprintf("[mutex] warning: pid %d unlocking a mutex owned by %d\n",
                task_current()->pid, m->owner_pid);
    m->owner_pid = -1;
    sem_signal(&m->sem);
}
