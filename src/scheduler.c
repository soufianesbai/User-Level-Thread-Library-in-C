#include "preemption.h"
#include "thread_internal.h"
#include "thread_sync_internal.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/queue.h>

#ifdef THREAD_MULTICORE
#include <pthread.h>
#endif

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

#ifdef THREAD_MULTICORE
pthread_mutex_t scheduler_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t scheduler_cond = PTHREAD_COND_INITIALIZER;

static pthread_t *worker_threads = NULL;
static int worker_count = 0;
static int scheduler_running = 0;

static THREAD_LOCAL int tls_worker_id = -1;
static THREAD_LOCAL thread tls_worker_stub;
static THREAD_LOCAL int tls_worker_stub_initialized = 0;
static THREAD_LOCAL thread *tls_last_yielded = NULL;
/*
 * Deferred join: set by thread_join() on a worker before calling
 * fast_swap_context() to the worker stub. The worker loop sets joined_by
 * AFTER the context is fully saved, so thread_exit() cannot restore the
 * joiner's context before the save is complete.
 */
static THREAD_LOCAL thread *tls_join_joiner = NULL;
static THREAD_LOCAL thread *tls_join_target_ref = NULL;
#endif

struct thread_queue *thread_get_ready_queue(void) {
  return &ready_queue;
}

int thread_ready_queue_empty(void) {
  return TAILQ_EMPTY(&ready_queue);
}

static int thread_can_run_on_current_worker(const thread *t) {
#ifdef THREAD_MULTICORE
  if (t == NULL)
    return 0;
  if (t->affinity < 0)
    return 1;
  if (tls_worker_id < 0)
    return 1;
  return t->affinity == tls_worker_id;
#else
  (void)t;
  return 1;
#endif
}

void thread_scheduler_enqueue_locked(thread *t) {
  if (t == NULL || t->in_ready_queue || t->state != THREAD_READY)
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

  SCHED_SIGNAL();
}

void thread_scheduler_enqueue(thread *t) {
  SCHED_LOCK();
  thread_scheduler_enqueue_locked(t);
  SCHED_UNLOCK();
}

thread *thread_scheduler_pick_next_locked(void) {
  thread *cursor = TAILQ_FIRST(&ready_queue);

  while (cursor != NULL) {
    thread *next_cursor = TAILQ_NEXT(cursor, entries);
    if (cursor->state == THREAD_READY && thread_can_run_on_current_worker(cursor)) {
      TAILQ_REMOVE(&ready_queue, cursor, entries);
      cursor->in_ready_queue = 0;
      cursor->state = THREAD_RUNNING;
      return cursor;
    }
    cursor = next_cursor;
  }

  return NULL;
}

/*
 * thread_scheduler_pick_next — dequeue and return the next thread to run.
 *
 * Always picks the head of the queue (highest priority under PRIO,
 * oldest arrival under FIFO). Returns NULL if the queue is empty.
 */
