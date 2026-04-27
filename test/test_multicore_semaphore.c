#include "thread.h"
#include "thread_sem.h"
#include <stdint.h>
#include <stdio.h>

static thread_sem_t slots;
static thread_sem_t items;
static thread_mutex_t lock;

static int produced = 0;
static int consumed = 0;

static void *producer(void *arg) {
  int count = (int)(uintptr_t)arg;

  for (int i = 0; i < count; ++i) {
    if (thread_sem_wait(&slots) != 0) {
      return (void *)0;
    }

    if (thread_mutex_lock(&lock) != 0) {
      return (void *)0;
    }
    produced++;
    if (thread_mutex_unlock(&lock) != 0) {
      return (void *)0;
    }

    if (thread_sem_post(&items) != 0) {
      return (void *)0;
    }

    if ((i & 15) == 0) {
      thread_yield();
    }
  }

  return (void *)1;
}

static void *consumer(void *arg) {
  int count = (int)(uintptr_t)arg;

  for (int i = 0; i < count; ++i) {
    if (thread_sem_wait(&items) != 0) {
      return (void *)0;
    }

    if (thread_mutex_lock(&lock) != 0) {
      return (void *)0;
    }
    consumed++;
    if (thread_mutex_unlock(&lock) != 0) {
      return (void *)0;
    }

    if (thread_sem_post(&slots) != 0) {
      return (void *)0;
    }

    if ((i & 15) == 0) {
      thread_yield();
    }
  }

  return (void *)1;
}

int main(void) {
  thread_set_concurrency(4);

  enum {
    PRODUCERS = 6,
    CONSUMERS = 6,
    OPS_PER_THREAD = 8000,
    CAPACITY = 32
  };

  if (thread_sem_init(&slots, CAPACITY) != 0) {
    return 1;
  }
  if (thread_sem_init(&items, 0) != 0) {
    return 2;
  }
  if (thread_mutex_init(&lock) != 0) {
    return 3;
  }

  thread_t prod[PRODUCERS];
  thread_t cons[CONSUMERS];

  for (int i = 0; i < PRODUCERS; ++i) {
    if (thread_create(&prod[i], producer, (void *)(uintptr_t)OPS_PER_THREAD) != 0) {
      return 4;
    }
  }
  for (int i = 0; i < CONSUMERS; ++i) {
    if (thread_create(&cons[i], consumer, (void *)(uintptr_t)OPS_PER_THREAD) != 0) {
      return 5;
    }
  }

  for (int i = 0; i < PRODUCERS; ++i) {
    void *ret = NULL;
    if (thread_join(prod[i], &ret) != 0 || (uintptr_t)ret != 1u) {
      return 6;
    }
  }
  for (int i = 0; i < CONSUMERS; ++i) {
    void *ret = NULL;
    if (thread_join(cons[i], &ret) != 0 || (uintptr_t)ret != 1u) {
      return 7;
    }
  }

  if (produced != PRODUCERS * OPS_PER_THREAD) {
    return 8;
  }
  if (consumed != CONSUMERS * OPS_PER_THREAD) {
    return 9;
  }

  if (thread_mutex_destroy(&lock) != 0) {
    return 10;
  }
  if (thread_sem_destroy(&items) != 0) {
    return 11;
  }
  if (thread_sem_destroy(&slots) != 0) {
    return 12;
  }

  printf("test_multicore_semaphore: ok\n");
  return 0;
}
