#include "thread.h"
#include "preemption.h"
#include "thread_internal.h"
#include "thread_sync_internal.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <valgrind/valgrind.h>

// The main thread is initialized at the start of the program and will be used
// as the initial context for the main execution flow.
static thread main_thread = {.id = 0,
                             .state = THREAD_RUNNING,
                             .joined_by = NULL,
                             .priority = THREAD_DEFAULT_PRIORITY,
                             .affinity = -1,
                             .waiting_for = NULL,
                             .head_joiner = NULL};
/*
 * current thread pointer is thread-local in multicore mode so each worker has
 * its own currently-running user thread. In monocore mode this is still a
 * single global pointer (THREAD_LOCAL expands to empty).
 */
static THREAD_LOCAL thread *current_thread = &main_thread;
static int next_thread_id = 1;
static int scheduler_initialized = 0;

/*
 * Thread struct pool — recycles thread heap allocations across create/join.
 *
 * Why: thread_create() calls malloc(sizeof(thread)) and thread_join() calls
 * free() on every cycle. A fixed-size pool of recycled structs replaces those
 * with a pointer array push/pop in O(1).
 */
#define MAX_POOLED_THREADS 50000
static thread *thread_pool[MAX_POOLED_THREADS];
static int thread_pool_size = 0;

#define HEAD_REF_CHUNK_SIZE 1024
static thread ***head_ref_chunks = NULL;
static int head_ref_chunks_count = 0;
static int head_ref_chunks_cap = 0;
static thread **head_ref_active_chunk = NULL;
static int head_ref_active_index = 0;

#define MAX_DEFERRED_STACKS 16384
static struct stack_entry deferred_stacks[MAX_DEFERRED_STACKS];
static int deferred_stack_count = 0;

#define STACK_PREFILL_COUNT 4096

static void reclaim_deferred_stacks_batch(int budget) {
  SCHED_LOCK();
  while (deferred_stack_count > 0 && budget-- > 0) {
    struct stack_entry e = deferred_stacks[--deferred_stack_count];
    SCHED_UNLOCK();
    stack_pool_push(&e);
    SCHED_LOCK();
  }
  SCHED_UNLOCK();
}

void reclaim_deferred_stacks_all(void) {
  reclaim_deferred_stacks_batch(MAX_DEFERRED_STACKS);
}

static void defer_stack_reclaim(thread *t) {
  if (t == NULL || t->stack_map == NULL) {
    return;
  }

  /*
   * Called from thread_exit() while scheduler_lock is already held.
   * Keep this helper lock-free to avoid self-deadlock in multicore mode.
   */
  if (deferred_stack_count < MAX_DEFERRED_STACKS) {
    deferred_stacks[deferred_stack_count++] = (struct stack_entry){
        .map = t->stack_map,
        .stack = t->stack_base,
        .valgrind_id = t->valgrind_stack_id,
    };

    // Mark ownership transferred so thread_join() does not push twice.
    t->stack_map = NULL;
    t->stack_base = NULL;
  }
}

/*
 * thread_pool_free_all — free cached thread objects at process exit.
 */
static void thread_pool_free_all(void) {
  thread_scheduler_sync_shutdown();
  reclaim_deferred_stacks_all();

  for (int i = 0; i < thread_pool_size; ++i) {
    thread *t = thread_pool[i];
    if (t == NULL)
      continue;
    free(t);
    thread_pool[i] = NULL;
  }
  thread_pool_size = 0;

  for (int i = 0; i < head_ref_chunks_count; ++i) {
    free(head_ref_chunks[i]);
    head_ref_chunks[i] = NULL;
  }
  free(head_ref_chunks);
  head_ref_chunks = NULL;
  head_ref_chunks_count = 0;
  head_ref_chunks_cap = 0;
  head_ref_active_chunk = NULL;
  head_ref_active_index = 0;
}

/*
 * thread_alloc — pop a thread struct from the pool or malloc a fresh one.
 */
static thread *thread_alloc(void) {
  SCHED_LOCK();
  if (thread_pool_size > 0) {
    thread *reused = thread_pool[--thread_pool_size];
    SCHED_UNLOCK();
    return reused;
  }
  SCHED_UNLOCK();
  return malloc(sizeof(thread));
}

/*
 * thread_free — return a thread struct to the pool, or free it if full.
 */