thread *thread_scheduler_pick_next(void) {
  SCHED_LOCK();
  thread *next = thread_scheduler_pick_next_locked();
  SCHED_UNLOCK();
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

  thread *prev = thread_get_current();
#ifdef THREAD_MULTICORE
  thread *worker_stub = thread_scheduler_get_worker_stub();
  if (worker_stub != NULL) {
    tls_last_yielded = prev;
    fast_swap_context(&prev->context, &worker_stub->context);
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }
#endif

  SCHED_LOCK();

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

  thread *next = thread_scheduler_pick_next_locked();

  if (!next || next == prev) {
    if (next == prev) {
      next->state = THREAD_READY;
      thread_scheduler_enqueue_locked(next);
    }
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  /* Under PRIO: if no waiting thread beats the current thread's
   * (now-aged) priority, put next back and keep running. */
  if (next->priority <= prev->priority) {
    next->state = THREAD_READY;
    thread_scheduler_enqueue_locked(next);
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }
#endif

  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    thread_scheduler_enqueue_locked(prev);
  }

  SCHED_UNLOCK();

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
  thread *prev = thread_get_current();

  SCHED_LOCK();

  if (target->state != THREAD_READY || !target->in_ready_queue) {
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return thread_yield();
  }

  TAILQ_REMOVE(&ready_queue, target, entries);
  target->in_ready_queue = 0;

#ifdef THREAD_MULTICORE
  thread *worker_stub = thread_scheduler_get_worker_stub();
  if (worker_stub != NULL) {
    TAILQ_INSERT_HEAD(&ready_queue, target, entries);
    target->in_ready_queue = 1;
    SCHED_UNLOCK();
    tls_last_yielded = prev;
    fast_swap_context(&prev->context, &worker_stub->context);
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }
#endif

  target->state = THREAD_RUNNING;

  prev->state = THREAD_READY;
  thread_scheduler_enqueue_locked(prev);

  SCHED_UNLOCK();

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

  SCHED_LOCK();
  th->priority = prio;

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  if (th->state == THREAD_READY && th->in_ready_queue) {
    TAILQ_REMOVE(&ready_queue, th, entries);
    th->in_ready_queue = 0;
<<<<<<< HEAD
    thread_scheduler_enqueue_locked(th);
=======
    thread_scheduler_enqueue(th);

    /* If the target now outranks the calling thread, yield directly to it. */
    thread *current = thread_get_current_thread();
    if (th != current && th->priority > current->priority) {
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      return thread_yield_to(t);
    }
>>>>>>> 7281f55 (final code)
  }
#endif
  SCHED_UNLOCK();

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

#ifdef THREAD_MULTICORE
/*
 * thread_set_deferred_join — called from thread_join() on a worker before
 * fast_swap_context(). The worker loop will atomically set joined_by after
 * the context is saved, preventing thread_exit() from restoring a half-written
 * context. Also clears tls_last_yielded so the worker loop does not re-enqueue
 * the joiner (it is BLOCKED, not RUNNING).
 */
void thread_set_deferred_join(thread *joiner, thread *target) {
  tls_join_joiner = joiner;
  tls_join_target_ref = target;
  tls_last_yielded = NULL;
}

static void thread_worker_stub_init(void) {
  if (tls_worker_stub_initialized)
    return;

  tls_worker_stub.id = -(tls_worker_id + 1);
  tls_worker_stub.state = THREAD_RUNNING;
  tls_worker_stub.start_fun = NULL;
  tls_worker_stub.arg = NULL;
  tls_worker_stub.retval = NULL;
  tls_worker_stub.valgrind_stack_id = 0;
  tls_worker_stub.stack_map = NULL;
  tls_worker_stub.stack_base = NULL;
  tls_worker_stub.joined_by = NULL;
  tls_worker_stub.waiting_for = NULL;
  tls_worker_stub.priority = THREAD_DEFAULT_PRIORITY;
  tls_worker_stub.in_ready_queue = 0;
  tls_worker_stub.affinity = tls_worker_id;
  tls_worker_stub.head_joiner = NULL;
#ifdef ENABLE_SIGNAL
  tls_worker_stub.pending_signals = 0;
  tls_worker_stub.blocked_signals = 0;
  tls_worker_stub.waited_signals = 0;
  tls_worker_stub.waiting_for_signal = 0;
#endif
  tls_worker_stub_initialized = 1;
}

static void *thread_worker_loop(void *arg) {
  tls_worker_id = (int)(intptr_t)arg;
  thread_worker_stub_init();
  thread_set_current(&tls_worker_stub);

  while (1) {
    SCHED_LOCK();
    while (scheduler_running) {
      thread *next = thread_scheduler_pick_next_locked();
      if (next != NULL) {
        SCHED_UNLOCK();
        tls_last_yielded = NULL;
        thread_set_current(next);
        fast_swap_context(&tls_worker_stub.context, &next->context);
        thread_set_current(&tls_worker_stub);
        tls_worker_stub.state = THREAD_RUNNING;
        SCHED_LOCK();
        thread *yielded = tls_last_yielded;
        if (yielded != NULL && yielded->state == THREAD_RUNNING) {
          yielded->state = THREAD_READY;
          thread_scheduler_enqueue_locked(yielded);
        }
        tls_last_yielded = NULL;
        /*
         * Deferred join: joiner's context is now fully saved. Set joined_by
         * atomically under the scheduler lock so thread_exit() cannot enqueue
         * the joiner before the save was complete. If the target already
         * terminated while we were switching, re-enqueue the joiner directly.
         */
        thread *jjoiner = tls_join_joiner;
        thread *jtarget = tls_join_target_ref;
        tls_join_joiner = NULL;
        tls_join_target_ref = NULL;
        if (jjoiner != NULL) {
          if (jtarget->state == THREAD_TERMINATED) {
            jjoiner->state = THREAD_READY;
            thread_scheduler_enqueue_locked(jjoiner);
          } else {
            jtarget->joined_by = jjoiner;
          }
        }
        SCHED_UNLOCK();
        goto continue_loop;
      }
      SCHED_WAIT();
    }
    SCHED_UNLOCK();
    break;

  continue_loop:;
  }

  return NULL;
}
#endif

void thread_scheduler_sync_init(void) {
#ifdef THREAD_MULTICORE
  SCHED_LOCK();
  scheduler_running = 1;
  SCHED_UNLOCK();
#endif
}

void thread_scheduler_sync_shutdown(void) {
#ifdef THREAD_MULTICORE
  SCHED_LOCK();
  scheduler_running = 0;
  SCHED_BROADCAST();
  SCHED_UNLOCK();

  for (int i = 0; i < worker_count; ++i) {
    pthread_join(worker_threads[i], NULL);
  }

  free(worker_threads);
  worker_threads = NULL;
  worker_count = 0;
#endif
}

int thread_scheduler_has_workers(void) {
#ifdef THREAD_MULTICORE
  return worker_count > 0;
#else
  return 0;
#endif
}

thread *thread_scheduler_get_worker_stub(void) {
#ifdef THREAD_MULTICORE
  if (tls_worker_id < 0 || !tls_worker_stub_initialized)
    return NULL;
  return &tls_worker_stub;
#else
  return NULL;
#endif
}

void thread_scheduler_wake_workers(void) {
  SCHED_LOCK();
  SCHED_BROADCAST();
  SCHED_UNLOCK();
}

int thread_set_concurrency(int nworkers) {
#ifndef THREAD_MULTICORE
  (void)nworkers;
  return 0;
#else
  if (nworkers < 0) {
    errno = EINVAL;
    return -1;
  }

  if (nworkers == 0) {
    return 0;
  }

  int effective_workers = nworkers;

  SCHED_LOCK();
  if (worker_count > 0) {
    SCHED_UNLOCK();
    return 0;
  }

  worker_threads = calloc((size_t)effective_workers, sizeof(*worker_threads));
  if (worker_threads == NULL) {
    SCHED_UNLOCK();
    errno = ENOMEM;
    return -1;
  }

  scheduler_running = 1;
  int created = 0;
  for (; created < effective_workers; ++created) {
    if (pthread_create(
            &worker_threads[created], NULL, thread_worker_loop, (void *)(intptr_t)created) != 0) {
      break;
    }
  }

  if (created != effective_workers) {
    scheduler_running = 0;
    SCHED_BROADCAST();
    SCHED_UNLOCK();
    for (int i = 0; i < created; ++i) {
      pthread_join(worker_threads[i], NULL);
    }
    free(worker_threads);
    worker_threads = NULL;
    errno = EAGAIN;
    return -1;
  }

  worker_count = effective_workers;
  SCHED_UNLOCK();
  return 0;
#endif
}

int thread_set_affinity(thread_t thread_handle, int worker_id) {
#ifndef THREAD_MULTICORE
  (void)thread_handle;
  (void)worker_id;
  return 0;
#else
  if (thread_handle == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (worker_id < -1) {
    errno = EINVAL;
    return -1;
  }

  SCHED_LOCK();
  if (worker_id >= worker_count && worker_count > 0) {
    SCHED_UNLOCK();
    errno = EINVAL;
    return -1;
  }

  thread *t = (thread *)thread_handle;
  t->affinity = worker_id;
  SCHED_SIGNAL();
  SCHED_UNLOCK();
  return 0;
#endif
}
