#include "thread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <ucontext.h>
#include <valgrind/valgrind.h>

/* =========================================================================
 * Constants & thread states
 * ========================================================================= */

#define STACK_SIZE (1024 * 1024)
#define GUARD_SIZE 4096

#define THREAD_READY      0
#define THREAD_RUNNING    1
#define THREAD_TERMINATED 2

/* =========================================================================
 * Thread structure
 * ========================================================================= */

typedef struct thread {
  int id;                    // Thread ID
  ucontext_t context;        // Context for the thread
  void *(*start_fun)(void*); // Function pointer for the thread's start routine
  void *arg;                 // Argument to pass to the start routine
  STAILQ_ENTRY(thread) entries; // Queue entries for the ready queue
  int state;                 // Thread state: READY, RUNNING, TERMINATED
  void *retval;              // Return value from the thread
  int joined;                // Flag to detect double-join
  unsigned valgrind_stack_id; // Valgrind stack ID for memory checking
} thread;

/* =========================================================================
 * Scheduler state
 * ========================================================================= */

// Queue to hold the ready threads
static struct thread_queue ready_queue;

// The main thread is initialized at the start of the program and will be used
// as the initial context for the main execution flow.
static thread  main_thread       = {0, .state = THREAD_RUNNING};
static thread *current_thread    = &main_thread;
static int     next_thread_id    = 1;
static int     scheduler_initialized = 0;

// Deferred free: a thread that has exited but whose stack we cannot free yet
// because we are still running on it. It will be freed at the next scheduler
// entry (thread_yield or thread_exit).
static thread *to_free = NULL;

/* =========================================================================
 * Cleanup stack — used to free the last unjoined thread safely from a
 * neutral stack before calling exit(0).
 * ========================================================================= */

static char      cleanup_stack[4096];
static ucontext_t cleanup_ctx;
static thread   *final_thread_to_free = NULL;

/*
 * do_final_cleanup — runs on cleanup_stack.
 * Frees the last unjoined thread then exits the process.
 */
static void do_final_cleanup(void) {
  if (final_thread_to_free != NULL) {
    VALGRIND_STACK_DEREGISTER(final_thread_to_free->valgrind_stack_id);
    void *map = (char *)final_thread_to_free->context.uc_stack.ss_sp - GUARD_SIZE;
    munmap(map, STACK_SIZE + GUARD_SIZE);
    free(final_thread_to_free);
    final_thread_to_free = NULL;
  }
  exit(0);
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/*
 * free_deferred — frees the thread stored in to_free, if any.
 * Must be called while running on a different stack than to_free's.
 */
static void free_deferred(void) {
  if (to_free != NULL) {
    VALGRIND_STACK_DEREGISTER(to_free->valgrind_stack_id);
    void *map = (char *)to_free->context.uc_stack.ss_sp - GUARD_SIZE;
    munmap(map, STACK_SIZE + GUARD_SIZE);
    free(to_free);
    to_free = NULL;
  }
}

/*
 * thread_entry — entry point for every new thread.
 * Calls the user function then exits the thread when it returns.
 */
static void thread_entry(void) {
  // Call the start function of the current thread with its argument and store the return value.
  void *retval = current_thread->start_fun(current_thread->arg);
  thread_exit(retval);
}

/* =========================================================================
 * Public API — thread lifecycle
 * ========================================================================= */

/*
 * thread_self — retrieves the identifier of the current thread.
 */
thread_t thread_self(void) { return (thread_t)current_thread; }

/*
 * thread_create — creates a new thread that will execute func(funcarg).
 * Returns 0 on success, -1 on error.
 */
int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg) {
  if (!scheduler_initialized) {
    STAILQ_INIT(&ready_queue);
    if (getcontext(&main_thread.context) == -1) {
      return -1;
    }
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

  // Allocate the stack with a guard page at the bottom to catch overflows
  void *map = mmap(NULL, STACK_SIZE + GUARD_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                   -1, 0);
  if (map == MAP_FAILED) {
    free(newth);
    return -1;
  }

  if (mprotect(map, GUARD_SIZE, PROT_NONE) == -1) {
    munmap(map, STACK_SIZE + GUARD_SIZE);
    free(newth);
    return -1;
  }

  newth->id        = next_thread_id++;
  newth->start_fun = func;
  newth->arg       = funcarg;
  newth->state     = THREAD_READY;
  newth->joined    = 0;
  newth->retval    = NULL;

  if (getcontext(&newth->context) == -1) {
    munmap(map, STACK_SIZE + GUARD_SIZE);
    free(newth);
    return -1;
  }

  void *stack_base = (char *)map + GUARD_SIZE;
  newth->context.uc_stack.ss_sp    = stack_base;
  newth->context.uc_stack.ss_size  = STACK_SIZE;
  newth->context.uc_stack.ss_flags = 0;
  newth->context.uc_link           = NULL;
  makecontext(&newth->context, thread_entry, 0);

  newth->valgrind_stack_id = VALGRIND_STACK_REGISTER(
      stack_base, (char *)stack_base + STACK_SIZE);

  STAILQ_INSERT_TAIL(&ready_queue, newth, entries);
  *newthread = (thread_t)newth;

  return 0;
}

/*
 * thread_yield — yields the CPU to another ready thread.
 * If no other thread is ready, returns immediately.
 */
int thread_yield(void) {
  // Free any thread that exited during the previous scheduling round
  free_deferred();

  thread *next = STAILQ_FIRST(&ready_queue);
  if (!next) {
    // No other thread is ready to run, so we just return and continue executing the current thread.
    return 0;
  }

  thread *prev = current_thread;
  STAILQ_REMOVE_HEAD(&ready_queue, entries); // Remove the next thread from the ready queue

  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    STAILQ_INSERT_TAIL(&ready_queue, prev, entries); // Put the current thread back in the ready queue if it's still running
  }

  current_thread = next;
  next->state    = THREAD_RUNNING;

  swapcontext(&prev->context, &next->context);
  return 0;
}

