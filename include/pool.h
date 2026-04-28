#ifndef POOL_H
#define POOL_H

/*
 * Stack memory layout per thread:
 *
 *   [ GUARD_SIZE | STACK_SIZE ]
 *     ^            ^
 *     map          stack (usable region)
 *
 * The guard page is mapped PROT_NONE at the low end of the allocation.
 * Any stack overflow will fault on it instead of silently corrupting memory.
 */
#define STACK_SIZE (32 * 1024)
#define GUARD_SIZE (4 * 1024)

struct stack_entry {
  void *map;   /* base of the full mmap'd region (guard + stack) */
  void *stack; /* top of the usable stack (passed to fast_ctx_init) */
  
  /* Valgrind requires explicit registration of mmap'd stacks; without it,
   * it reports false positives on every stack access. */
  unsigned valgrind_id;
};

/* Allocate a stack from the pool, or mmap a new one if the pool is empty.
 * Returns 0 on success, -1 on error. */
int stack_pool_alloc(struct stack_entry *entry);

/* Return a stack to the pool for reuse. If the pool is full, the stack is
 * unmapped immediately instead of being stored. */
void stack_pool_push(struct stack_entry *entry);

/* Unmap all stacks currently held in the pool. */
void stack_pool_free_all(void);

/* Returns 1 if the pool has no recycled stacks available, 0 otherwise. */
int stack_pool_empty(void);

#endif /* POOL_H */
