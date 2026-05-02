#include "pool.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <valgrind/valgrind.h>

#ifdef THREAD_MULTICORE
#include <pthread.h>
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
#define POOL_LOCK()   pthread_mutex_lock(&pool_lock)
#define POOL_UNLOCK() pthread_mutex_unlock(&pool_lock)
#else
#define POOL_LOCK()   ((void)0)
#define POOL_UNLOCK() ((void)0)
#endif

/* Guard page enabled by default for stack overflow detection.
 * Override with -DTHREAD_ENABLE_GUARD_PAGE=0 to disable (better perf on alloc-heavy workloads). */
#ifndef THREAD_ENABLE_GUARD_PAGE
#define THREAD_ENABLE_GUARD_PAGE 1
#endif

/*
 * Stack pool used to recycle thread stacks.
 * Entries are stored in a static array with a simple size counter.
 *
 * MAX_POOLED_STACKS must cover the high-water mark of simultaneously
 * alive threads for the target workload (fib(29) ≈ 300k peak).
 * A large pool avoids mmap/munmap on the hot path: stacks are reused
 * instead of mapped/unmapped for each thread create/join cycle.
 */
#define MAX_POOLED_STACKS (1 << 17) /* 131072 — covers fib(29+) recycle phase (~3 MB static) */
static struct stack_entry stack_pool[MAX_POOLED_STACKS];
static int stack_pool_size = 0;

/*
 * stack_pool_alloc — pop a stack from the pool, or allocate a fresh one.
 *
 * Fast path (pool non-empty): pop one entry in O(1).
 * Slow path (pool empty): allocate a new mapping and set a guard page.
 */
int stack_pool_alloc(struct stack_entry *entry) {
  POOL_LOCK();
  if (stack_pool_size > 0) {
    /* Reuse a previously pooled stack. */
    *entry = stack_pool[--stack_pool_size];
    POOL_UNLOCK();
    return 0;
  }
  POOL_UNLOCK();

  /*
   * Allocate a fresh stack mapping with one guard page at the low address.
   * The usable stack is placed above it because stacks grow downward.
   */
  void *map = mmap(NULL,
                   STACK_SIZE + GUARD_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                   -1,
                   0);
  if (map == MAP_FAILED) {
    return -1;
  }

  /* Make the guard page inaccessible unless disabled for benchmark speed. */
#if THREAD_ENABLE_GUARD_PAGE
  if (mprotect(map, GUARD_SIZE, PROT_NONE) == -1) {
    munmap(map, STACK_SIZE + GUARD_SIZE);
    return -1;
  }
#endif

  /* The usable stack starts right after the guard page. */
  void *stack = (char *)map + GUARD_SIZE;
  entry->map = map;
  entry->stack = stack;
  entry->valgrind_id = VALGRIND_STACK_REGISTER(stack, (char *)stack + STACK_SIZE);
  return 0;
}

/*
 * stack_pool_push — return a stack to the pool for reuse.
 *
 * If the pool is full, the stack is unmapped immediately.
 * Otherwise, it is stored in O(1). Above a pool size threshold (8192),
 * MADV_FREE is also called to release the physical pages backing idle stacks.
 * MADV_FREE is advisory: if the OS has not yet reclaimed the pages when the
 * stack is next popped, no page fault occurs — making it essentially free on
 * unloaded systems while preventing OOM on large workloads.
 */
void stack_pool_push(struct stack_entry *entry) {
  POOL_LOCK();
  if (stack_pool_size >= MAX_POOLED_STACKS) {
    POOL_UNLOCK();
    VALGRIND_STACK_DEREGISTER(entry->valgrind_id);
    munmap(entry->map, STACK_SIZE + GUARD_SIZE);
    return;
  }

  if (stack_pool_size > 8192)
    madvise(entry->map, STACK_SIZE + GUARD_SIZE, MADV_FREE);

  stack_pool[stack_pool_size++] = *entry;
  POOL_UNLOCK();
}

int stack_pool_empty(void) {
  POOL_LOCK();
  int empty = stack_pool_size == 0;
  POOL_UNLOCK();
  return empty;
}

/*
 * stack_pool_free_all — drain all pooled stacks at program exit.
 */
void stack_pool_free_all(void) {
  POOL_LOCK();
  for (int i = 0; i < stack_pool_size; ++i) {
    VALGRIND_STACK_DEREGISTER(stack_pool[i].valgrind_id);
    munmap(stack_pool[i].map, STACK_SIZE + GUARD_SIZE);
  }
  stack_pool_size = 0;
  POOL_UNLOCK();
}
