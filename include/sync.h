#ifndef SYNC_H
#define SYNC_H

#include "task.h"

/* Counting semaphore (Dijkstra P/V). Tasks block instead of spinning. */
typedef struct {
    int          count;
    wait_queue_t waiters;
} semaphore_t;

void sem_init(semaphore_t *s, int initial);
void sem_wait(semaphore_t *s);       /* P / down */
bool sem_trywait(semaphore_t *s);
void sem_signal(semaphore_t *s);     /* V / up   */

/* Mutex = binary semaphore that remembers its owner */
typedef struct {
    semaphore_t sem;
    int         owner_pid;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

#endif
