#include "thread_internal.h"
#include "preemption.h"
#include <errno.h>
#include <stddef.h>
#include <sys/queue.h>

static struct thread_queue ready_queue = TAILQ_HEAD_INITIALIZER(ready_queue);

struct thread_queue *thread_get_ready_queue(void) {
  return &ready_queue;
}

void thread_scheduler_enqueue(thread *t) {
  if (t == NULL || t->in_ready_queue)
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

thread *thread_scheduler_pick_next(void) {
  if (TAILQ_EMPTY(&ready_queue))
    return NULL;
  thread *next = TAILQ_FIRST(&ready_queue);
  TAILQ_REMOVE(&ready_queue, next, entries);
  next->in_ready_queue = 0;
  return next;
}

int swap_thread(thread *prev, thread *next) {
  thread_set_current_thread(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);
  return 0;
}

int thread_yield(void) {
#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  reclaim_deferred_stacks_all();

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
