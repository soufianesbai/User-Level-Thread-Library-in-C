#include "thread_cond.h"
#include "preemption.h"
#include "thread_internal.h"
#include <stddef.h>
#include <sys/queue.h>

int thread_cond_init(thread_cond_t *cond) {
  if (cond == NULL)
    return -1;
  TAILQ_INIT(&cond->waiting_queue);
  return 0;
}

int thread_cond_destroy(thread_cond_t *cond) {
  if (cond == NULL || !TAILQ_EMPTY(&cond->waiting_queue))
    return -1;
  return 0;
}

/*
 * thread_cond_wait — atomically release mutex and block on the condition.
 *
 * Protocol:
 *   1. Under preem_block, release the mutex (to let other threads modify
 *      shared state) and park the current thread in the condition's
 *      waiting queue. Steps 1 and 2 are atomic with respect to signals:
 *      a wakeup cannot arrive between the unlock and the block (lost
 *      wakeup problem).
 *   2. When thread_cond_signal/broadcast wakes us, we are put back in
 *      the ready queue. On resumption, re-acquire the mutex before
 *      returning to the caller.
 */
int thread_cond_wait(thread_cond_t *cond, thread_mutex_t *mutex) {
  if (cond == NULL || mutex == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  thread *prev = thread_get_current_thread();
  thread *next = thread_scheduler_pick_next();

  /* Release the mutex while staying under preem_block.
   * If threads are waiting on the mutex, transfer ownership directly. */
  if (!TAILQ_EMPTY(&mutex->waiting_queue)) {
    thread *revived = TAILQ_FIRST(&mutex->waiting_queue);
    TAILQ_REMOVE(&mutex->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue(revived);
    /* locked stays 1: the revived thread takes ownership */
  } else {
    mutex->locked = 0;
  }

  if (next == NULL) {
    /* No other thread can signal us — would deadlock. */
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    thread_mutex_lock(mutex);
    return -1;
  }

  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&cond->waiting_queue, prev, entries);

  thread_set_current_thread(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);

  /* Execution resumes here after signal/broadcast re-enqueued us and the
   * scheduler picked us. Re-acquire the mutex before returning. */
#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  thread_mutex_lock(mutex);
  return 0;
}

/*
 * thread_cond_signal — wake one thread waiting on the condition.
 *
 * The woken thread is put back in the ready queue. It will re-acquire
 * the mutex inside thread_cond_wait() before continuing.
 * No-op if no thread is waiting.
 */
int thread_cond_signal(thread_cond_t *cond) {
  if (cond == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  if (!TAILQ_EMPTY(&cond->waiting_queue)) {
    thread *revived = TAILQ_FIRST(&cond->waiting_queue);
    TAILQ_REMOVE(&cond->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue(revived);
  }

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_cond_broadcast — wake all threads waiting on the condition.
 *
 * Each woken thread will contend for the mutex individually inside
 * thread_cond_wait() when it resumes.
 */
int thread_cond_broadcast(thread_cond_t *cond) {
  if (cond == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  while (!TAILQ_EMPTY(&cond->waiting_queue)) {
    thread *revived = TAILQ_FIRST(&cond->waiting_queue);
    TAILQ_REMOVE(&cond->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue(revived);
  }

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}
