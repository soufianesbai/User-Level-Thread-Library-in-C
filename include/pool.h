#ifndef POOL_H
#define POOL_H

#define STACK_SIZE (64 * 1024)
#define GUARD_SIZE (4 * 1024)

struct stack_entry {
  void *map;            // The entire mapped area (including guard)
  void *stack;          // The usable stack area (after the guard)
  unsigned valgrind_id; // Valgrind stack ID for memory checking
};

int stack_pool_alloc(struct stack_entry *entry);
void stack_pool_push(struct stack_entry *entry);
void stack_pool_free_all(void);

#endif // POOL_H
