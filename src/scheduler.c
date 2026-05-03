#include "preemption.h"
#include "thread_internal.h"
#include <errno.h>
#include <stddef.h>
#include <sys/queue.h>

/*
 * ready_queue — the run queue holding all READY threads.
 *
 * Under THREAD_SCHED_FIFO threads are inserted at the tail and dequeued
 * from the head, giving strict round-robin ordering.
 *
 * Under THREAD_SCHED_PRIO threads are inserted in descending priority
 * order so the head always holds the highest-priority runnable thread.
 */
static struct thread_queue ready_queue = TAILQ_HEAD_INITIALIZER(ready_queue);

struct thread_queue *thread_get_ready_queue(void) {
  return &ready_queue;
}

int thread_ready_queue_empty(void) {
  return TAILQ_EMPTY(&ready_queue);
}

/*
 * thread_scheduler_enqueue — insert t into the ready queue.
 *
 * The in_ready_queue flag guards against double-insertion: if t is
 * already queued (e.g. called from two places racing), the second call
 * is a no-op.
 *
 * FIFO: append to tail — O(1).
 * PRIO: walk the queue to find the first thread with strictly lower
 *       priority and insert before it — O(n), but n is typically small.
 *       Threads with equal priority are appended after existing peers
 *       (stable ordering).
 */
void thread_scheduler_enqueue(thread *t) {
  if (t->in_ready_queue)
    return;
#if THREAD_SCHED_POLICY == THREAD_SCHED_FIFO
  t->in_ready_queue = 1;
  TAILQ_INSERT_TAIL(&ready_queue, t, entries);
#elif THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  thread *cursor;
  TAILQ_FOREACH(cursor, &ready_queue, entries) {
    if (t->priority > cursor->priority) {
      t->in_ready_queue = 1;
      TAILQ_INSERT_BEFORE(cursor, t, entries);
      return;
    }
  }
  t->in_ready_queue = 1;
  TAILQ_INSERT_TAIL(&ready_queue, t, entries);
#endif
}

/*
 * thread_scheduler_pick_next — dequeue and return the next thread to run.
 *
 * Always picks the head of the queue (highest priority under PRIO,
 * oldest arrival under FIFO). Returns NULL if the queue is empty.
 */
thread *thread_scheduler_pick_next(void) {
  if (TAILQ_EMPTY(&ready_queue))
    return NULL;
  thread *next = TAILQ_FIRST(&ready_queue);
  TAILQ_REMOVE(&ready_queue, next, entries);
  next->in_ready_queue = 0;
  return next;
}

/*
 * thread_yield — voluntarily give up the CPU.
 *
 * Under THREAD_SCHED_PRIO, aging is applied before picking the next
 * thread: every waiting thread gains THREAD_WAIT priority (starvation
 * prevention) and the running thread loses THREAD_AGING priority
 * (monopolisation prevention). If after aging no thread has higher
 * priority than the current one, the yield is a no-op.
 *
 * Under THREAD_SCHED_FIFO, the current thread is simply moved to the
 * tail and the head thread runs next.
 *
 * Returns 0 in all cases (errors are silent, matching pthread_yield).
 */
int thread_yield(void) {
#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  thread *prev = thread_get_current_thread();

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  thread *t;
  TAILQ_FOREACH(t, &ready_queue, entries) {
    t->priority += THREAD_WAIT;
    if (t->priority > THREAD_MAX_PRIORITY)
      t->priority = THREAD_MAX_PRIORITY;
  }

  if (prev->state == THREAD_RUNNING) {
    prev->priority -= THREAD_AGING;
    if (prev->priority < THREAD_MIN_PRIORITY)
      prev->priority = THREAD_MIN_PRIORITY;
  }
#endif

  thread *next = thread_scheduler_pick_next();

  if (!next || next == prev) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  /* Under PRIO: if no waiting thread beats the current thread's
   * (now-aged) priority, put next back and keep running. */
  if (next->priority <= prev->priority) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    thread_scheduler_enqueue(next);
    return 0;
  }
#endif

  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    thread_scheduler_enqueue(prev);
  }

  swap_thread(prev, next);

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_yield_to — yield directly to a specific target thread.
 *
 * If target is not READY (already running or blocked), falls back to
 * a normal thread_yield(). Otherwise, removes target from the ready
 * queue and switches to it immediately, bypassing the scheduler's
 * ordering policy.
 *
 * target is removed from the queue with TAILQ_REMOVE before the swap
 * so that pick_next cannot return it a second time while it is already
 * RUNNING.
 */
int thread_yield_to(thread_t target_handle) {
  if (target_handle == NULL) {
    errno = EINVAL;
    return -1;
  }

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  thread *target = (thread *)target_handle;

  if (target->state != THREAD_READY) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return thread_yield();
  }

  thread *prev = thread_get_current_thread();
  prev->state = THREAD_READY;
  thread_scheduler_enqueue(prev);

  TAILQ_REMOVE(&ready_queue, target, entries);
  target->in_ready_queue = 0;

  swap_thread(prev, target);

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_set_priority — change the scheduling priority of thread t.
 *
 * The value is clamped to [THREAD_MIN_PRIORITY, THREAD_MAX_PRIORITY].
 * Under THREAD_SCHED_PRIO, if t is already in the ready queue it is
 * re-inserted at the correct position for its new priority.
 * Under THREAD_SCHED_FIFO, priority is stored but has no scheduling
 * effect.
 *
 * Returns 0 on success, -1 if t is NULL.
 */
int thread_set_priority(thread_t t, int prio) {
  if (!t)
    return -1;

  thread *th = (thread *)t;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  if (prio > THREAD_MAX_PRIORITY)
    prio = THREAD_MAX_PRIORITY;
  if (prio < THREAD_MIN_PRIORITY)
    prio = THREAD_MIN_PRIORITY;

  th->priority = prio;

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  if (th->state == THREAD_READY) {
    TAILQ_REMOVE(&ready_queue, th, entries);
    th->in_ready_queue = 0;
    thread_scheduler_enqueue(th);

    /* If the target now outranks the calling thread, yield directly to it. */
    thread *current = thread_get_current_thread();
    if (th != current && th->priority > current->priority) {
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      return thread_yield_to(t);
    }
  }
#endif

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}