/*
 * thread_exit — terminates the current thread with the given return value.
 * This function never returns.
 *
 * Memory management strategy for unjoined threads:
 *   - If another thread is waiting in the ready queue, defer the free so
 *     the next thread handles it (we cannot free our own live stack).
 *   - If this is the last thread, switch to a neutral cleanup_stack first,
 *     then free safely before calling exit(0).
 */
void thread_exit(void *retval) {
  current_thread->retval = retval;
  current_thread->state  = THREAD_TERMINATED;

  thread *dying = current_thread;

  // Free any thread that was deferred from the previous round
  free_deferred();

  // Find the next thread to run
  thread *next = STAILQ_FIRST(&ready_queue);

  if (!next) {
    // No other thread is ready to run.
    // If this thread is unjoined we must free it, but we cannot do so while
    // still running on its stack — switch to the neutral cleanup_stack first.
    if (dying != &main_thread && dying->joined == 0) {
      final_thread_to_free = dying;
      getcontext(&cleanup_ctx);
      cleanup_ctx.uc_stack.ss_sp   = cleanup_stack;
      cleanup_ctx.uc_stack.ss_size = sizeof(cleanup_stack);
      cleanup_ctx.uc_link          = NULL;
      makecontext(&cleanup_ctx, do_final_cleanup, 0);
      setcontext(&cleanup_ctx);
    }
    exit(0);
  }

  // Remove the next thread from the ready queue and switch to it
  STAILQ_REMOVE_HEAD(&ready_queue, entries);
  current_thread = next;
  next->state    = THREAD_RUNNING;

  // Mark dying thread for deferred free if nobody will join it
  if (dying != &main_thread && dying->joined == 0) {
    to_free = dying;
  }

  // Switch to the next thread's context. Since the current thread is terminating,
  // we use setcontext instead of swapcontext to avoid saving the dying thread's context (which we won't return to).
  setcontext(&next->context);

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

  // Protection against double-join: if the thread has already been joined, we return an error.
  if (target->joined) {
    errno = EINVAL;
    return -1;
  }
  target->joined = 1;

  // Wait for the target thread to terminate
  while (target->state != THREAD_TERMINATED) {
    thread_yield();
  }

  if (retval != NULL) {
    *retval = target->retval;
  }

  // Free the stack and structure of the thread (except main)
  if (target != &main_thread) {
    VALGRIND_STACK_DEREGISTER(target->valgrind_stack_id);
    void *map = (char *)target->context.uc_stack.ss_sp - GUARD_SIZE;
    munmap(map, STACK_SIZE + GUARD_SIZE);
    free(target);
  }

  return 0;
}

/* =========================================================================
 * Public API — mutex
 * ========================================================================= */

int thread_mutex_init(thread_mutex_t *mutex) {
  if (mutex == NULL) return -1;
  mutex->locked = 0;
  STAILQ_INIT(&mutex->waiting_queue);
  return 0;
}

int thread_mutex_destroy(thread_mutex_t *mutex) {
  if (mutex == NULL || !STAILQ_EMPTY(&mutex->waiting_queue)) {
    return -1; // On ne détruit pas un mutex si des threads attendent
  }
  return 0;
}

int thread_mutex_lock(thread_mutex_t *mutex) {
  if (mutex == NULL) return -1;

  while (mutex->locked) {
    thread *prev = current_thread;
    thread *next = STAILQ_FIRST(&ready_queue);
    if (next == NULL) {
      return -1; // Deadlock potentiel
    }
    // Move current thread to the mutex waiting queue
    STAILQ_REMOVE_HEAD(&ready_queue, entries);
    STAILQ_INSERT_TAIL(&mutex->waiting_queue, prev, entries);
    current_thread = next;
    next->state    = THREAD_RUNNING;
    swapcontext(&prev->context, &next->context);
  }
  mutex->locked = 1;
  return 0;
}

int thread_mutex_unlock(thread_mutex_t *mutex) {
  if (mutex == NULL) return -1;
  mutex->locked = 0;
  // Wake up the first thread waiting on this mutex, if any
  if (!STAILQ_EMPTY(&mutex->waiting_queue)) {
    thread *revived = STAILQ_FIRST(&mutex->waiting_queue);
    STAILQ_REMOVE_HEAD(&mutex->waiting_queue, entries);
    revived->state = THREAD_READY;
    STAILQ_INSERT_TAIL(&ready_queue, revived, entries);
  }
  return 0;
}