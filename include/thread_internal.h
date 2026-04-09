#ifndef THREAD_INTERNAL_H
#define THREAD_INTERNAL_H

#include "pool.h"
#include "thread.h"
#include <ucontext.h>

#define THREAD_READY 0
#define THREAD_RUNNING 1
#define THREAD_TERMINATED 2
#define THREAD_BLOCKED 3

typedef struct thread {
  int id;                      // Thread ID
  ucontext_t context;          // Context for the thread
  void *(*start_fun)(void *);  // Function pointer for the thread's start routine
  void *arg;                   // Argument to pass to the start routine
  TAILQ_ENTRY(thread) entries; // Queue entries for ready and join queues
  int state;                   // Thread state: READY, RUNNING, TERMINATED, BLOCKED
  void *retval;                // Return value from the thread
  unsigned valgrind_stack_id;  // Valgrind stack ID for memory checking
  void *stack_map;             // Mapped memory for stack (for reuse in pool)
  struct thread *joined_by;    // The thread that is joining on this thread (if any)
  struct thread *waiting_for;  // The thread that is waiting for this thread to terminate (if any)
  int priority;                // Thread priority for scheduling
  int in_ready_queue; // Flag to indicate if the thread is currently in the ready queue (for
                      // debugging)
} thread;

void thread_cleanup_register(void);
void thread_switch_to_cleanup(void);
void thread_zombie_add(thread *t);
void thread_zombie_remove(thread *t);
struct thread_queue *thread_get_ready_queue(void);
thread *thread_get_current_thread(void);
void thread_set_current_thread(thread *t);
int swap_thread(thread *prev, thread *next);
thread *thread_scheduler_pick_next(void);
void thread_scheduler_enqueue(thread *t);
int thread_set_priority(thread_t t, int prio);

#endif // THREAD_INTERNAL_H
