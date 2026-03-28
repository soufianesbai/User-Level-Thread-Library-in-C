#include <stdio.h>
#include <assert.h>
#include "thread.h"

/* test du join d'un thread qui fait plein de yield().
 *
 * le programme doit retourner correctement.
 * valgrind doit être content.
 *
 * support nécessaire:
 * - thread_create()
 * - thread_exit()
 * - thread_join() avec récupération valeur de retour, avec thread_exit()
 *   sur un thread qui yield() plusieurs fois vers celui qui joine.
 */

static void * thfunc(void *dummy __attribute__((unused)))
{
  unsigned i;
  for(i=0; i<10; i++) {
    printf("  le fils yield\n");
    thread_yield();
  }
  thread_exit((void*)0xdeadbeef);
  return NULL; /* unreachable, shut up the compiler */
}

int main()
{
  thread_t th;
  int err;
  void *res = NULL;

  err = thread_create(&th, thfunc, NULL);
  assert(!err);

  printf("le main joine...\n");
  err = thread_join(th, &res);
  assert(!err);
  assert(res == (void*) 0xdeadbeef);

  printf("join OK\n");
  return 0;
}