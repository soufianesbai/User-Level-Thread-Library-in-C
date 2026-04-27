#include "thread.h"
#include <stdint.h>
#include <stdio.h>

static void *cpu_task(void *arg) {
  uintptr_t seed = (uintptr_t)arg + 1u;
  uint64_t acc = 0;
  for (uint64_t i = 0; i < 3000000ULL; ++i) {
    acc += (i ^ seed) * 2654435761u;
  }
  return (void *)(uintptr_t)(acc != 0);
}

int main(void) {
  thread_set_concurrency(4);

  enum { N = 8 };
  thread_t th[N];
  for (int i = 0; i < N; ++i) {
    if (thread_create(&th[i], cpu_task, (void *)(uintptr_t)i) != 0)
      return 1;
  }

  int ok = 1;
  for (int i = 0; i < N; ++i) {
    void *ret = NULL;
    if (thread_join(th[i], &ret) != 0)
      return 1;
    ok &= ((uintptr_t)ret == 1u);
  }

  printf("test_multicore_cpu: %s\n", ok ? "ok" : "fail");
  return ok ? 0 : 2;
}
