#include "thread.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

/* Test join-cycle deadlock detection in multicore mode.
 *
 * Same cycle as 81-deadlock: main(th0) joins th1, th1 joins th2, th2 joins
 * th0. Exactly one join must return EDEADLK; the sum of all return values
 * must equal EDEADLK. Workers run concurrently so the cycle detection must
 * work across multiple CPUs. */

static thread_t th0, th1, th2;
static int totalerr = 0;

static void *thfunc2(void *arg __attribute__((unused))) {
  void *res;
  int err = thread_join(th0, &res);
  totalerr += err;
  return NULL;
}

static void *thfunc1(void *arg __attribute__((unused))) {
  void *res;
  int err = thread_create(&th2, thfunc2, NULL);
  assert(!err);
  err = thread_join(th2, &res);
  totalerr += err;
  return NULL;
}

int main(void) {
  thread_set_concurrency(4);

  th0 = thread_self();

  int err = thread_create(&th1, thfunc1, NULL);
  assert(!err);

  void *res;
  err = thread_join(th1, &res);
  totalerr += err;

  assert(totalerr == EDEADLK);
  printf("test_multicore_deadlock: ok\n");
  return 0;
}
