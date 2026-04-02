#include "thread.h"
#include "pool.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <ucontext.h>
#include <valgrind/valgrind.h>

#define THREAD_READY 0
#define THREAD_RUNNING 1
#define THREAD_TERMINATED 2
#define THREAD_BLOCKED 3

typedef struct thread {
  int id;                     // Thread ID
  ucontext_t context;         // Context for the thread
  void *(*start_fun)(void *); // Function pointer for the thread's start routine
  void *arg;                  // Argument to pass to the start routine
  TAILQ_ENTRY(thread)
  entries; // Queue entries (ready queue, zombie queue, or mutex waiting queue)
  int state;    // Thread state: READY, RUNNING, TERMINATED, BLOCKED
  void *retval; // Return value from the thread
  unsigned valgrind_stack_id;     // Valgrind stack ID for memory checking
  void *stack_map;                // Mapped memory for stack (for reuse in pool)
  struct thread_queue join_queue; // Threads waiting to join this one
} thread;

// Queue of threads that are ready to run
static struct thread_queue ready_queue;

// The main thread is initialized at the start of the program and will be used
// as the initial context for the main execution flow.
static thread main_thread = {0, .state = THREAD_RUNNING};
static thread *current_thread = &main_thread;
static int next_thread_id = 1;
static int scheduler_initialized = 0;
static int cleanup_registered = 0;

/*
 * Zombie queue — terminated threads not yet joined.
 *
 * A thread that exits with and not yet joined cannot be freed immediately: a
 * future thread_join() call must still be able to read its retval. We place it
 * here so it survives until thread_join() claims it or until free_zombies()
 * cleans it up at program exit.
 */
static struct thread_queue zombie_queue;
static int zombie_initialized = 0;

/*
 * zombie_init — initialises the zombie queue on first use.
 */
static void zombie_init(void) {
  if (!zombie_initialized) {
    TAILQ_INIT(&zombie_queue);
    zombie_initialized = 1;
  }
}

/*
 * zombie_add — adds a terminated, unjoined thread to the zombie queue.
 * Must only be called for non-main threads not yet joined.
 */
static void zombie_add(thread *t) {
  assert(t != &main_thread);
  zombie_init();
  TAILQ_INSERT_TAIL(&zombie_queue, t, entries);
}

/*
 * free_zombies — frees every zombie that has not been claimed by thread_join.
 * Also returns any pooled stacks to the pool for reuse.
 */
static void free_zombies(void) {
  if (!zombie_initialized) {
    return;
  }

  thread *t = TAILQ_FIRST(&zombie_queue);
  while (t != NULL) {
    thread *next = TAILQ_NEXT(t, entries);
    TAILQ_REMOVE(&zombie_queue, t, entries);

    // free the entire map since no other threads will be created at this point
    if (t->stack_map != NULL) {
      munmap(t->stack_map, STACK_SIZE + GUARD_SIZE); // Free the mapped memory
    }

    free(t);
    t = next;
  }
}

/*
 * Cleanup stack — used to free zombies safely from a neutral stack
 * before calling exit(0), since we cannot free the stack we are running on.
 */
static char cleanup_stack[8192];
static ucontext_t cleanup_ctx;

/*
 * do_final_cleanup — runs on cleanup_stack, never on a thread stack.
 * Frees all remaining zombies then exits the process cleanly.
 */
static void do_final_cleanup(void) {
  free_zombies();
  stack_pool_free_all();
  exit(0);
}

/*
 * switch_to_cleanup — switches execution to the neutral cleanup_stack
 * so that do_final_cleanup can safely free the current thread's stack.
 */
