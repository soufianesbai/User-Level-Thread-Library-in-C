#ifndef THREAD_SEM_H
#define THREAD_SEM_H

#include "thread.h"

#ifndef USE_PTHREAD

typedef struct {
  int count;                         /* available resources; negative means threads are waiting */
  struct thread_queue waiting_queue; /* threads blocked on this semaphore, in FIFO order */
} thread_sem_t;

/* Initialize sem with the given initial value. Returns 0 on success, -1 on error. */
int thread_sem_init(thread_sem_t *sem, int value);

/* Decrement the semaphore. Blocks the calling thread if count == 0.
 * Returns 0 on success, -1 on error. */
int thread_sem_wait(thread_sem_t *sem);

/* If threads are waiting, wake one and transfer the resource directly (count unchanged).
 * Otherwise increment count. Returns 0 on success, -1 on error. */
int thread_sem_post(thread_sem_t *sem);

/* Destroy sem. Behaviour is undefined if threads are still waiting. */
int thread_sem_destroy(thread_sem_t *sem);

#else /* USE_PTHREAD — redirect to POSIX semaphores for compatibility */

#include <semaphore.h>

#define thread_sem_t sem_t
#define thread_sem_init(_sem, _value) sem_init((_sem), 0, (_value))
#define thread_sem_wait sem_wait
#define thread_sem_post sem_post
#define thread_sem_destroy sem_destroy

#endif /* USE_PTHREAD */

#endif /* THREAD_SEM_H */
