#include "thread.h"
#include <assert.h>
#include <stdio.h>

#define N 20

static int trace[N];
static int idx = 0;

void log_exec(int id) {
  if (idx < N)
    trace[idx++] = id;
}

void *high(void *arg) {
  (void)arg;

  for (int i = 0; i < 3; i++) {
    log_exec(2);
    thread_yield();
  }

  return NULL;
}

void *mid(void *arg) {
  (void)arg;

  for (int i = 0; i < 3; i++) {
    log_exec(3);
    thread_yield();
  }

  return NULL;
}

void *low(void *arg) {
  (void)arg;

  for (int i = 0; i < 3; i++) {
    log_exec(1);
    thread_yield();
  }

  return NULL;
}

int main(void) {
  thread_t t1, t2, t3;

  thread_create(&t1, low, NULL);
  thread_create(&t2, high, NULL);
  thread_create(&t3, mid, NULL);

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO
  thread_set_priority(t1, 10);  // low
  thread_set_priority(t2, 80);  // high
  thread_set_priority(t3, 40);  // mid
#endif

  thread_join(t1, NULL);
  thread_join(t2, NULL);
  thread_join(t3, NULL);

  printf("Trace: ");
  for (int i = 0; i < idx; i++)
    printf("%d ", trace[i]);
  printf("\n");

#if THREAD_SCHED_POLICY == THREAD_SCHED_PRIO

  // ---- TEST 1: domination stricte initiale ----
  // high doit monopoliser au début
  assert(trace[0] == 2);
  assert(trace[1] == 2);
  assert(trace[2] == 2);

  // ---- TEST 2: ordre global ----
  // aucun low avant mid tant que mid existe
  int seen_mid = 0;
  for (int i = 0; i < idx; i++) {
    if (trace[i] == 3)
      seen_mid = 1;

    if (trace[i] == 1) {
      assert(seen_mid); // low ne doit pas passer avant mid
    }
  }

  // ---- TEST 3: fréquence ----
  int c1 = 0, c2 = 0, c3 = 0;
  for (int i = 0; i < idx; i++) {
    if (trace[i] == 1) c1++;
    if (trace[i] == 2) c2++;
    if (trace[i] == 3) c3++;
  }

  assert(c1 == 3);
  assert(c2 == 3);
  assert(c3 == 3);

  printf("OK: PRIO scheduling robuste\n");

#else

  // FIFO strict: ordre de création
  // t1 -> t2 -> t3 en boucle

  assert(trace[0] == 1);
  assert(trace[1] == 2);
  assert(trace[2] == 3);

  printf("OK: FIFO stable\n");

#endif

  return 0;
}