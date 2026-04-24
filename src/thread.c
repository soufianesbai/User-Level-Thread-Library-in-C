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
static thread main_thread = {.id = 0,
                             .state = THREAD_RUNNING,
                             .joined_by = NULL,
                             .priority = THREAD_DEFAULT_PRIORITY,
                             .waiting_for = NULL,
                             .head_joiner = NULL};
static thread *current_thread = &main_thread;
static int next_thread_id = 1;
static int scheduler_initialized = 0;

/*
 * Template context captured once during scheduler initialization.
 * Used in thread_create() to initialize each new context by copy,
 * instead of calling getcontext() for every thread creation.
 *
 * Why: getcontext() is a syscall that captures registers, signal mask, etc.
 * makecontext() overwrites all the parts that matter anyway (stack pointer,
 * entry point), so the template content is irrelevant — only its structure
 * (size, alignment) is needed. Copying it saves ~10,000 syscalls on test 21.
 */
static ucontext_t scheduler_template_ctx;

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

#define STACK_PREFILL_COUNT 2048

static void reclaim_deferred_stacks_batch(int budget) {
  while (deferred_stack_count > 0 && budget-- > 0) {
    struct stack_entry e = deferred_stacks[--deferred_stack_count];
    stack_pool_push(&e);
  }
}

static void reclaim_deferred_stacks_all(void) {
  reclaim_deferred_stacks_batch(MAX_DEFERRED_STACKS);
}

static void defer_stack_reclaim(thread *t) {
  if (t == NULL || t->stack_map == NULL) {
    return;
  }

  if (deferred_stack_count < MAX_DEFERRED_STACKS) {
    deferred_stacks[deferred_stack_count++] = (struct stack_entry){
        .map = t->stack_map,
        .stack = t->context.uc_stack.ss_sp,
        .valgrind_id = t->valgrind_stack_id,
    };

    // Mark ownership transferred so thread_join() does not push twice.
    t->stack_map = NULL;
    t->context.uc_stack.ss_sp = NULL;
  }
}

/*
 * thread_pool_free_all — free cached thread objects at process exit.
 */
static void thread_pool_free_all(void) {
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
  if (thread_pool_size > 0)
    return thread_pool[--thread_pool_size];
  return malloc(sizeof(thread));
}

/*
 * thread_free — return a thread struct to the pool, or free it if full.
 */
static void thread_free(thread *t) {
  if (thread_pool_size < MAX_POOLED_THREADS)
    thread_pool[thread_pool_size++] = t;
  else
    free(t);
}

