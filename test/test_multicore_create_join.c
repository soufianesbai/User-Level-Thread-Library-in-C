#include "thread.h"
#include <stdint.h>
#include <stdio.h>

static void *unit_task(void *arg) {
  uintptr_t v = (uintptr_t)arg;
  for (int i = 0; i < 1500; ++i) {
    v = v * 1103515245u + 12345u;
    if ((i & 63) == 0) {
      thread_yield();
    }
  }
  return (void *)(uintptr_t)((v != 0u) ? 1u : 0u);
}

int main(void) {
  thread_set_concurrency(4);

  enum { ROUNDS = 20, NTHREADS = 64 };

  for (int round = 0; round < ROUNDS; ++round) {
    thread_t th[NTHREADS];

    for (int i = 0; i < NTHREADS; ++i) {
      uintptr_t seed = (uintptr_t)(i + 1 + round * NTHREADS);
      if (thread_create(&th[i], unit_task, (void *)seed) != 0) {
        return 1;
      }
    }

    for (int i = 0; i < NTHREADS; ++i) {
      void *ret = NULL;
      if (thread_join(th[i], &ret) != 0 || (uintptr_t)ret != 1u) {
        return 2;
      }
    }
  }

  printf("test_multicore_create_join: ok\n");
  return 0;
}
