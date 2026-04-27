#include "thread_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h> /* needed for cleanup_ctx (ucontext_t) */

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
 * Cleanup stack — used to free zombies safely from a neutral stack
 * before calling exit(0), since we cannot free the stack we are running on.
 */
static char cleanup_stack[8192];
static ucontext_t cleanup_ctx;

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
void thread_zombie_add(thread *t) {
  zombie_init();
  t->in_zombie_queue = 1;
  TAILQ_INSERT_TAIL(&zombie_queue, t, entries);
}

/*
 * zombie_remove — removes a zombie thread from the zombie queue.
 * Safe to call even if the thread was never zombified (in_zombie_queue == 0).
 */
void thread_zombie_remove(thread *t) {
  if (!t->in_zombie_queue) {
    return;
  }
  t->in_zombie_queue = 0;
  TAILQ_REMOVE(&zombie_queue, t, entries);
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
      munmap(t->stack_map, STACK_SIZE + GUARD_SIZE);
    }

    free(t);
    t = next;
  }
}

/*
 * do_final_cleanup — runs on cleanup_stack, never on a thread stack.
 * Frees all remaining zombies then exits the process cleanly.
 */
static void do_final_cleanup(void) {
  free_zombies();
  stack_pool_free_all();
}

/*
 * thread_cleanup_register — registers final cleanup exactly once.
 */
void thread_cleanup_register(void) {
  static int cleanup_registered = 0;

  if (!cleanup_registered) {
    atexit(do_final_cleanup);
    cleanup_registered = 1;
  }
}

/*
 * thread_switch_to_cleanup — switches execution to the neutral cleanup_stack
 * so that do_final_cleanup can safely free the current thread's stack.
 */
void thread_switch_to_cleanup(void) {
  getcontext(&cleanup_ctx);
  cleanup_ctx.uc_stack.ss_sp = cleanup_stack;
  cleanup_ctx.uc_stack.ss_size = sizeof(cleanup_stack);
  cleanup_ctx.uc_link = NULL;
  makecontext(&cleanup_ctx, do_final_cleanup, 0);
  setcontext(&cleanup_ctx);
  exit(1);
}