static void switch_to_cleanup(void) {
  getcontext(&cleanup_ctx);
  cleanup_ctx.uc_stack.ss_sp = cleanup_stack;
  cleanup_ctx.uc_stack.ss_size = sizeof(cleanup_stack);
  cleanup_ctx.uc_link = NULL;
  makecontext(&cleanup_ctx, do_final_cleanup, 0);
  setcontext(&cleanup_ctx);
  // unreachable
  exit(1);
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
thread_t thread_self(void) { return (thread_t)current_thread; }

/*
 * thread_create — creates a new thread that will execute func(funcarg).
 * Returns 0 on success, -1 on error.
 */
int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg) {
  if (!scheduler_initialized) {
    TAILQ_INIT(&ready_queue);
    TAILQ_INIT(&main_thread.join_queue);
    if (!cleanup_registered) {
      atexit(do_final_cleanup);
      cleanup_registered = 1;
    }
    // Initialize the main thread's context so it can be switched to like any
    // other thread.
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

  // Allocate or reuse stack from pool
  struct stack_entry stack_entry;
  if (stack_pool_alloc(&stack_entry) == -1) {
    free(newth);
    errno = ENOMEM;
    return -1;
  }

  newth->id = next_thread_id++;
  newth->start_fun = func;
  newth->arg = funcarg;
  newth->state = THREAD_READY;
  newth->retval = NULL;
  newth->stack_map = stack_entry.map;
  newth->valgrind_stack_id = stack_entry.valgrind_id;
  TAILQ_INIT(&newth->join_queue);

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

  TAILQ_INSERT_TAIL(&ready_queue, newth, entries);
  *newthread = (thread_t)newth;

  return 0;
}

/*
 * thread_yield — yields the CPU to another ready thread.
 * If no other thread is ready, returns immediately.
 */
int thread_yield(void) {
  thread *next = TAILQ_FIRST(&ready_queue);
  if (!next) {
    // No other thread is ready to run, so we just return and continue
    // executing the current thread.
    return 0;
  }

  thread *prev = current_thread;
  TAILQ_REMOVE(&ready_queue, next, entries);

  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    TAILQ_INSERT_TAIL(
        &ready_queue, prev,
        entries); // Put the current thread back in the ready queue
  }

  current_thread = next;
  next->state = THREAD_RUNNING;

  swapcontext(&prev->context, &next->context);
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
  current_thread->retval = retval;
  current_thread->state = THREAD_TERMINATED;

  thread *dying = current_thread;

  // Wake up all threads waiting to join this one
  struct thread_queue *jq = &dying->join_queue;
  thread *joiner;
  while ((joiner = TAILQ_FIRST(jq)) != NULL) {
    TAILQ_REMOVE(jq, joiner, entries);
    joiner->state = THREAD_READY;
    TAILQ_INSERT_TAIL(&ready_queue, joiner, entries);
  }

  thread *next = TAILQ_FIRST(&ready_queue);

  if (!next) {
    // Last thread standing — switch to cleanup_stack to free zombies safely
    if (dying != &main_thread) {
      zombie_add(dying);
    }
    switch_to_cleanup();
  }

  // Remove the next thread from the ready queue and switch to it
  TAILQ_REMOVE(&ready_queue, next, entries);
  current_thread = next;
  current_thread->state = THREAD_RUNNING;

  if (dying != &main_thread) {
    zombie_add(dying);
  }

  // Switch to the current_thread thread's context. Since the current thread is
  // terminating, we use setcontext instead of swapcontext (no need to
  // save the dying thread's context — we will never return to it).
  setcontext(&current_thread->context);

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

  // If the target is already terminated, process it immediately
  if (target->state == THREAD_TERMINATED) {
    goto cleanup;
  }

  // Target is still running: block the current thread in the target's join
  // queue
  thread *joiner = current_thread;
  struct thread_queue *jq = &target->join_queue;

  // Add joiner to target's join queue
  joiner->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(jq, joiner, entries);

  // Pick the next thread to run
  thread *next = TAILQ_FIRST(&ready_queue);
  if (next == NULL) {
    // No other thread is ready; target must finish or we deadlock
    // This is an error, so remove ourselves and fail
    TAILQ_REMOVE(jq, joiner, entries);
    joiner->state = THREAD_RUNNING;
    errno = EDEADLK;
    return -1;
  }

  // Switch to the next thread
  TAILQ_REMOVE(&ready_queue, next, entries);
  current_thread = next;
  next->state = THREAD_RUNNING;

  swapcontext(&joiner->context, &next->context);

  // When we return here, we've been woken up by target's thread_exit

cleanup:
  if (retval != NULL) {
    *retval = target->retval;
  }

  // Claim the zombie and return its stack to the pool
  if (target != &main_thread) {
    TAILQ_REMOVE(&zombie_queue, target, entries);
    // Return stack to the pool
    if (target->stack_map != NULL) {
      struct stack_entry entry = {.map = target->stack_map,
                                  .stack = target->context.uc_stack.ss_sp,
                                  .valgrind_id = target->valgrind_stack_id};
      stack_pool_push(&entry);
    }
    free(target);
  }

  return 0;
}

