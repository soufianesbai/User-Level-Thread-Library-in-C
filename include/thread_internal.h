#ifndef THREAD_INTERNAL_H
#define THREAD_INTERNAL_H

#include "context.h"
#include "pool.h"
#include "thread.h"

#ifdef THREAD_MULTICORE
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL
#endif

#define THREAD_READY 0
#define THREAD_RUNNING 1
#define THREAD_TERMINATED 2
#define THREAD_BLOCKED 3

typedef struct thread {
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
#ifdef ENABLE_SIGNAL
  unsigned int pending_signals; // Bitmask of pending internal signals
  unsigned int blocked_signals; // Bitmask of masked internal signals
  unsigned int waited_signals;  // Bitmask used by thread_sigwait()
  int waiting_for_signal;       // 1 when blocked inside thread_sigwait()
#endif
} thread;

/* current_thread is defined in thread.c and used here. Provide small
 * inline accessors so callers get zero-cost access while keeping a single
 * canonical symbol for the variable in `thread.c`. */
extern THREAD_LOCAL thread *current_thread;

static inline thread *thread_get_current(void) { return current_thread; }
static inline void thread_set_current(thread *t) { current_thread = t; }

static inline int swap_thread(thread *prev, thread *next) {
  thread_set_current(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);
  return 0;
}

void thread_cleanup_register(void);
void reclaim_deferred_stacks_all(void);
void thread_switch_to_cleanup(void);
void thread_zombie_add(thread *t);
void thread_zombie_remove(thread *t);
struct thread_queue *thread_get_ready_queue(void);
thread *thread_scheduler_pick_next(void);
thread *thread_scheduler_pick_next_locked(void);
void thread_scheduler_enqueue(thread *t);
void thread_scheduler_enqueue_locked(thread *t);
int thread_set_priority(thread_t t, int prio);
int thread_set_concurrency(int nworkers);
int thread_set_affinity(thread_t thread, int worker_id);

void thread_scheduler_sync_init(void);
void thread_scheduler_sync_shutdown(void);
int thread_scheduler_has_workers(void);
thread *thread_scheduler_get_worker_stub(void);
void thread_scheduler_wake_workers(void);

#endif // THREAD_INTERNAL_H
