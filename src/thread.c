#include "thread.h"
#include "preemption.h"
#include "thread_internal.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <ucontext.h>
#include <valgrind/valgrind.h>

// Queue of threads that are ready to run
static struct thread_queue ready_queue;

// The main thread is initialized at the start of the program and will be used
// as the initial context for the main execution flow.
static thread main_thread = {0, .state = THREAD_RUNNING, .joined_by = NULL, .waiting_for = NULL};
static thread *current_thread = &main_thread;
static int next_thread_id = 1;
static int scheduler_initialized = 0;

struct thread_queue *thread_get_ready_queue(void) {
  return &ready_queue;
}

thread *thread_get_current_thread(void) {
  return current_thread;
}

void thread_set_current_thread(thread *t) {
  current_thread = t;
}

/*
  set current_thread to next as THREAD_RUNNING and switch context from prev to next.
*/
int swap_thread(thread *prev, thread *next) {
  if (next->state == THREAD_READY) {
    TAILQ_REMOVE(&ready_queue, next, entries);
  }
  current_thread = next;
  next->state = THREAD_RUNNING;
  return swapcontext(&prev->context, &next->context);
}

/*
  a wrapper for the preemption signal handler that just yields the current thread.
  sigaction take an func(int) not a func(void)
*/
void preemption_handler(int sig) {
  (void)sig;

  // Guard against re-entrant signals: if a SIGALRM fires while we are
  // already inside this handler, ignore it.
  static volatile sig_atomic_t in_handler = 0;
  if (in_handler)
    return;
  in_handler = 1;

  thread_yield();

  in_handler = 0;
}

/*
 * thread_entry — entry point for every new thread.
 * Calls the user function then exits the thread when it returns.
 */
static void thread_entry(void) {
  void *retval = current_thread->start_fun(current_thread->arg);
  thread_exit(retval);
}

/*
 * thread_self — retrieves the identifier of the current thread.
 */
thread_t thread_self(void) {
  return (thread_t)current_thread;
}

/*
 * thread_create — creates a new thread that will execute func(funcarg).
 * Returns 0 on success, -1 on error.
 */
int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg) {
  if (newthread == NULL || func == NULL) {
    errno = EINVAL; // Invalid argument
    return -1;
  }

  if (!scheduler_initialized) {
    TAILQ_INIT(&ready_queue);
    thread_cleanup_register();
    // Initialize the main thread's context so it can be switched to like any
    // other thread.
    if (getcontext(&main_thread.context) == -1) {
      return -1;
    }
#ifdef ENABLE_PREEMPTION
    init_prem(preemption_handler, 5);
#endif
    scheduler_initialized = 1;
  }

  thread *newth = malloc(sizeof(*newth));
  if (newth == NULL) {
    errno = ENOMEM; // Out of memory
    return -1;
  }

  // Allocate or reuse stack from pool
  struct stack_entry stack_entry;
  if (stack_pool_alloc(&stack_entry) == -1) {
    free(newth);
    errno = ENOMEM;
    return -1;
  }

  newth->start_fun = func;
  newth->arg = funcarg;
  newth->state = THREAD_READY;
  newth->retval = NULL;
  newth->stack_map = stack_entry.map;
  newth->valgrind_stack_id = stack_entry.valgrind_id;
  newth->joined_by = NULL;
  newth->waiting_for = NULL;

  if (getcontext(&newth->context) == -1) {
    free(newth);
    stack_pool_push(&stack_entry);
    return -1;
  }

  newth->context.uc_stack.ss_sp = stack_entry.stack;
  newth->context.uc_stack.ss_size = STACK_SIZE;
  newth->context.uc_stack.ss_flags = 0;
  newth->context.uc_link = NULL;
  makecontext(&newth->context, thread_entry, 0);

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif
  // Keep the critical section small: only shared scheduler state
  newth->id = next_thread_id++;
  TAILQ_INSERT_TAIL(&ready_queue, newth, entries);
  *newthread = (thread_t)newth;

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_yield — yields the CPU to another ready thread.
 * If no other thread is ready, returns immediately.
 */
int thread_yield(void) {

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif
  // Block first to avoid races with asynchronous preemption

  thread *next = TAILQ_FIRST(&ready_queue);
  if (!next) {
// No other thread is ready to run, so we just return and continue
// executing the current thread.
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }

  thread *prev = current_thread;

  // prev must be either RUNNING (normal yield) or BLOCKED (called from
  // thread_join). Any other state indicates a scheduler bug.
  assert(prev->state == THREAD_RUNNING || prev->state == THREAD_BLOCKED);

  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    // Put the current thread back in the ready queue
    TAILQ_INSERT_TAIL(&ready_queue, prev, entries);
  }

  swap_thread(prev, next);

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_yield_to — yields execution to the specified target thread, if it is
 * ready. If the target thread is not ready, falls back to a normal yield.
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

  // If the target thread is not ready, fallback to a normal yield
  if (target->state != THREAD_READY) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return thread_yield();
  }

  thread *prev = current_thread;
  prev->state = THREAD_READY;
  TAILQ_INSERT_TAIL(&ready_queue, prev, entries);

  swap_thread(prev, target);

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_exit — terminates the current thread with the given return value.
 * This function never returns.
 *
 * Memory management strategy:
 *   - not yet joined at exit time: a future thread_join() may still need
 *     retval. We add to the zombie queue so the struct survives until
 *     thread_join() claims it or free_zombies() cleans it at program exit.
 *   - Last thread standing: switch to the neutral cleanup_stack so that
 *     do_final_cleanup() can safely free any remaining zombies.
 *
 * Wakeup strategy:
 *   - If another thread is blocked in thread_join() waiting for us,
 *     we put it back in the ready queue before switching away.
 *     This guarantees the joiner is woken up exactly once, by us,
 *     eliminating the need for a polling loop in thread_join().
 */