int thread_mutex_init(thread_mutex_t *mutex) {
  if (mutex == NULL)
    return -1;
  mutex->locked = 0;
  TAILQ_INIT(&mutex->waiting_queue);
  return 0;
}

int thread_mutex_destroy(thread_mutex_t *mutex) {
  if (mutex == NULL || !TAILQ_EMPTY(&mutex->waiting_queue)) {
    // Do not destroy a mutex if threads are still waiting on it
    return -1;
  }
  return 0;
}

/*
 * thread_mutex_lock — acquires the mutex.
 *
 * If the mutex is free: acquire it directly in O(1).
 * If the mutex is locked: park the current thread in the mutex waiting queue
 * (state = BLOCKED, not put back in ready_queue). Ownership is transferred
 * directly by thread_mutex_unlock(), so we return here already owning the
 * mutex.
 */
int thread_mutex_lock(thread_mutex_t *mutex) {
  if (mutex == NULL)
    return -1;

  if (!mutex->locked) {
    // Fast path: mutex is free, acquire it immediately
    mutex->locked = 1;
    return 0;
  }

  // Slow path: park current thread in waiting queue until unlock() wakes us.
  // We will return here with the mutex already owned (locked stays 1).
  thread *prev = current_thread;
  thread *next = TAILQ_FIRST(&ready_queue);
  if (next == NULL) {
    // No other thread can unlock the mutex — deadlock
    return -1;
  }

  TAILQ_REMOVE(&ready_queue, next,
               entries); // O(1) — TAILQ knows the predecessor via tqe_prev

  // Park current thread: BLOCKED state means thread_yield() won't pick it up
  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&mutex->waiting_queue, prev, entries);

  current_thread = next;
  next->state = THREAD_RUNNING;
  swapcontext(&prev->context, &next->context);

  // When we return here, unlock() has transferred ownership to us.
  // mutex->locked is still 1 — we are the new owner.
  return 0;
}

/*
 * thread_mutex_unlock — releases the mutex.
 *
 * If threads are waiting: transfer ownership directly to the first waiter
 * (locked stays 1, no window where another thread could steal the mutex).
 * Otherwise: release the mutex (locked = 0).
 */
int thread_mutex_unlock(thread_mutex_t *mutex) {
  if (mutex == NULL)
    return -1;
  if (!mutex->locked) {
    // Cannot unlock a mutex that is not locked
    return -1;
  }

  if (!TAILQ_EMPTY(&mutex->waiting_queue)) {
    // Transfer ownership directly: the revived thread becomes the new owner.
    // locked stays 1 — no window where another thread could steal the mutex.
    thread *revived = TAILQ_FIRST(&mutex->waiting_queue);
    TAILQ_REMOVE(&mutex->waiting_queue, revived,
                 entries); // O(1) — TAILQ knows the predecessor via tqe_prev
    revived->state = THREAD_READY;
    TAILQ_INSERT_TAIL(&ready_queue, revived, entries);
    // locked intentionally stays at 1
  } else {
    mutex->locked = 0;
  }
  return 0;
}