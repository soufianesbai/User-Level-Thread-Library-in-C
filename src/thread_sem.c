#include "thread_sem.h"
#include "preemption.h"
#include "thread_internal.h"
#include "thread_sync_internal.h"
#include <stddef.h>
#include <sys/queue.h>

int thread_sem_init(thread_sem_t *sem, int value) {
  if (sem == NULL || value < 0)
    return -1;
  sem->count = value;
  TAILQ_INIT(&sem->waiting_queue);
  return 0;
}

int thread_sem_destroy(thread_sem_t *sem) {
  if (sem == NULL || !TAILQ_EMPTY(&sem->waiting_queue))
    return -1;
  sem->count = 0;
  return 0;
}

/*
 * thread_sem_wait — P() operation (decrement / acquire).
 *
 * Fast path (count > 0): decrement count and return immediately.
 * Slow path (count == 0): block the current thread in the waiting queue
 * until a thread_sem_post() wakes it and transfers the resource directly.
 * When we return from the slow path, post() has already accounted for
 * the resource (count unchanged by post in that case).
 */
int thread_sem_wait(thread_sem_t *sem) {
  if (sem == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

#ifdef THREAD_MULTICORE
  while (1) {
    SCHED_LOCK();
    if (sem->count > 0) {
      sem->count--;
      SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      return 0;
    }
    SCHED_UNLOCK();
    thread_yield();
  }
#endif

  SCHED_LOCK();
  if (sem->count > 0) {
    sem->count--;
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }
  SCHED_UNLOCK();

  /* Slow path: block until a post() wakes us. */
  thread *prev = thread_get_current();

  SCHED_LOCK();
  thread *next = thread_scheduler_pick_next_locked();

  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&sem->waiting_queue, prev, entries);

#ifdef THREAD_MULTICORE
  {
    thread *worker_stub = thread_scheduler_get_worker_stub();
    if (worker_stub != NULL) {
      if (next != NULL) {
        next->state = THREAD_READY;
        thread_scheduler_enqueue_locked(next);
      }
      SCHED_UNLOCK();
      fast_swap_context(&prev->context, &worker_stub->context);
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      return 0;
    }
  }
#endif

  if (next == NULL) {
    TAILQ_REMOVE(&sem->waiting_queue, prev, entries);
    prev->state = THREAD_RUNNING;
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return -1;
  }

  SCHED_UNLOCK();

  thread_set_current(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);

  /* Execution resumes here after post() woke us and transferred the resource. */
#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_sem_post — V() operation (increment / release).
 *
 * If threads are waiting: wake the first one and transfer the resource
 * directly (count stays unchanged — same hand-off pattern as mutex_unlock).
 * Otherwise: increment count so a future wait() can proceed immediately.
 */
int thread_sem_post(thread_sem_t *sem) {
  if (sem == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  SCHED_LOCK();
  if (!TAILQ_EMPTY(&sem->waiting_queue)) {
    thread *revived = TAILQ_FIRST(&sem->waiting_queue);
    TAILQ_REMOVE(&sem->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue_locked(revived);
  } else {
    sem->count++;
  }
  SCHED_UNLOCK();

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}
