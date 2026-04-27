#ifndef THREAD_INTERNAL_H
#define THREAD_INTERNAL_H

#include "context.h"
#include "pool.h"
#include "thread.h"

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
  struct thread **head_joiner; // Shared reference to the head of the joining chain
#ifdef ENABLE_SIGNAL
  unsigned int pending_signals; // Bitmask of pending internal signals
  unsigned int blocked_signals; // Bitmask of masked internal signals
  unsigned int waited_signals;  // Bitmask used by thread_sigwait()
  int waiting_for_signal;       // 1 when blocked inside thread_sigwait()
#endif
} thread;

/* current_thread is defined in thread.c and inlined here for zero-cost access. */
extern thread *current_thread;

static inline thread *thread_get_current_thread(void) {
  return current_thread;
}

static inline void thread_set_current_thread(thread *t) {
  current_thread = t;
}

static inline int swap_thread(thread *prev, thread *next) {
  thread_set_current_thread(next);
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
void thread_scheduler_enqueue(thread *t);
int thread_set_priority(thread_t t, int prio);

#endif // THREAD_INTERNAL_H
