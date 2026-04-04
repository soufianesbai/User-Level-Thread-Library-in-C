#include "thread.h"
#include "thread_sem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define NB_THREADS 8
#define LOOPS 1000

static thread_sem_t sem_lock;
static thread_sem_t sem_block;
static int counter = 0;
static int gate_released = 0;

static void *inc_worker(void *arg __attribute__((unused))) {
  for (int i = 0; i < LOOPS; i++) {
    assert(thread_sem_wait(&sem_lock) == 0);
    int tmp = counter;
    thread_yield();
    counter = tmp + 1;
    assert(thread_sem_post(&sem_lock) == 0);
  }
  return NULL;
}

static void *blocked_worker(void *arg __attribute__((unused))) {
  assert(thread_sem_wait(&sem_block) == 0);
  gate_released = 1;
  return NULL;
}

int main(void) {
  thread_t th[NB_THREADS];

  assert(thread_sem_init(&sem_lock, 1) == 0);
  for (int i = 0; i < NB_THREADS; i++) {
    assert(thread_create(&th[i], inc_worker, NULL) == 0);
  }
  for (int i = 0; i < NB_THREADS; i++) {
    assert(thread_join(th[i], NULL) == 0);
  }
  assert(counter == NB_THREADS * LOOPS);
  assert(thread_sem_destroy(&sem_lock) == 0);

  assert(thread_sem_init(&sem_block, 0) == 0);
  thread_t waiter;
  assert(thread_create(&waiter, blocked_worker, NULL) == 0);

  for (int i = 0; i < 5; i++) {
    thread_yield();
  }
  assert(gate_released == 0);

  assert(thread_sem_post(&sem_block) == 0);
  assert(thread_join(waiter, NULL) == 0);
  assert(gate_released == 1);
  assert(thread_sem_destroy(&sem_block) == 0);

  printf("65-semaphore: OK\n");
  return EXIT_SUCCESS;
}
