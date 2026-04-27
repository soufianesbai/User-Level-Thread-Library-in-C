#include "thread.h"
#include <stdint.h>
#include <stdio.h>

static thread_mutex_t g_lock;
static int g_counter = 0;

static void *worker(void *arg) {
  (void)arg;
  for (int i = 0; i < 20000; ++i) {
    if (thread_mutex_lock(&g_lock) != 0)
      return (void *)0;
    g_counter++;
    if (thread_mutex_unlock(&g_lock) != 0)
      return (void *)0;
  }
  return (void *)1;
}

int main(void) {
  thread_set_concurrency(4);

  if (thread_mutex_init(&g_lock) != 0)
    return 1;

  enum { N = 8 };
  thread_t th[N];
  for (int i = 0; i < N; ++i) {
    if (thread_create(&th[i], worker, NULL) != 0)
      return 1;
  }

  for (int i = 0; i < N; ++i) {
    void *ret = NULL;
    if (thread_join(th[i], &ret) != 0 || (uintptr_t)ret != 1u)
      return 1;
  }

  if (thread_mutex_destroy(&g_lock) != 0)
    return 1;

  if (g_counter != N * 20000)
    return 2;

  printf("test_multicore_mutex: ok\n");
  return 0;
}