void thread_exit(void *retval) {
#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  current_thread->retval = retval;
  current_thread->state = THREAD_TERMINATED;

  thread *dying = current_thread;
  // Non-main threads go to the zombie queue so thread_join() can
  // still read their retval. The main thread's struct is static —
  // it never needs to be freed, so we skip zombification.
  if (dying != &main_thread) {
    thread_zombie_add(dying);
  }

  // If a thread was waiting on this one, unblock it now
  // by putting it back in the ready queue.
  if (dying->joined_by != NULL) {
    thread *joiner = dying->joined_by;
    joiner->state = THREAD_READY;
    TAILQ_INSERT_TAIL(&ready_queue, joiner, entries);
  }

  // Pick the next thread normally via FIFO — no special case needed.
  // The joiner (if any) is already in the queue at this point.
  thread *next = TAILQ_FIRST(&ready_queue);

  if (next == NULL) {
    thread_switch_to_cleanup();
  }

  TAILQ_REMOVE(&ready_queue, next, entries);
  current_thread = next;
  current_thread->state = THREAD_RUNNING;
  setcontext(&current_thread->context);

  exit(1);
}

/*
 * thread_join — waits for the given thread to terminate.
 * Places the thread's return value in *retval (if non-NULL).
 * Returns 0 on success, -1 on error.
 *
 * Blocking strategy:
 *   - If the target is not yet terminated, the current thread is marked
 *     BLOCKED and yields exactly once. It will be re-inserted into the
 *     ready queue by thread_exit() when the target terminates.
 *   - This guarantees the joiner wakes up exactly once, with no polling.
 *
 * Deadlock detection:
 *   - Follows the waiting_for chain from target. If current_thread is
 *     reached, joining would create a cycle → EDEADLK.
 *   - O(cycle length), correct for cycles of any length.
 */
int thread_join(thread_t thread_handle, void **retval) {
  if (thread_handle == NULL) {
    errno = EINVAL;
    return -1;
  }

  thread *target = (thread *)thread_handle;

  // Multiple join check: a thread can only be joined by one other thread
  if (target->joined_by != NULL) {
    errno = EINVAL;
    return -1;
  }

  // Deadlock check: follow waiting_for from target.
  // If we reach current_thread, joining would create a cycle.
  thread *cursor = target;
  while (cursor != NULL) {
    if (cursor == current_thread) {
      errno = EDEADLK;
      return EDEADLK;
    }
    cursor = cursor->waiting_for;
  }

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  // Mark the current thread as the joiner of the target
  target->joined_by = current_thread;

  if (target->state != THREAD_TERMINATED) {
    current_thread->state = THREAD_BLOCKED;
    current_thread->waiting_for = target;

    thread *prev = current_thread;
    thread *next = TAILQ_FIRST(&ready_queue);

    if (next == NULL) {
      // Nobody else to run → real deadlock
      current_thread->state = THREAD_RUNNING;
      current_thread->waiting_for = NULL;
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      errno = EDEADLK;
      return -1;
    }

    // Yield exactly once. We will return here only when thread_exit()
    // of target puts us back into the ready queue.
    swap_thread(prev, next);

    // At this point, target->state == THREAD_TERMINATED is guaranteed
    // by the wakeup invariant in thread_exit().
    current_thread->waiting_for = NULL;
    current_thread->state = THREAD_RUNNING;
  }

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  if (retval != NULL) {
    *retval = target->retval;
  }

  // Claim the zombie and return its stack to the pool
#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  if (target != &main_thread) {
    thread_zombie_remove(target);
    if (target->stack_map != NULL) {
      struct stack_entry entry = {.map = target->stack_map,
                                  .stack = target->context.uc_stack.ss_sp,
                                  .valgrind_id = target->valgrind_stack_id};
      stack_pool_push(&entry);
    }
    free(target);
  }

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  return 0;
}