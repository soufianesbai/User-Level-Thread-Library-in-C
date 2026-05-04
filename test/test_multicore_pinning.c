#include "thread.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Test thread pinning: threads created with thread_set_affinity() pinned to
 * a specific worker should complete correctly and not interfere with each
 * other across workers. Each group of threads is pinned to one worker and
 * shares a per-worker counter protected by a mutex — if a thread runs on the
 * wrong worker the counter invariant would still hold (correctness), and we
 * verify the final totals. */

#define N_WORKERS 4
#define THREADS_PER_WORKER 8
#define ITERS 5000

static thread_mutex_t locks[N_WORKERS];
static int counters[N_WORKERS];

static void *pinned_worker(void *arg) {
  int wid = (int)(uintptr_t)arg;

  for (int i = 0; i < ITERS; i++) {
    thread_mutex_lock(&locks[wid]);
    counters[wid]++;
    thread_mutex_unlock(&locks[wid]);
    if ((i & 63) == 0)
      thread_yield();
  }
  return (void *)1;
}

int main(void) {
  thread_set_concurrency(N_WORKERS);

  for (int w = 0; w < N_WORKERS; w++) {
    thread_mutex_init(&locks[w]);
    counters[w] = 0;
  }

  thread_t th[N_WORKERS][THREADS_PER_WORKER];

  for (int w = 0; w < N_WORKERS; w++) {
    for (int i = 0; i < THREADS_PER_WORKER; i++) {
      assert(thread_create(&th[w][i], pinned_worker, (void *)(uintptr_t)w) == 0);
      thread_set_affinity(th[w][i], w);
    }
  }

  for (int w = 0; w < N_WORKERS; w++)
    for (int i = 0; i < THREADS_PER_WORKER; i++)
      assert(thread_join(th[w][i], NULL) == 0);

  for (int w = 0; w < N_WORKERS; w++) {
    assert(counters[w] == THREADS_PER_WORKER * ITERS);
    thread_mutex_destroy(&locks[w]);
  }

  printf("test_multicore_pinning: ok\n");
  return 0;
}
