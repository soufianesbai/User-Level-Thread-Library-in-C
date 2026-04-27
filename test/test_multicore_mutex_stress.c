#include "thread.h"
#include <stdint.h>
#include <stdio.h>

static thread_mutex_t g_lock;
static int g_shared = 0;

static void *mutex_worker(void *arg) {
  uintptr_t id = (uintptr_t)arg;

  for (int i = 0; i < 25000; ++i) {
    if ((i & 31) == 0) {
      thread_yield();
    }

    if (thread_mutex_lock(&g_lock) != 0) {
      return (void *)0;
    }

    int local = g_shared;
    local += (int)(id & 1u) ? 1 : 2;
    g_shared = local;

    if (thread_mutex_unlock(&g_lock) != 0) {
      return (void *)0;
    }
  }

  return (void *)1;
}

int main(void) {
  thread_set_concurrency(4);

  enum { N = 16 };
  thread_t th[N];

  if (thread_mutex_init(&g_lock) != 0) {
    return 1;
  }

  for (int i = 0; i < N; ++i) {
    if (thread_create(&th[i], mutex_worker, (void *)(uintptr_t)(i + 1)) != 0) {
      return 2;
    }
  }

  int expected = 0;
  for (int i = 0; i < N; ++i) {
    expected += (i % 2 == 0) ? 1 : 2;
  }
  expected *= 25000;

  for (int i = 0; i < N; ++i) {
    void *ret = NULL;
    if (thread_join(th[i], &ret) != 0 || (uintptr_t)ret != 1u) {
      return 3;
    }
  }

  if (thread_mutex_destroy(&g_lock) != 0) {
    return 4;
  }

  if (g_shared != expected) {
    return 5;
  }

  printf("test_multicore_mutex_stress: ok\n");
  return 0;
}
