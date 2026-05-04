#include "thread.h"
#include "thread_cond.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Test 1: thread_cond_signal wakes exactly one waiter at a time.
 * N threads block on the condition; main signals N times and joins all. */

static thread_mutex_t m1;
static thread_cond_t c1;
static int woken1 = 0;

static void *signal_waiter(void *arg __attribute__((unused))) {
  thread_mutex_lock(&m1);
  thread_cond_wait(&c1, &m1);
  woken1++;
  thread_mutex_unlock(&m1);
  return NULL;
}

/* Test 2: thread_cond_broadcast wakes all waiters at once.
 * N threads block; main waits for all to be parked, then broadcasts. */

static thread_mutex_t m2;
static thread_cond_t c2;
static int ready2 = 0;
static int woken2 = 0;

static void *broadcast_waiter(void *arg __attribute__((unused))) {
  thread_mutex_lock(&m2);
  ready2++;
  thread_cond_wait(&c2, &m2);
  woken2++;
  thread_mutex_unlock(&m2);
  return NULL;
}

int main(void) {
  thread_set_concurrency(4);

  /* --- test 1: signal --- */
  thread_mutex_init(&m1);
  thread_cond_init(&c1);

  enum { N1 = 8 };
  thread_t th1[N1];
  for (int i = 0; i < N1; i++)
    assert(thread_create(&th1[i], signal_waiter, NULL) == 0);

  for (int i = 0; i < N1; i++) {
    thread_yield();
    thread_mutex_lock(&m1);
    thread_cond_signal(&c1);
    thread_mutex_unlock(&m1);
  }

  for (int i = 0; i < N1; i++)
    assert(thread_join(th1[i], NULL) == 0);

  assert(woken1 == N1);

  /* --- test 2: broadcast --- */
  thread_mutex_init(&m2);
  thread_cond_init(&c2);

  enum { N2 = 12 };
  thread_t th2[N2];
  for (int i = 0; i < N2; i++)
    assert(thread_create(&th2[i], broadcast_waiter, NULL) == 0);

  /* Spin until all waiters are parked inside thread_cond_wait. */
  thread_mutex_lock(&m2);
  while (ready2 < N2) {
    thread_mutex_unlock(&m2);
    thread_yield();
    thread_mutex_lock(&m2);
  }
  thread_cond_broadcast(&c2);
  thread_mutex_unlock(&m2);

  for (int i = 0; i < N2; i++)
    assert(thread_join(th2[i], NULL) == 0);

  assert(woken2 == N2);

  thread_cond_destroy(&c1);
  thread_mutex_destroy(&m1);
  thread_cond_destroy(&c2);
  thread_mutex_destroy(&m2);

  printf("test_multicore_cond: ok\n");
  return 0;
}