static void thread_free(thread *t) {
  SCHED_LOCK();
  if (thread_pool_size < MAX_POOLED_THREADS) {
    thread_pool[thread_pool_size++] = t;
    SCHED_UNLOCK();
  } else {
    SCHED_UNLOCK();
    free(t);
  }
}

static thread **thread_head_ref_alloc(thread *owner) {
  SCHED_LOCK();
  if (head_ref_active_chunk == NULL || head_ref_active_index >= HEAD_REF_CHUNK_SIZE) {
    thread **new_chunk = malloc(sizeof(*new_chunk) * HEAD_REF_CHUNK_SIZE);
    if (new_chunk == NULL) {
      SCHED_UNLOCK();
      return NULL;
    }

    if (head_ref_chunks_count >= head_ref_chunks_cap) {
      int new_cap = (head_ref_chunks_cap == 0) ? 64 : head_ref_chunks_cap * 2;
      thread ***new_chunks = realloc(head_ref_chunks, sizeof(*new_chunks) * new_cap);
      if (new_chunks == NULL) {
        free(new_chunk);
        SCHED_UNLOCK();
        return NULL;
      }
      head_ref_chunks = new_chunks;
      head_ref_chunks_cap = new_cap;
    }

    head_ref_chunks[head_ref_chunks_count++] = new_chunk;
    head_ref_active_chunk = new_chunk;
    head_ref_active_index = 0;
  }

  thread **ref = &head_ref_active_chunk[head_ref_active_index++];
  *ref = owner;
  SCHED_UNLOCK();
  return ref;
}

static void thread_obj_release(thread *t) {
  if (t == NULL || t == &main_thread) {
    return;
  }

  // The object is recycled; do not keep a shared chain pointer in the pool.
  t->head_joiner = NULL;

  thread_free(t);
}

thread *thread_get_current(void) {
  return current_thread;
}

void thread_set_current(thread *t) {
  current_thread = t;
}

thread *thread_get_current_thread(void) {
  return thread_get_current();
}

void thread_set_current_thread(thread *t) {
  thread_set_current(t);
}

/*
  a wrapper for the preemption signal handler that just yields the current thread.
  sigaction take an func(int) not a func(void)
*/
void preemption_handler(int sig) {
  (void)sig;
  thread_yield();
}

/*
 * thread_entry — entry point for every new thread.
 * Calls the user function then exits the thread when it returns.
 */
static void thread_entry(void) {
#ifdef ENABLE_PREEMPTION
  /* New threads inherit the caller's signal mask (preem_block was active
   * during the context switch). Unblock here so preemption fires normally. */
  preem_unblock();
#endif
  thread *self = thread_get_current();
  void *retval = self->start_fun(self->arg);
  thread_exit(retval);
}

/*
 * thread_self — retrieves the identifier of the current thread.
 */
thread_t thread_self(void) {
  return (thread_t)thread_get_current();
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
    thread_scheduler_sync_init();
    thread_cleanup_register();
    atexit(thread_pool_free_all);

    main_thread.head_joiner = thread_head_ref_alloc(&main_thread);
    if (main_thread.head_joiner == NULL) {
      errno = ENOMEM;
      return -1;
    }

#ifdef ENABLE_PREEMPTION
    init_prem(preemption_handler, 100);
#endif

    /*
     * Pre-fill the stack pool so early thread_create() calls can reuse
     * mappings instead of paying mmap()+mprotect() on the critical path.
     */
    for (int i = 0; i < STACK_PREFILL_COUNT; i++) {
      struct stack_entry e;
      if (stack_pool_alloc(&e) == 0)
        stack_pool_push(&e);
    }

    scheduler_initialized = 1;
  }

  /*
   * Reuse a thread struct from the pool instead of calling malloc().
   * Falls back to malloc() only when the pool is empty (cold start or
   * high concurrency beyond MAX_POOLED_THREADS simultaneous threads).
   */
  thread *newth = thread_alloc();
  if (newth == NULL) {
    errno = ENOMEM;
    return -1;
  }

  // Allocate or reuse stack from pool.
  struct stack_entry stack_entry;
  if (stack_pool_empty() && deferred_stack_count > 0)
    reclaim_deferred_stacks_all();
  int alloc_retries = 0;
  while (stack_pool_alloc(&stack_entry) == -1) {
    int ready_empty = 0;
    reclaim_deferred_stacks_all();
    SCHED_LOCK();
    ready_empty = TAILQ_EMPTY(thread_get_ready_queue());
    SCHED_UNLOCK();

    if (ready_empty || alloc_retries++ >= 4096) {
      thread_obj_release(newth);
      errno = ENOMEM;
      return -1;
    }
    thread_yield();
  }

  newth->start_fun = func;
  newth->arg = funcarg;
  newth->state = THREAD_READY;
  newth->retval = NULL;
  newth->stack_map = stack_entry.map;
  newth->stack_base = stack_entry.stack;
  newth->valgrind_stack_id = stack_entry.valgrind_id;
  newth->priority = THREAD_DEFAULT_PRIORITY;
  newth->affinity = -1;
  newth->in_ready_queue = 0;
  newth->joined_by = NULL;
  newth->waiting_for = NULL;
