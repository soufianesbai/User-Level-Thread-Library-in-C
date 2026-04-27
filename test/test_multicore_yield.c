#include "thread.h"
#include <stdint.h>
#include <stdio.h>

static void *yield_worker(void *arg) {
  uintptr_t id = (uintptr_t)arg;
  uintptr_t acc = id;
  for (int i = 0; i < 10000; ++i) {
    acc += (uintptr_t)i;
    thread_yield();
  }
  return (void *)(uintptr_t)(acc != 0);
}

int main(void) {
  thread_set_concurrency(4);

  enum { N = 24 };
  thread_t th[N];

  for (int i = 0; i < N; ++i) {
    if (thread_create(&th[i], yield_worker, (void *)(uintptr_t)(i + 1)) != 0)
      return 1;
  }

  for (int i = 0; i < N; ++i) {
    void *ret = NULL;
    if (thread_join(th[i], &ret) != 0 || (uintptr_t)ret != 1u)
      return 1;
  }

  printf("test_multicore_yield: ok\n");
  return 0;
}
