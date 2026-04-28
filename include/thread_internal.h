#ifndef THREAD_INTERNAL_H
#define THREAD_INTERNAL_H

#include "context.h"
#include "pool.h"
#include "thread.h"

/*
 * Thread lifecycle states and valid transitions:
 *
 *    READY  ──>  RUNNING  ──>  TERMINATED
 *     ^             │
 *     └──  BLOCKED  ┘
 *
 * READY:      in the run queue, eligible to be scheduled.
 * RUNNING:    currently executing on the CPU.
 * BLOCKED:    waiting for an event (join, mutex, cond, signal).
 * TERMINATED: start_fun returned; retval is set; waiting for a joiner.
 */
#define THREAD_READY      0
#define THREAD_RUNNING    1
#define THREAD_TERMINATED 2
#define THREAD_BLOCKED    3

typedef struct thread {
  int id;                      /* unique thread identifier */
  struct fast_ctx context;     /* saved CPU registers (callee-saved + PC/SP only) */
  void *(*start_fun)(void *);  /* entry function supplied by the caller */
  void *arg;                   /* argument forwarded to start_fun */
  TAILQ_ENTRY(thread) entries; /* list node for ready/zombie queues */
  int state;                   /* current lifecycle state */
  void *retval;                /* value returned by start_fun, retrieved by joiner */
  unsigned valgrind_stack_id;  /* handle for Valgrind's mmap'd-stack tracking */
  void *stack_map;             /* base of the full mmap region (guard + stack) */
  void *stack_base;            /* top of the usable stack (= stack_map + GUARD_SIZE) */

  /*
   * Join relationship: if thread B calls thread_join(A),
   *   A->joined_by   = B  (who is waiting on A)
   *   B->waiting_for = A  (what B is blocked on)
   */
  struct thread *joined_by;    /* thread blocked waiting for this thread to terminate */
  struct thread *waiting_for;  /* thread this thread is currently blocked on */

  int priority;                /* scheduling priority (higher value = higher priority) */
  int in_ready_queue;          /* guard against double-insertion in the ready queue */
  int in_zombie_queue;         /* guard against double-insertion in the zombie queue */

  /* All threads in the same joining chain share this pointer to their common head.
   * Comparing two threads' head_joiner pointers detects membership in the same chain in O(1). */
  struct thread **head_joiner;

#ifdef ENABLE_SIGNAL
  unsigned int pending_signals; /* bitmask of signals delivered but not yet handled */
  unsigned int blocked_signals; /* bitmask of signals currently masked */
  unsigned int waited_signals;  /* bitmask of signals thread_sigwait() is waiting for */
  int waiting_for_signal;       /* 1 while blocked inside thread_sigwait() */
#endif
} thread;

/* Defined in thread.c */
extern thread *current_thread;

static inline thread *thread_get_current_thread(void) {
  return current_thread;
}

static inline void thread_set_current_thread(thread *t) {
  current_thread = t;
}

/*
 * Perform a context switch from prev to next.
 * Saves prev's registers, marks next as RUNNING, then restores next's registers.
 * Returns only when prev is scheduled again.
 */
static inline int swap_thread(thread *prev, thread *next) {
  thread_set_current_thread(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);
  return 0;
}

/* Register do_final_cleanup() via atexit() to free all remaining zombies and
 * pooled stacks at process exit. Safe to call multiple times (no-op after first). */
void thread_cleanup_register(void);

/* Push all deferred stacks back to the stack pool (or unmap if pool is full). */
void reclaim_deferred_stacks_all(void);

/* Switch execution to a static cleanup context so that do_final_cleanup() can
 * safely free the last running thread's stack. Called when no thread remains. */
void thread_switch_to_cleanup(void);

/* Add t to the zombie queue (terminated, not yet joined). */
void thread_zombie_add(thread *t);

/* Remove t from the zombie queue (joiner has collected the return value). */
void thread_zombie_remove(thread *t);

/* Return a pointer to the scheduler's ready queue. */
struct thread_queue *thread_get_ready_queue(void);

/* Pick the next thread to run according to the scheduling policy. */
thread *thread_scheduler_pick_next(void);

/* Enqueue t in the ready queue so it becomes eligible for scheduling. */
void thread_scheduler_enqueue(thread *t);

/* Set the scheduling priority of thread t. Returns 0 on success, -1 on error. */
int thread_set_priority(thread_t t, int prio);

#endif /* THREAD_INTERNAL_H */