#ifdef ENABLE_SIGNAL
  newth->pending_signals = 0;
  newth->blocked_signals = 0;
  newth->waited_signals = 0;
  newth->waiting_for_signal = 0;
#endif
  newth->head_joiner = thread_head_ref_alloc(newth);
  if (newth->head_joiner == NULL) {
    thread_obj_release(newth);
    stack_pool_push(&stack_entry);
    errno = ENOMEM;
    return -1;
  }

  /* Set up the initial execution context.
   * fast_ctx_init arranges the stack and entry point so that the first
   * fast_swap_context to this thread jumps directly to thread_entry on the
   * thread's own stack — no makecontext/getcontext syscalls needed. */
  fast_ctx_init(&newth->context, (char *)stack_entry.stack + STACK_SIZE, thread_entry);

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif
  // Keep the critical section small: only shared scheduler state
  SCHED_LOCK();
  newth->id = next_thread_id++;
  SCHED_UNLOCK();
  thread_scheduler_enqueue(newth);
  *newthread = (thread_t)newth;

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

  thread *dying = thread_get_current();

  SCHED_LOCK();
  dying->retval = retval;
  dying->state = THREAD_TERMINATED;
  // Non-main threads go to the zombie queue so thread_join() can
  // still read their retval. The main thread's struct is static —
  // it never needs to be freed, so we skip zombification.
  if (dying != &main_thread) {
    // A terminating thread cannot safely recycle its own running stack.
    // Defer stack reuse until another thread is running.
    defer_stack_reclaim(dying);
    thread_zombie_add(dying);
  }

  // If a thread was waiting on this one, unblock it now
  // by putting it back in the ready queue.
  if (dying->joined_by != NULL) {
    thread *joiner = dying->joined_by;
    joiner->state = THREAD_READY;
    thread_scheduler_enqueue_locked(joiner);
  }

  // Pick the next thread normally via FIFO — no special case needed.
  // The joiner (if any) is already in the queue at this point.
  thread *next = thread_scheduler_pick_next_locked();
  SCHED_BROADCAST();
  SCHED_UNLOCK();

  if (next == NULL) {
#ifdef THREAD_MULTICORE
    thread *worker_stub = thread_scheduler_get_worker_stub();
    if (worker_stub != NULL) {
      /*
       * Return to the worker loop context saved in the worker stub. The
       * worker loop handles waiting for new READY threads and clean shutdown
       * when scheduler_running=0.
       */
      thread_set_current(worker_stub);
      worker_stub->state = THREAD_RUNNING;
      fast_swap_context(&dying->context, &worker_stub->context);
    }
#endif
    thread_switch_to_cleanup();
  }

  thread_set_current(next);
  next->state = THREAD_RUNNING;
  fast_restore_context(&next->context);

  __builtin_unreachable();
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
 */
