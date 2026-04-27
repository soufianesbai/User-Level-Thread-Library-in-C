#include "thread.h"
#include <stdint.h>
#include <stdio.h>

static void *inc_task(void *arg) {
  uintptr_t v = (uintptr_t)arg;
  for (int i = 0; i < 2000; ++i) {
    v += 1;
    thread_yield();
  }
  return (void *)v;
}

int main(void) {
  thread_t t1, t2;
  if (thread_create(&t1, inc_task, (void *)0) != 0)
    return 1;
  if (thread_create(&t2, inc_task, (void *)100) != 0)
    return 1;

  void *r1 = NULL;
  void *r2 = NULL;
  if (thread_join(t1, &r1) != 0)
    return 1;
  if (thread_join(t2, &r2) != 0)
    return 1;

  if ((uintptr_t)r1 != 2000 || (uintptr_t)r2 != 2100)
    return 2;

  printf("test_monocore_compat: ok\n");
  return 0;
}
