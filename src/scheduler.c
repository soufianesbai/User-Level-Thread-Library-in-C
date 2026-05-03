#include "preemption.h"
#include "thread_internal.h"
#include <errno.h>
#include <stddef.h>
#include <sys/queue.h>

/* ── FIFO: array ring buffer ─────────────────────────────────────────────── */
#if THREAD_SCHED_POLICY == THREAD_SCHED_FIFO

/*
 * Ring buffer for the ready queue.
 * O(1) enqueue/dequeue with bitmask wrapping — no pointer chasing.
 * Size must be >= max simultaneously READY threads; 1<<18 = 262144 covers
 * fib(29+) peak plus slack for lazy-deleted slots left by thread_yield_to.
 */
#define RING_BITS 18
#define RING_SIZE (1 << RING_BITS)
#define RING_MASK (RING_SIZE - 1)

static thread *ready_ring[RING_SIZE];
static unsigned ring_head = 0;
static unsigned ring_tail = 0;

struct thread_queue *thread_get_ready_queue(void) { return NULL; }

int thread_ready_queue_empty(void) { return ring_head == ring_tail; }

void thread_scheduler_enqueue(thread *t) {
  if (t->in_ready_queue)
    return;
  t->in_ready_queue = 1;
  ready_ring[ring_tail++ & RING_MASK] = t;
}

/*
 * Pop the next runnable thread.
 * Slots whose in_ready_queue was cleared by thread_yield_to are skipped
 * (lazy deletion — avoids O(n) search for arbitrary removal in a ring).
 */
thread *thread_scheduler_pick_next(void) {
  while (ring_head != ring_tail) {
    thread *next = ready_ring[ring_head++ & RING_MASK];
    if (next->in_ready_queue) {
      next->in_ready_queue = 0;
      return next;
    }
  }
  return NULL;
}

/* ── PRIO: sorted TAILQ ──────────────────────────────────────────────────── */
#elif THREAD_SCHED_POLICY == THREAD_SCHED_PRIO

static struct thread_queue ready_queue = TAILQ_HEAD_INITIALIZER(ready_queue);

struct thread_queue *thread_get_ready_queue(void) { return &ready_queue; }

int thread_ready_queue_empty(void) { return TAILQ_EMPTY(&ready_queue); }

void thread_scheduler_enqueue(thread *t) {
  if (t->in_ready_queue)
    return;
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
}

thread *thread_scheduler_pick_next(void) {
  if (TAILQ_EMPTY(&ready_queue))
    return NULL;
  thread *next = TAILQ_FIRST(&ready_queue);
  TAILQ_REMOVE(&ready_queue, next, entries);
  next->in_ready_queue = 0;
  return next;
}

#endif /* THREAD_SCHED_POLICY */

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

#if THREAD_SCHED_POLICY == THREAD_SCHED_FIFO
  /* Lazy-remove target from the ring: clear the guard so pick_next skips
   * the stale slot when it is eventually popped off the head. */
  target->in_ready_queue = 0;
#elif THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  TAILQ_REMOVE(&ready_queue, target, entries);
  target->in_ready_queue = 0;
#endif

  swap_thread(prev, target);

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

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
  }
#endif

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}
