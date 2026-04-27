#include "preemption.h"
#include "thread_sync_internal.h"
#include "thread_internal.h"
#include <errno.h>
#include <stddef.h>
#include <sys/queue.h>

int thread_mutex_init(thread_mutex_t *mutex) {
  if (mutex == NULL)
    return -1;
  mutex->locked = 0;
  TAILQ_INIT(&mutex->waiting_queue);
  return 0;
}

int thread_mutex_destroy(thread_mutex_t *mutex) {
  if (mutex == NULL || !TAILQ_EMPTY(&mutex->waiting_queue)) {
    // Do not destroy a mutex if threads are still waiting on it
    return -1;
  }
  mutex->locked = -1;
  return 0;
}

/*
 * thread_mutex_lock — acquires the mutex.
 *
 * If the mutex is free: acquire it directly in O(1).
 * If the mutex is locked: park the current thread in the mutex waiting queue
 * (state = BLOCKED, not put back in ready_queue). Ownership is transferred
 * directly by thread_mutex_unlock(), so we return here already owning the
 * mutex.
 */
int thread_mutex_lock(thread_mutex_t *mutex) {
  if (mutex == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  SCHED_LOCK();

  if (mutex->locked == -1) {
    SCHED_UNLOCK();
    errno = EINVAL;
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return -1;
  }

  if (!mutex->locked) {
    // Fast path: mutex is free, acquire it immediately
    mutex->locked = 1;
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }

  // Slow path: park current thread in waiting queue until unlock() wakes us.
  // We will return here with the mutex already owned (locked stays 1).
  thread *prev = thread_get_current();
  thread *next = thread_scheduler_pick_next_locked();
  if (next == NULL) {
    SCHED_UNLOCK();
    // No other thread is ready to run — cannot proceed without risking livelock
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return -1;
  }

  // Park current thread: BLOCKED state means thread_yield() won't pick it up
  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&mutex->waiting_queue, prev, entries);

  SCHED_UNLOCK();

  swap_thread(prev, next);
#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  // When we return here, unlock() has transferred ownership to us.
  // mutex->locked is still 1 — we are the new owner.
  return 0;
}

/*
 * thread_mutex_unlock — releases the mutex.
 *
 * If threads are waiting: transfer ownership directly to the first waiter
 * (locked stays 1, no window where another thread could steal the mutex).
 * Otherwise: release the mutex (locked = 0).
 */
int thread_mutex_unlock(thread_mutex_t *mutex) {
  if (mutex == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  SCHED_LOCK();

  if (mutex->locked == -1) {
    SCHED_UNLOCK();
    errno = EINVAL;
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return -1;
  }

  if (!mutex->locked) {
    // Cannot unlock a mutex that is not locked
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return -1;
  }

  if (!TAILQ_EMPTY(&mutex->waiting_queue)) {
    // Transfer ownership directly: the revived thread becomes the new owner.
    // locked stays 1 — no window where another thread could steal the mutex.
    thread *revived = TAILQ_FIRST(&mutex->waiting_queue);
    TAILQ_REMOVE(&mutex->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue_locked(revived);
    // locked intentionally stays at 1
  } else {
    mutex->locked = 0;
  }

  SCHED_UNLOCK();

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}