int thread_join(thread_t thread_handle, void **retval) {
  if (thread_handle == NULL) {
    errno = EINVAL;
    return -1;
  }

  thread *target = (thread *)thread_handle;

  // Multiple join check: follow pthread semantics — only one joiner allowed.
  if (target->joined_by != NULL) {
    errno = EINVAL;
    return -1;
  }

  thread *self = thread_get_current();

  if (target == self) {
    errno = EDEADLK;
    return EDEADLK;
  }

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  // Fast path: already terminated target can be joined immediately without blocking or yielding.
  if (target->state == THREAD_TERMINATED) {
    if (retval != NULL) {
      *retval = target->retval;
    }

    if (target != &main_thread) {
      SCHED_LOCK();
      thread_zombie_remove(target);
      SCHED_UNLOCK();
      if (target->stack_map != NULL) {
        struct stack_entry entry = {.map = target->stack_map,
                                    .stack = target->stack_base,
                                    .valgrind_id = target->valgrind_stack_id};
        stack_pool_push(&entry);
      }
      thread_obj_release(target);
    }

#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }

#ifdef THREAD_MULTICORE
  /*
   * When worker threads are active, the main thread must not perform direct
   * user-thread context switches concurrently with workers. It waits on the
   * scheduler condition and lets workers drive execution.
   */
  if (thread_scheduler_has_workers() && thread_scheduler_get_worker_stub() == NULL) {
    while (target->state != THREAD_TERMINATED) {
      SCHED_LOCK();
      if (target->state == THREAD_TERMINATED) {
        SCHED_UNLOCK();
        break;
      }
      SCHED_WAIT();
      SCHED_UNLOCK();
    }

#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif

    if (retval != NULL) {
      *retval = target->retval;
    }

#ifdef ENABLE_PREEMPTION
    preem_block();
#endif
    if (target != &main_thread) {
      SCHED_LOCK();
      thread_zombie_remove(target);
      SCHED_UNLOCK();
      if (target->stack_map != NULL) {
        struct stack_entry entry = {.map = target->stack_map,
                                    .stack = target->stack_base,
                                    .valgrind_id = target->valgrind_stack_id};
        stack_pool_push(&entry);
      }
      thread_obj_release(target);
    }
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }
#endif

  if (target->head_joiner == NULL || self->head_joiner == NULL) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    errno = EINVAL;
    return -1;
  }

  thread *target_head = *target->head_joiner;
  int target_is_head = (target_head == target);
  int same_chain = (self->head_joiner == target->head_joiner);

  // Joining the head from inside the same chain would create a cycle.
  if (target_is_head && same_chain) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    errno = EDEADLK;
    return EDEADLK;
  }

  // Mark the current thread as the joiner of the target
  target->joined_by = self;

  // If an external thread joins the current head, it becomes the new head in O(1).
  if (target_is_head && !same_chain) {
    self->head_joiner = target->head_joiner;
    *target->head_joiner = self;
  } else {
    // current thread joins target's chain
    self->head_joiner = target->head_joiner;
  }

  if (target->state != THREAD_TERMINATED) {
    self->state = THREAD_BLOCKED;
    self->waiting_for = target;

    thread *prev = self;
    thread *next = thread_scheduler_pick_next();

    if (next == NULL) {
#ifdef THREAD_MULTICORE
      if (thread_scheduler_has_workers()) {
        while (target->state != THREAD_TERMINATED) {
          SCHED_LOCK();
          if (target->state == THREAD_TERMINATED) {
            SCHED_UNLOCK();
            break;
          }
          SCHED_WAIT();
          SCHED_UNLOCK();
        }

        self->state = THREAD_RUNNING;
        self->waiting_for = NULL;
      } else {
        self->state = THREAD_RUNNING;
        self->waiting_for = NULL;
#ifdef ENABLE_PREEMPTION
        preem_unblock();
#endif
        errno = EDEADLK;
        return EDEADLK;
      }
#else
      self->state = THREAD_RUNNING;
      self->waiting_for = NULL;
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      errno = EDEADLK;
      return EDEADLK;
#endif
    }

    if (next != NULL) {
      // Yield exactly once. We will return here only when thread_exit()
      // of target puts us back into the ready queue.
      swap_thread(prev, next);
    }

    // At this point, target->state == THREAD_TERMINATED is guaranteed
    // by the wakeup invariant in thread_exit().
    self->waiting_for = NULL;
    self->state = THREAD_RUNNING;
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
    SCHED_LOCK();
    thread_zombie_remove(target);
    SCHED_UNLOCK();
    if (target->stack_map != NULL) {
      struct stack_entry entry = {.map = target->stack_map,
                                  .stack = target->stack_base,
                                  .valgrind_id = target->valgrind_stack_id};
      stack_pool_push(&entry);
    }
    thread_obj_release(target);
  }

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  return 0;
}
