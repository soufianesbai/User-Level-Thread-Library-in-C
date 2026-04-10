#include "thread.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Deadlock long chain test:
 * th0(main) joins th1, th1 joins th2, ..., thN joins th0.
 * Exactly one join should return EDEADLK, others should return 0.
 */

#define CHAIN_LEN 12

static thread_t threads[CHAIN_LEN];
static int totalerr = 0;

static void *chain_func(void *arg) {
  int idx = (int)(long)arg;
  void *res = NULL;
  int err;

  if (idx + 1 < CHAIN_LEN) {
    err = thread_create(&threads[idx + 1], chain_func, (void *)(long)(idx + 1));
    assert(err == 0);

    err = thread_join(threads[idx + 1], &res);
    printf("join th%d->th%d = %d\n", idx, idx + 1, err);
    totalerr += err;
  } else {
    err = thread_join(threads[0], &res);
    printf("join th%d->th0 = %d\n", idx, err);
    totalerr += err;
  }

  thread_exit(NULL);
}

int main(void) {
  void *res = NULL;
  int err;

  threads[0] = thread_self();

  err = thread_create(&threads[1], chain_func, (void *)(long)1);
  assert(err == 0);

  err = thread_join(threads[1], &res);
  printf("join th0->th1 = %d\n", err);
  totalerr += err;

  printf("sum return values = %d (expected %d)\n", totalerr, EDEADLK);
  assert(totalerr == EDEADLK);

  return (totalerr == EDEADLK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
