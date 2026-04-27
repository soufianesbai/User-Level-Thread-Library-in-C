#include "preemption.h"
#include "thread_sync_internal.h"
#include "thread_internal.h"
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/queue.h>

#ifdef THREAD_MULTICORE
#include <pthread.h>
#endif

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
#endif

struct thread_queue *thread_get_ready_queue(void) {
  return &ready_queue;
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

thread *thread_scheduler_pick_next(void) {
  SCHED_LOCK();
  thread *next = thread_scheduler_pick_next_locked();
  SCHED_UNLOCK();
  return next;
}

int swap_thread(thread *prev, thread *next) {
  thread_set_current(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);
  return 0;
}

int thread_yield(void) {
#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  thread *prev = thread_get_current();

#ifdef THREAD_MULTICORE
  thread *worker_stub = thread_scheduler_get_worker_stub();
  if (worker_stub != NULL) {
    /*
     * Tell the worker loop which thread is yielding so it can re-enqueue us
     * AFTER fast_swap_context has saved our register state. Enqueueing before
     * the save would let another worker restore a half-written context.
     */
    tls_last_yielded = prev;
    fast_swap_context(&prev->context, &worker_stub->context);
    /* Resumed here when the worker loop picks us again. */
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
    /*
     * Direct thread-to-thread swaps are unsafe in multicore: prev would be
     * enqueued before fast_swap_context saves its registers, letting another
     * worker restore a half-written context (same race as the yield bug).
     * Instead, put target at the head of the queue so this worker picks it
     * immediately, then yield back through the worker stub safely.
     */
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
    thread_scheduler_enqueue_locked(th);
  }
#endif
  SCHED_UNLOCK();

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

#ifdef THREAD_MULTICORE
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
        /*
         * Clear before running so thread_exit (which doesn't set
         * tls_last_yielded) doesn't leave a stale pointer from a prior yield.
         */
        tls_last_yielded = NULL;
        thread_set_current(next);
        fast_swap_context(&tls_worker_stub.context, &next->context);
        thread_set_current(&tls_worker_stub);
        tls_worker_stub.state = THREAD_RUNNING;
        /*
         * Re-enqueue the thread that yielded back to us via the worker-stub
         * path. The context is now fully saved, so it is safe to make it
         * visible to other workers. Threads that exited or blocked have states
         * THREAD_TERMINATED / THREAD_BLOCKED and are skipped.
         */
        SCHED_LOCK();
        thread *yielded = tls_last_yielded;
        if (yielded != NULL && yielded->state == THREAD_RUNNING) {
          yielded->state = THREAD_READY;
          thread_scheduler_enqueue_locked(yielded);
        }
        tls_last_yielded = NULL;
        SCHED_UNLOCK();
        goto continue_loop;
      }
      SCHED_WAIT();
    }
    SCHED_UNLOCK();
    break;

  continue_loop:
    ;
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
    if (pthread_create(&worker_threads[created], NULL, thread_worker_loop,
                       (void *)(intptr_t)created) != 0) {
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