static thread **thread_head_ref_alloc(thread *owner) {
  if (head_ref_active_chunk == NULL || head_ref_active_index >= HEAD_REF_CHUNK_SIZE) {
    thread **new_chunk = malloc(sizeof(*new_chunk) * HEAD_REF_CHUNK_SIZE);
    if (new_chunk == NULL) {
      return NULL;
    }

    if (head_ref_chunks_count >= head_ref_chunks_cap) {
      int new_cap = (head_ref_chunks_cap == 0) ? 64 : head_ref_chunks_cap * 2;
      thread ***new_chunks = realloc(head_ref_chunks, sizeof(*new_chunks) * new_cap);
      if (new_chunks == NULL) {
        free(new_chunk);
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
 * thread_scheduler_enqueue — adds a thread to the ready queue.
 * - If THREAD_SCHED_POLICY is FIFO: appends at the end (FIFO order)
 * - If THREAD_SCHED_POLICY is PRIO: inserts by priority (higher priority first)
 */
void thread_scheduler_enqueue(thread *t) {
  if (t == NULL || t->in_ready_queue)
    return;
#if THREAD_SCHED_POLICY == THREAD_SCHED_FIFO
  // FIFO: just append at the end
  t->in_ready_queue = 1;
  TAILQ_INSERT_TAIL(&ready_queue, t, entries);
#elif THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  // Priority: insert in descending order of priority
  thread *cursor;
  TAILQ_FOREACH(cursor, &ready_queue, entries) {
    if (t->priority > cursor->priority) {
      // printf("Inserting thread %d with priority %d before thread %d with priority %d\n",
      //        t->id, t->priority, cursor->id, cursor->priority);
      t->in_ready_queue = 1;
      TAILQ_INSERT_BEFORE(cursor, t, entries);
      return;
    }
  }
  t->in_ready_queue = 1;
  // If not inserted before any thread, add at the end
  TAILQ_INSERT_TAIL(&ready_queue, t, entries);
#endif
}

/*
 * thread_scheduler_pick_next — gets the next thread to run.
 * - If THREAD_SCHED_POLICY is FIFO: returns the first ready thread
 * - If THREAD_SCHED_POLICY is PRIO: returns the highest priority thread
 *   (already ordered by enqueue, so just return first)
 */
thread *thread_scheduler_pick_next(void) {
  if (TAILQ_EMPTY(&ready_queue))
    return NULL;

  thread *next = TAILQ_FIRST(&ready_queue);

  TAILQ_REMOVE(&ready_queue, next, entries);
  next->in_ready_queue = 0;

  return next;
}

/*
  set current_thread to next as THREAD_RUNNING and switch context from prev to next.
*/
int swap_thread(thread *prev, thread *next) {
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
    atexit(thread_pool_free_all);

    // Initialize the main thread's context so it can be switched to like any
    // other thread.
    if (getcontext(&main_thread.context) == -1) {
      return -1;
    }

    /*
     * Capture the template context exactly once.
     * All future threads will copy this instead of calling getcontext(),
     * eliminating one syscall per thread_create() call.
     */
    if (getcontext(&scheduler_template_ctx) == -1) {
      return -1;
    }

    main_thread.head_joiner = thread_head_ref_alloc(&main_thread);
    if (main_thread.head_joiner == NULL) {
      errno = ENOMEM;
      return -1;
    }

#ifdef ENABLE_PREEMPTION
    init_prem(preemption_handler, 5);
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
    reclaim_deferred_stacks_all();
    if (TAILQ_EMPTY(&ready_queue) || alloc_retries++ >= 4096) {
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
  newth->valgrind_stack_id = stack_entry.valgrind_id;
  newth->priority = THREAD_DEFAULT_PRIORITY;
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

  /*
   * Copy the template context instead of calling getcontext().
   * makecontext() will overwrite the stack pointer and entry point anyway,
   * so the template's register content does not matter — only the
   * ucontext_t structure layout is needed. This avoids one syscall per
   * thread creation (~10,000 syscalls saved on test 21 with nb=10000).
   */
  newth->context = scheduler_template_ctx;
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
  thread_scheduler_enqueue(newth);
  *newthread = (thread_t)newth;

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_yield — yields the CPU to another ready thread.
 * In priority mode, only yields if there's a higher priority thread ready.
 * If no other thread is ready (or no higher priority thread), returns immediately.
 */
int thread_yield(void) {
#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  if (deferred_stack_count > 0)
    reclaim_deferred_stacks_batch(64);

  thread *prev = current_thread;

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO

  // Aging: tous les threads en attente gagnent de la priorité
  thread *t;
  TAILQ_FOREACH(t, &ready_queue, entries) {
    t->priority += THREAD_WAIT;
    if (t->priority > THREAD_MAX_PRIORITY)
      t->priority = THREAD_MAX_PRIORITY;
  }

  // Pénalité sur le thread courant
  if (prev->state == THREAD_RUNNING) {
    prev->priority -= THREAD_AGING;
    if (prev->priority < THREAD_MIN_PRIORITY)
      prev->priority = THREAD_MIN_PRIORITY;
  }

#endif

  // Choisir le prochain
  thread *next = thread_scheduler_pick_next();

  if (!next || next == prev) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO

  // Décision: switch seulement si meilleur
  if (next->priority <= prev->priority) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    thread_scheduler_enqueue(next);
    return 0;
  }

#endif

  // 5. Remettre le courant dans la queue
  if (prev->state == THREAD_RUNNING) {
    prev->state = THREAD_READY;
    thread_scheduler_enqueue(prev);
  }

  // 6. Switch
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
  thread_scheduler_enqueue(prev);

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
    thread_scheduler_enqueue(joiner);
  }

  // Pick the next thread normally via FIFO — no special case needed.
  // The joiner (if any) is already in the queue at this point.
  thread *next = thread_scheduler_pick_next();

  if (next == NULL) {
    thread_switch_to_cleanup();
  }

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
 */
int thread_join(thread_t thread_handle, void **retval) {
  if (thread_handle == NULL) {
    errno = EINVAL;
    return -1;
  }

  thread *target = (thread *)thread_handle;

  // Multiple join check: a thread can only be joined by one other thread
  // if (target->joined_by != NULL) {
  //   errno = EINVAL;
  //   return -1;
  // }

  if (target == current_thread) {
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
      thread_zombie_remove(target);
      if (target->stack_map != NULL) {
        struct stack_entry entry = {.map = target->stack_map,
                                    .stack = target->context.uc_stack.ss_sp,
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

  if (target->head_joiner == NULL || current_thread->head_joiner == NULL) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    errno = EINVAL;
    return -1;
  }

  thread *target_head = *target->head_joiner;
  int target_is_head = (target_head == target);
  int same_chain = (current_thread->head_joiner == target->head_joiner);

  // Joining the head from inside the same chain would create a cycle.
  if (target_is_head && same_chain) {
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    errno = EDEADLK;
    return EDEADLK;
  }

  // Mark the current thread as the joiner of the target
  target->joined_by = current_thread;

  // If an external thread joins the current head, it becomes the new head in O(1).
  if (target_is_head && !same_chain) {
    current_thread->head_joiner = target->head_joiner;
    *target->head_joiner = current_thread;
  } else {
    // current thread joins target's chain
    current_thread->head_joiner = target->head_joiner;
  }

  if (target->state != THREAD_TERMINATED) {
    current_thread->state = THREAD_BLOCKED;
    current_thread->waiting_for = target;

    thread *prev = current_thread;
    thread *next = thread_scheduler_pick_next();

    if (next == NULL) {
      current_thread->state = THREAD_RUNNING;
      current_thread->waiting_for = NULL;
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      errno = EDEADLK;
      return EDEADLK;
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
    thread_obj_release(target);
  }

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  return 0;
}

int thread_set_priority(thread_t t, int prio) {
  if (!t)
    return -1;

  thread *th = (thread *)t;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  // clamp
  if (prio > THREAD_MAX_PRIORITY)
    prio = THREAD_MAX_PRIORITY;
  if (prio < THREAD_MIN_PRIORITY)
    prio = THREAD_MIN_PRIORITY;

  th->priority = prio;

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  if (th->state == THREAD_READY) { // Pour insérer le thread à la bonne position dans la ready_queue
                                   // si i est dèjà dans la queue
    TAILQ_REMOVE(&ready_queue, th, entries);
    th->in_ready_queue = 0;
    thread_scheduler_enqueue(th);
  }
#endif

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  return 0;
}
