/*
 * 67-signal — test the internal thread signal API (thread_signal_send /
 * thread_sigwait).
 *
 * Compile with -DENABLE_SIGNAL to activate the signal subsystem; without it
 * the test reports "skipped" and exits successfully so make check always
 * passes.
 *
 * Test plan:
 *   1. Blocking sigwait: waiter blocks, main sends signal, waiter unblocks.
 *   2. Pre-pending signal: signal sent before sigwait; sigwait returns
 *      immediately without blocking.
 *   3. Two threads wait for different signals: routing is correct (each thread
 *      receives exactly the signal it was waiting for).
 *   4. Set of signals: thread waits on {SIG1|SIG2}, receives whichever arrives.
 */
#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(ENABLE_SIGNAL) || defined(USE_PTHREAD)

int main(void) {
  printf("67-signal: skipped (recompile with ENABLE_SIGNAL=1)\n");
  return EXIT_SUCCESS;
}

#else

#define SIG1 1
#define SIG2 2
#define SIG3 3
#define SIG4 4
#define SIGBIT(s) ((thread_sigset_t)(1u << ((unsigned int)(s) - 1u)))

/* --- Test 1: blocking sigwait ------------------------------------------ */
static int t1_received;

static void *t1_waiter(void *arg __attribute__((unused))) {
  int sig;
  assert(thread_sigwait(SIGBIT(SIG1), &sig) == 0);
  assert(sig == SIG1);
  t1_received = 1;
  return NULL;
}

static void test_basic_wait(void) {
  thread_t th;
  t1_received = 0;
  assert(thread_create(&th, t1_waiter, NULL) == 0);
  thread_yield(); /* let waiter reach sigwait and block */
  assert(t1_received == 0);
  assert(thread_signal_send(th, SIG1) == 0);
  assert(thread_join(th, NULL) == 0);
  assert(t1_received == 1);
}

/* --- Test 2: pre-pending signal ---------------------------------------- */
static int t2_received;

static void *t2_waiter(void *arg __attribute__((unused))) {
  int sig;
  /* Signal was already delivered before we entered sigwait. */
  assert(thread_sigwait(SIGBIT(SIG2), &sig) == 0);
  assert(sig == SIG2);
  t2_received = 1;
  return NULL;
}

static void test_prepend_signal(void) {
  thread_t th;
  t2_received = 0;
  assert(thread_create(&th, t2_waiter, NULL) == 0);
  /* Send before the thread gets a chance to run. Signal stays pending in
   * the thread's bitmask and sigwait will consume it immediately. */
  assert(thread_signal_send(th, SIG2) == 0);
  assert(thread_join(th, NULL) == 0);
  assert(t2_received == 1);
}

/* --- Test 3: two threads, different signals ----------------------------- */
static int t3_sig_a, t3_sig_b;

static void *t3_waiter_a(void *arg __attribute__((unused))) {
  assert(thread_sigwait(SIGBIT(SIG3), &t3_sig_a) == 0);
  return NULL;
}

static void *t3_waiter_b(void *arg __attribute__((unused))) {
  assert(thread_sigwait(SIGBIT(SIG4), &t3_sig_b) == 0);
  return NULL;
}

static void test_two_waiters(void) {
  thread_t tha, thb;
  t3_sig_a = t3_sig_b = 0;
  assert(thread_create(&tha, t3_waiter_a, NULL) == 0);
  assert(thread_create(&thb, t3_waiter_b, NULL) == 0);
  thread_yield();
  thread_yield();
  /* Send in reverse order: signal routing must be per-thread. */
  assert(thread_signal_send(thb, SIG4) == 0);
  assert(thread_signal_send(tha, SIG3) == 0);
  assert(thread_join(tha, NULL) == 0);
  assert(thread_join(thb, NULL) == 0);
  assert(t3_sig_a == SIG3);
  assert(t3_sig_b == SIG4);
}

/* --- Test 4: sigwait on a set of signals ------------------------------- */
static int t4_received_sig;

static void *t4_waiter(void *arg __attribute__((unused))) {
  assert(thread_sigwait(SIGBIT(SIG1) | SIGBIT(SIG2), &t4_received_sig) == 0);
  return NULL;
}

static void test_signal_set(void) {
  thread_t th;
  t4_received_sig = 0;
  assert(thread_create(&th, t4_waiter, NULL) == 0);
  thread_yield();
  /* Send only SIG2: thread is waiting on {SIG1|SIG2} so it must receive SIG2. */
  assert(thread_signal_send(th, SIG2) == 0);
  assert(thread_join(th, NULL) == 0);
  assert(t4_received_sig == SIG2);
}

int main(void) {
  test_basic_wait();
  test_prepend_signal();
  test_two_waiters();
  test_signal_set();
  printf("67-signal: OK\n");
  return EXIT_SUCCESS;
}

#endif /* ENABLE_SIGNAL && !USE_PTHREAD */
