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
  int id;                       // Thread ID
  ucontext_t context;           // Context for the thread
  void *(*start_fun)(void *);   // Function pointer for the thread's start routine
  void *arg;                    // Argument to pass to the start routine
  STAILQ_ENTRY(thread) entries; // Queue entries (ready queue or zombie queue)
  int state;                    // Thread state: READY, RUNNING, TERMINATED
  void *retval;                 // Return value from the thread
  int joined;                   // 1 if thread_join() has claimed this thread
  unsigned valgrind_stack_id;   // Valgrind stack ID for memory checking
} thread;

/* =========================================================================
 * Scheduler state
 * ========================================================================= */

// Queue of threads that are ready to run
static struct thread_queue ready_queue;

// The main thread is initialized at the start of the program and will be used
// as the initial context for the main execution flow.
static thread  main_thread           = {0, .state = THREAD_RUNNING};
static thread *current_thread        = &main_thread;
static int     next_thread_id        = 1;
static int     scheduler_initialized = 0;

/* =========================================================================
 * Zombie queue — terminated threads not yet joined
 *
 * When a thread exits without being joined, we cannot free its stack
 * immediately (we are still running on it). Instead we place it in this
 * queue. It will be freed either:
 *   - by free_zombies() when the program is about to exit, or
 *   - never freed by free_zombies() if thread_join() claimed it first
 *     (joined=1 is the synchronisation flag between the two paths).
 * ========================================================================= */

static struct thread_queue zombie_queue;
static int zombie_initialized = 0;

/*
 * zombie_init — lazily initialises the zombie queue on first use.
 */
static void zombie_init(void) {
  if (!zombie_initialized) {
    STAILQ_INIT(&zombie_queue);
    zombie_initialized = 1;
  }
}

/*
 * zombie_add — adds a terminated, unjoined thread to the zombie queue.
 * Must only be called for non-main threads with joined == 0.
 */
static void zombie_add(thread *t) {
  zombie_init();
  STAILQ_INSERT_TAIL(&zombie_queue, t, entries);
}

/*
 * free_zombies — frees every zombie that has not been claimed by thread_join.
 * Safe to call at any time from any stack, because we never free the
 * current thread here (it is RUNNING, not TERMINATED).
 */
static void free_zombies(void) {
  if (!zombie_initialized) return;

  thread *t = STAILQ_FIRST(&zombie_queue);
  while (t != NULL) {
    thread *next = STAILQ_NEXT(t, entries);
    if (t->joined == 0) {
      // Nobody joined this thread — free it now
      STAILQ_REMOVE(&zombie_queue, t, thread, entries);
      VALGRIND_STACK_DEREGISTER(t->valgrind_stack_id);
      void *map = (char *)t->context.uc_stack.ss_sp - GUARD_SIZE;
      munmap(map, STACK_SIZE + GUARD_SIZE);
      free(t);
    }
    // If joined == 1, thread_join() already freed it or will do so — skip
    t = next;
  }
}

/* =========================================================================
 * Cleanup stack — used to free the last zombie safely from a neutral stack
 * before calling exit(0), since we cannot free the stack we are running on.
 * ========================================================================= */

static char       cleanup_stack[8192];
static ucontext_t cleanup_ctx;

/*
 * do_final_cleanup — runs on cleanup_stack, never on a thread stack.
 * Frees all remaining zombies then exits the process cleanly.
 */
static void do_final_cleanup(void) {
  free_zombies();
  exit(0);
}

/*
 * switch_to_cleanup — switches execution to the neutral cleanup_stack
 * so that do_final_cleanup can safely free the current thread's stack.
 */
static void switch_to_cleanup(void) {
  getcontext(&cleanup_ctx);
  cleanup_ctx.uc_stack.ss_sp   = cleanup_stack;
  cleanup_ctx.uc_stack.ss_size = sizeof(cleanup_stack);
  cleanup_ctx.uc_link          = NULL;
  makecontext(&cleanup_ctx, do_final_cleanup, 0);
  setcontext(&cleanup_ctx);
  // unreachable
  exit(1);
}

/* =========================================================================
 * Internal helper — thread entry point
 * ========================================================================= */

/*
 * thread_entry — entry point for every new thread.
 * Calls the user function then exits the thread when it returns.
 */
static void thread_entry(void) {
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
  thread *next = STAILQ_FIRST(&ready_queue);
  if (!next) {
    // No other thread is ready to run, so we just return and continue
    // executing the current thread.
    return 0;
  }

  thread *prev = current_thread;
  STAILQ_REMOVE_HEAD(&ready_queue, entries); // Remove the next thread from the ready queue

  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    STAILQ_INSERT_TAIL(&ready_queue, prev, entries); // Put the current thread back in the ready queue
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
 * Memory management strategy:
 *   - We cannot free our own stack while running on it.
 *   - If the thread was not joined, we add it to the zombie queue.
 *     free_zombies() will clean it up later (at program exit), unless
 *     thread_join() claims it first (joined=1 prevents the free).
 *   - If this is the last thread, we switch to a neutral cleanup_stack
 *     before calling free_zombies() + exit(0).
 */
void thread_exit(void *retval) {
  current_thread->retval = retval;
  current_thread->state  = THREAD_TERMINATED;

  thread *dying = current_thread;

  // If nobody joined this thread, add it to the zombie queue for later cleanup.
  // joined == 0 here means thread_join() has not claimed it yet.
  // thread_join() sets joined = 1 before doing anything else, so this flag
  // is the safe synchronisation point between the two code paths.
  if (dying != &main_thread && dying->joined == 0) {
    zombie_add(dying);
  }

  // Find the next thread to run
  thread *next = STAILQ_FIRST(&ready_queue);

  if (!next) {
    // No other thread is ready to run — we are the last one.
    // Switch to the neutral cleanup_stack so we can safely free all zombies
    // (including possibly ourselves) without touching a dead stack.
    switch_to_cleanup();
    // unreachable
  }

  // Remove the next thread from the ready queue and switch to it
  STAILQ_REMOVE_HEAD(&ready_queue, entries);
  current_thread = next;
  next->state    = THREAD_RUNNING;

  // Switch to the next thread's context. Since the current thread is
  // terminating, we use setcontext instead of swapcontext (no need to
  // save the dying thread's context — we will never return to it).
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

  // Protection against double-join
  if (target->joined) {
    errno = EINVAL;
    return -1;
  }

  // Claim the thread BEFORE yielding so that free_zombies() (called from
  // do_final_cleanup) will not free it under our feet.
  target->joined = 1;

  // Wait for the target thread to terminate
  while (target->state != THREAD_TERMINATED) {
    thread_yield();
  }

  if (retval != NULL) {
    *retval = target->retval;
  }

  // Free the stack and structure of the thread.
  // main_thread is statically allocated — never free it.
  if (target != &main_thread) {
    // Remove from the zombie queue if it was added before we claimed it
    if (zombie_initialized) {
      STAILQ_REMOVE(&zombie_queue, target, thread, entries);
    }
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
    // Do not destroy a mutex if threads are still waiting on it
    return -1;
  }
  return 0;
}

int thread_mutex_lock(thread_mutex_t *mutex) {
  if (mutex == NULL) return -1;

  while (mutex->locked) {
    thread *prev = current_thread;
    thread *next = STAILQ_FIRST(&ready_queue);
    if (next == NULL) {
      // No other thread can unlock the mutex — deadlock
      return -1;
    }
    // Park the current thread in the mutex waiting queue
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