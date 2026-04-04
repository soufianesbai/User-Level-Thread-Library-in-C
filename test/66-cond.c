#include "thread.h"
#include "thread_cond.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static thread_mutex_t lock;
static thread_cond_t cond;

static int ready = 0;
static int value = 0;
static int consumed = 0;

static int go = 0;
static int awakened = 0;

static void *consumer(void *arg __attribute__((unused))) {
  assert(thread_mutex_lock(&lock) == 0);
  while (!ready) {
    assert(thread_cond_wait(&cond, &lock) == 0);
  }
  consumed = value;
  ready = 0;
  assert(thread_mutex_unlock(&lock) == 0);
  return NULL;
}

static void *producer(void *arg __attribute__((unused))) {
  thread_yield();
  assert(thread_mutex_lock(&lock) == 0);
  value = 42;
  ready = 1;
  assert(thread_cond_signal(&cond) == 0);
  assert(thread_mutex_unlock(&lock) == 0);
  return NULL;
}

static void *waiter(void *arg __attribute__((unused))) {
  assert(thread_mutex_lock(&lock) == 0);
  while (!go) {
    assert(thread_cond_wait(&cond, &lock) == 0);
  }
  awakened++;
  assert(thread_mutex_unlock(&lock) == 0);
  return NULL;
}

int main(void) {
  assert(thread_mutex_init(&lock) == 0);
  assert(thread_cond_init(&cond) == 0);

  thread_t c, p;
  assert(thread_create(&c, consumer, NULL) == 0);
  assert(thread_create(&p, producer, NULL) == 0);
  assert(thread_join(p, NULL) == 0);
  assert(thread_join(c, NULL) == 0);
  assert(consumed == 42);

  thread_t w1, w2;
  assert(thread_create(&w1, waiter, NULL) == 0);
  assert(thread_create(&w2, waiter, NULL) == 0);

  for (int i = 0; i < 5; i++) {
    thread_yield();
  }

  assert(thread_mutex_lock(&lock) == 0);
  go = 1;
  assert(thread_cond_broadcast(&cond) == 0);
  assert(thread_mutex_unlock(&lock) == 0);

  assert(thread_join(w1, NULL) == 0);
  assert(thread_join(w2, NULL) == 0);
  assert(awakened == 2);

  assert(thread_cond_destroy(&cond) == 0);
  assert(thread_mutex_destroy(&lock) == 0);

  printf("66-cond: OK\n");
  return EXIT_SUCCESS;
}
