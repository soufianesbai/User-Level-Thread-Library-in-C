#ifndef THREAD_COND_H
#define THREAD_COND_H

#include "thread.h"

#ifndef USE_PTHREAD

typedef struct {
  struct thread_queue waiting_queue; /* threads blocked on this condition */
} thread_cond_t;

/* Initialize a condition variable. Returns 0 on success, -1 on error. */
int thread_cond_init(thread_cond_t *cond);

/*
 * Atomically release mutex and block until the condition is signalled.
 * The mutex is reacquired before returning. The caller must hold the mutex.
 * Returns 0 on success, -1 on error.
 */
int thread_cond_wait(thread_cond_t *cond, thread_mutex_t *mutex);

/* Wake one thread waiting on cond. No-op if no thread is waiting. */
int thread_cond_signal(thread_cond_t *cond);

/* Wake all threads waiting on cond. */
int thread_cond_broadcast(thread_cond_t *cond);

/* Destroy a condition variable. Behaviour is undefined if threads are waiting. */
int thread_cond_destroy(thread_cond_t *cond);

#else /* USE_PTHREAD — redirect to pthreads for compatibility */

#define thread_cond_t pthread_cond_t
#define thread_cond_init(_cond) pthread_cond_init((_cond), NULL)
#define thread_cond_wait pthread_cond_wait
#define thread_cond_signal pthread_cond_signal
#define thread_cond_broadcast pthread_cond_broadcast
#define thread_cond_destroy pthread_cond_destroy

#endif /* USE_PTHREAD */

#endif /* THREAD_COND_H */
