#include "thread.h"
#include "thread_internal.h"
#include "preemption.h"
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
static thread main_thread = {0, .state = THREAD_RUNNING, .joined_by = NULL};
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
  // utile to avoid duplicated code
  TAILQ_REMOVE(&ready_queue, next, entries);
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
  thread_yield();
  // in_preemption_handler = 0;
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
  if (!scheduler_initialized) {
    TAILQ_INIT(&ready_queue);
    thread_cleanup_register();
    // Initialize the main thread's context so it can be switched to like any
    // other thread.
    if (getcontext(&main_thread.context) == -1) {
      return -1;
    }

    init_prem(preemption_handler, 5);
    scheduler_initialized = 1;
  }

  if (newthread == NULL || func == NULL) {
    errno = EINVAL; // Invalid argument
    return -1;
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

  preem_block(); // Keep the critical section small: only shared scheduler state
  newth->id = next_thread_id++;
  TAILQ_INSERT_TAIL(&ready_queue, newth, entries);
  *newthread = (thread_t)newth;

  preem_unblock();
  return 0;
}

/*
 * thread_yield — yields the CPU to another ready thread.
 * If no other thread is ready, returns immediately.
 */
int thread_yield(void) {
  preem_block(); // Block first to avoid races with asynchronous preemption

  thread *next = TAILQ_FIRST(&ready_queue);
  if (!next) {
    // No other thread is ready to run, so we just return and continue
    // executing the current thread.
    if (!in_preemption_handler) {
      preem_unblock();
    }
    return 0;
  }

  thread *prev = current_thread;

  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    TAILQ_INSERT_TAIL(&ready_queue,
                      prev,
                      entries); // Put the current thread back in the ready queue
  }

  swap_thread(prev, next);
  // current_thread = next;
  // next->state = THREAD_RUNNING;

  // swapcontext(&prev->context, &next->context);
  if (!in_preemption_handler) {
    preem_unblock();
  }
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

  preem_block(); // Block first to avoid races with asynchronous preemption

  thread *target = (thread *)target_handle;

  // If the target thread is not ready, fallback to a normal yield
  if (target->state != THREAD_READY) {
    preem_unblock();
    return thread_yield();
  }

  thread *prev = current_thread;
  TAILQ_INSERT_TAIL(&ready_queue, prev, entries);

  swap_thread(prev, target);

  preem_unblock();
  return 0;
}

/*
 * thread_exit — terminates the current thread with the given return value.
 * This function never returns.
 *
 * Memory management strategy:
 *   - not yet joined at exit time: a future thread_join() may still need
 * retval. We add to the zombie queue so the struct survives until thread_join()
 * claims it or free_zombies() cleans it at program exit.
 *   - Last thread standing: switch to the neutral cleanup_stack so that
 *     do_final_cleanup() can safely free any remaining zombies.
 */
void thread_exit(void *retval) {
  preem_block(); // Block preemption while exiting the thread
  current_thread->retval = retval;
  current_thread->state = THREAD_TERMINATED;

  thread *dying = current_thread;
  if (dying != &main_thread) {
    thread_zombie_add(dying);
  }


  thread *next = TAILQ_FIRST(&ready_queue);

  if(dying->joined_by != NULL){
    next = dying->joined_by;
  }
  if (!next) {
    thread_switch_to_cleanup();
  }
  

  if(next->state == THREAD_READY){
    TAILQ_REMOVE(&ready_queue, next, entries);
  }
  // Remove the next thread from the ready queue and switch to it
  // ps : thres a case where next is blocked, and not in the queue. this code still works
  current_thread = next;
  current_thread->state = THREAD_RUNNING;

  // Switch to the current_thread thread's context. Since the current thread is
  // terminating, we use setcontext instead of swapcontext (no need to
  // save the dying thread's context — we will never return to it).
  setcontext(&current_thread->context);

  // no need for unblocking preemption here since we are exiting the thread and will never return to it
  // If setcontext returns, it failed. Exit with error.
  exit(1);
}

/*
 * thread_join — waits for the given thread to terminate.
 * Places the thread's return value in *retval (if non-NULL).
 * Returns 0 on success, -1 on error.
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

  // Deadlock check: current thread cannot join a thread that it is already
  // waiting on (directly or transitively).
  thread *cursor = current_thread;
  while (cursor != NULL) {
    if (cursor == target) {
      errno = EDEADLK;
      return EDEADLK;
    }
    cursor = cursor->joined_by;
  }

  preem_block(); // Keep masked section focused on state/queue mutations

  // Mark the current thread as the joiner of the target thread
  target->joined_by = current_thread;

  // If the target is already terminated, process it immediately
  if (target->state != THREAD_TERMINATED) {
    current_thread->state = THREAD_BLOCKED;

    // Park the current thread until the target terminates.
    // We never switch directly to target here because it may not be READY.

    while (target->state != THREAD_TERMINATED) {
      if(target->state == THREAD_READY){
        swap_thread(current_thread, target);
        continue;
      }

      thread *prev = current_thread;
      thread *next = TAILQ_FIRST(&ready_queue);

      if (next == NULL) {
        current_thread->state = THREAD_RUNNING;
        preem_unblock();
        errno = EDEADLK;
        return -1;
      }

      swap_thread(prev, next);
    }

    current_thread->state = THREAD_RUNNING;
  }
  preem_unblock();

  if (retval != NULL) {
    *retval = target->retval;
  }

  // Claim the zombie and return its stack to the pool
  preem_block();
  if (target != &main_thread) {
    thread_zombie_remove(target);
    // Return stack to the pool
    if (target->stack_map != NULL) {
      struct stack_entry entry = {.map = target->stack_map,
                                  .stack = target->context.uc_stack.ss_sp,
                                  .valgrind_id = target->valgrind_stack_id};
      stack_pool_push(&entry);
    }
    free(target);
  }
  preem_unblock();


  return 0;
}