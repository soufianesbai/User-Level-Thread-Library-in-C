#include "thread.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

/* Test des cas d'erreur du mutex.
 *
 * - thread_mutex_destroy() avec un thread en attente renvoie -1.
 * - thread_mutex_lock() sur un mutex détruit renvoie -1 (errno=EINVAL).
 * - thread_mutex_unlock() sur un mutex détruit renvoie -1 (errno=EINVAL).
 *
 * valgrind doit être content.
 * Le programme doit finir.
 *
 * support nécessaire:
 * - thread_create(), thread_join(), thread_yield()
 * - thread_mutex_init(), thread_mutex_destroy()
 * - thread_mutex_lock(), thread_mutex_unlock()
 */

static thread_mutex_t mutex;

static void *waiter(void *arg __attribute__((unused))) {
  /* mutex is locked by main — this thread parks in the waiting queue. */
  int err = thread_mutex_lock(&mutex);
  assert(!err);
  thread_mutex_unlock(&mutex);
  return NULL;
}

int main(void) {
  thread_t th;
  int err;

  /* --- 1: destroy with a waiter must fail --- */
  err = thread_mutex_init(&mutex);
  assert(!err);

  err = thread_mutex_lock(&mutex);
  assert(!err);

  err = thread_create(&th, waiter, NULL);
  assert(!err);

  /* Let the waiter run and park itself in mutex.waiting_queue. */
  thread_yield();

  err = thread_mutex_destroy(&mutex);
  assert(err == -1);
  printf("68-mutex-errors: destroy with waiter correctly returned -1\n");

  /* Release the waiter so it can finish. */
  thread_mutex_unlock(&mutex);
  thread_join(th, NULL);

  /* --- 2: lock/unlock on a destroyed mutex must set EINVAL --- */
  err = thread_mutex_init(&mutex);
  assert(!err);
  err = thread_mutex_destroy(&mutex);
  assert(!err);

  errno = 0;
  err = thread_mutex_lock(&mutex);
  assert(err == -1 && errno == EINVAL);
  printf("68-mutex-errors: lock on destroyed mutex correctly returned EINVAL\n");

  errno = 0;
  err = thread_mutex_unlock(&mutex);
  assert(err == -1 && errno == EINVAL);
  printf("68-mutex-errors: unlock on destroyed mutex correctly returned EINVAL\n");

  printf("68-mutex-errors: OK\n");
  return 0;
}
