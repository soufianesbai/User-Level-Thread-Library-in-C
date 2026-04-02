#include "preemption.h"
#include "thread_internal.h"
#include <stddef.h>
#include <sys/queue.h>
#include <ucontext.h>

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

  preem_block();

  if (!mutex->locked) {
    // Fast path: mutex is free, acquire it immediately
    mutex->locked = 1;
    preem_unblock();
    return 0;
  }

  // Slow path: park current thread in waiting queue until unlock() wakes us.
  // We will return here with the mutex already owned (locked stays 1).
  struct thread_queue *ready_queue = thread_get_ready_queue();
  thread *prev = thread_get_current_thread();
  thread *next = TAILQ_FIRST(ready_queue);
  if (next == NULL) {
    // No other thread can unlock the mutex — deadlock
    preem_unblock();
    return -1;
  }

  TAILQ_REMOVE(ready_queue, next,
               entries); // O(1) — TAILQ knows the predecessor via tqe_prev

  // Park current thread: BLOCKED state means thread_yield() won't pick it up
  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&mutex->waiting_queue, prev, entries);

  thread_set_current_thread(next);
  next->state = THREAD_RUNNING;
  swapcontext(&prev->context, &next->context);
  thread_set_current_thread(prev);
  prev->state = THREAD_RUNNING;
  preem_unblock();

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

  preem_block();

  if (!mutex->locked) {
    // Cannot unlock a mutex that is not locked
    preem_unblock();
    return -1;
  }

  if (!TAILQ_EMPTY(&mutex->waiting_queue)) {
    // Transfer ownership directly: the revived thread becomes the new owner.
    // locked stays 1 — no window where another thread could steal the mutex.
    struct thread_queue *ready_queue = thread_get_ready_queue();
    thread *revived = TAILQ_FIRST(&mutex->waiting_queue);
    TAILQ_REMOVE(&mutex->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    TAILQ_INSERT_TAIL(ready_queue, revived, entries);
    // locked intentionally stays at 1
  } else {
    mutex->locked = 0;
  }

  preem_unblock();
  return 0;
}
