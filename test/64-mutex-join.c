#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include "thread.h"

/* test pour verifier que prendre un mutex ne casse pas le join.
 * si ca deadlock, c'est qu'il y a un problème.
 * la durée du test n'est pas clairement utilisable pour juger de la qualité de l'implementation.
 *
 * valgrind doit etre content.
 * Le programme doit finir.
 *
 * support nécessaire:
 * - thread_create()
 * - thread_join() dans un mutex
 * - thread_mutex_init()
 * - thread_mutex_destroy()
 * - thread_mutex_lock()
 * - thread_mutex_unlock()
 */

int pret = 0;
thread_mutex_t lock;

static void * thfunc(void *dummy __attribute__((unused)))
{
    unsigned i;
    int err;

    /* on prend le verrou puis on se marque pret */
    err = thread_mutex_lock(&lock);
    assert(!err);

    pret = 1;
    printf("  fils: verrouillé\n");
    for(i=0; i<5; i++) {
      printf("  fils: yield() pour verifier que le père n'arrive pas aller plus loin\n");
      thread_yield();
    }
    printf("  fils: va déverrouiller\n");
    pret = 2;
    err = thread_mutex_unlock(&lock);
    assert(!err);

    for(i=0; i<5; i++) {
      printf("  fils: yield() avant de finir\n");
      thread_yield();
    }
    thread_exit((void*) 0xdeadbeef);
}

int main(void)
{
  thread_t th;
  int err;
  void *ret;

  /* on cree le mutex et le thread */
  if (thread_mutex_init(&lock) != 0) {
      fprintf(stderr, "thread_mutex_init failed\n");
      return EXIT_FAILURE;
  }
  err = thread_create(&th, thfunc, NULL);
  assert(!err);

  printf("pere: attend que le fils ait démarré\n");
  while (pret == 0)
    thread_yield();

  printf("pere: fils pret, on verrouille\n");
  err = thread_mutex_lock(&lock);
  assert(!err);
  printf("pere: verrouillé, on joine\n");
  err = thread_join(th, &ret);
  assert(!err);
  assert(ret == (void*)0xdeadbeef);
  assert(pret == 2);
  printf("pere: join réussi\n");

  err = thread_mutex_unlock(&lock);
  assert(!err);
  thread_mutex_destroy(&lock);

  printf("terminé OK\n");
  return EXIT_SUCCESS;
}