#include "thread.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Recursive Fibonacci in multicore mode.
 *
 * Each call spawns two threads (one per sub-problem), creating an exponential
 * thread tree. This stresses concurrent create/join across all workers.
 * fib(16) = 987, which generates 2*fib(17)-1 = 3193 threads total. */

static void *fib(void *arg) {
  uintptr_t n = (uintptr_t)arg;

  if (n <= 1)
    return (void *)n;

  thread_t t1, t2;

  if (thread_create(&t1, fib, (void *)(n - 1)) != 0)
    return (void *)(uintptr_t)-1;
  if (thread_create(&t2, fib, (void *)(n - 2)) != 0)
    return (void *)(uintptr_t)-1;

  void *r1, *r2;
  thread_join(t1, &r1);
  thread_join(t2, &r2);

  return (void *)((uintptr_t)r1 + (uintptr_t)r2);
}

int main(void) {
  thread_set_concurrency(4);

  thread_t root;
  assert(thread_create(&root, fib, (void *)16) == 0);

  void *res;
  assert(thread_join(root, &res) == 0);
  assert((uintptr_t)res == 987);

  printf("test_multicore_fibonacci: ok (fib(16) = %lu)\n", (uintptr_t)res);
  return 0;
}
