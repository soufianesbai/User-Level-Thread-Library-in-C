#ifndef THREAD_INTERNAL_H
#define THREAD_INTERNAL_H

#include "context.h"
#include "pool.h"
#include "thread.h"

<<<<<<< HEAD
#ifdef THREAD_MULTICORE
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL
#endif

#define THREAD_READY 0
#define THREAD_RUNNING 1
=======
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
>>>>>>> 96588ff (full include documentation)
#define THREAD_TERMINATED 2
#define THREAD_BLOCKED    3

typedef struct thread {
<<<<<<< HEAD
  int id;                      // Thread ID
  struct fast_ctx context;     // Saved registers (64 bytes, no signal-mask overhead)
  void *(*start_fun)(void *);  // Function pointer for the thread's start routine
  void *arg;                   // Argument to pass to the start routine
  TAILQ_ENTRY(thread) entries; // Queue entries for ready and join queues
  int state;                   // Thread state: READY, RUNNING, TERMINATED, BLOCKED
  void *retval;                // Return value from the thread
  unsigned valgrind_stack_id;  // Valgrind stack ID for memory checking
  void *stack_map;             // Mapped memory for stack (for munmap)
  void *stack_base;            // Usable stack area start (= stack_map + GUARD_SIZE)
  struct thread *joined_by;    // The thread that is joining on this thread (if any)
  struct thread *waiting_for;  // The thread that is waiting for this thread to terminate (if any)
  int priority;                // Thread priority for scheduling
  int in_ready_queue;          // 1 when in ready queue, 0 otherwise
  int in_zombie_queue;         // 1 when in zombie queue, 0 otherwise
  int affinity;                // Worker affinity (-1 = any worker)
  struct thread **head_joiner; // Shared reference to the head of the joining chain
=======
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

<<<<<<< HEAD
>>>>>>> 96588ff (full include documentation)
=======
  /* Set to 1 if this thread was terminated by a stack overflow (guard page fault).
   * thread_join() returns EFAULT when this flag is set. */
  int stack_overflow;

>>>>>>> b479ed7 (fix bugs, implement stack overflow detection, add signal test, clean up Makefile)
#ifdef ENABLE_SIGNAL
  unsigned int pending_signals; /* bitmask of signals delivered but not yet handled */
  unsigned int blocked_signals; /* bitmask of signals currently masked */
  unsigned int waited_signals;  /* bitmask of signals thread_sigwait() is waiting for */
  int waiting_for_signal;       /* 1 while blocked inside thread_sigwait() */
#endif
} thread;

<<<<<<< HEAD
/* current_thread is defined in thread.c and used here. Provide small
 * inline accessors so callers get zero-cost access while keeping a single
 * canonical symbol for the variable in `thread.c`. */
=======
/* Defined in thread.c */
>>>>>>> 96588ff (full include documentation)
extern thread *current_thread;

static inline thread *thread_get_current(void) { return current_thread; }
static inline void thread_set_current(thread *t) { current_thread = t; }

/*
 * Perform a context switch from prev to next.
 * Saves prev's registers, marks next as RUNNING, then restores next's registers.
 * Returns only when prev is scheduled again.
 */
static inline int swap_thread(thread *prev, thread *next) {
  thread_set_current(next);
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
<<<<<<< HEAD
thread *thread_scheduler_pick_next_locked(void);
void thread_scheduler_enqueue(thread *t);
void thread_scheduler_enqueue_locked(thread *t);
=======

/* Enqueue t in the ready queue so it becomes eligible for scheduling. */
void thread_scheduler_enqueue(thread *t);

/* Set the scheduling priority of thread t. Returns 0 on success, -1 on error. */
>>>>>>> 96588ff (full include documentation)
int thread_set_priority(thread_t t, int prio);
int thread_set_concurrency(int nworkers);
int thread_set_affinity(thread_t thread, int worker_id);

void thread_scheduler_sync_init(void);
void thread_scheduler_sync_shutdown(void);
int thread_scheduler_has_workers(void);
thread *thread_scheduler_get_worker_stub(void);
void thread_scheduler_wake_workers(void);

#endif /* THREAD_INTERNAL_H */
