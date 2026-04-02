#include "pool.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <ucontext.h>
#include <valgrind/valgrind.h>

static struct stack_entry *stack_pool = NULL;
static int stack_pool_size = 0;
static int stack_pool_cap = 0;
static const int MAX_POOLED_STACKS = 64; // Prevent unbounded pool growth

/*
 * stack_pool_alloc — pop a stack from the pool, or allocate a fresh one.
 */
int stack_pool_alloc(struct stack_entry *entry) {
  if (stack_pool_size > 0) {
    // Reuse a stack from the pool
    *entry = stack_pool[--stack_pool_size];
    return 0;
  }

  // Allocate a fresh stack with guard page
  void *map = mmap(NULL,
                   STACK_SIZE + GUARD_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                   -1,
                   0);
  if (map == MAP_FAILED) {
    return -1;
  }

  if (mprotect(map, GUARD_SIZE, PROT_NONE) == -1) {
    munmap(map, STACK_SIZE + GUARD_SIZE);
    return -1;
  }

  void *stack = (char *)map + GUARD_SIZE;
  entry->map = map;
  entry->stack = stack;
  entry->valgrind_id = VALGRIND_STACK_REGISTER(stack, (char *)stack + STACK_SIZE);
  return 0;
}

/*
 * stack_pool_push — return a stack to the pool for reuse.
 */
void stack_pool_push(struct stack_entry *entry) {
  if (stack_pool_size >= MAX_POOLED_STACKS) {
    // Pool is full; free the stack immediately
    VALGRIND_STACK_DEREGISTER(entry->valgrind_id);
    munmap(entry->map, STACK_SIZE + GUARD_SIZE);
    return;
  }

  if (stack_pool_size >= stack_pool_cap) {
    // Grow the pool array
    int new_cap = (stack_pool_cap == 0) ? 8 : stack_pool_cap * 2;
    if (new_cap > MAX_POOLED_STACKS)
      new_cap = MAX_POOLED_STACKS;
    struct stack_entry *new_pool = realloc(stack_pool, sizeof(*new_pool) * new_cap);
    if (new_pool == NULL) {
      // Realloc failed; free the stack instead
      VALGRIND_STACK_DEREGISTER(entry->valgrind_id);
      munmap(entry->map, STACK_SIZE + GUARD_SIZE);
      return;
    }
    stack_pool = new_pool;
    stack_pool_cap = new_cap;
  }

  stack_pool[stack_pool_size++] = *entry;
}

/*
 * stack_pool_free_all — drain the pool at program exit.
 */
void stack_pool_free_all(void) {
  for (int i = 0; i < stack_pool_size; ++i) {
    VALGRIND_STACK_DEREGISTER(stack_pool[i].valgrind_id);
    munmap(stack_pool[i].map, STACK_SIZE + GUARD_SIZE);
  }
  free(stack_pool);
  stack_pool = NULL;
  stack_pool_size = 0;
  stack_pool_cap = 0;
}
