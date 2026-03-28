#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include "thread.h"

/* test pour verifier que prendre un mutex ne desactive pas l'equité envers les autres threads.
 * si ca deadlock, c'est qu'il y a un problème.
 * la durée du test n'est pas clairement utilisable pour juger de l'équité.
 *
 * valgrind doit etre content.
 * Le programme doit finir.
 *
 * support nécessaire:
 * - thread_create()
 * - thread_join() sans récupération de la valeur de retour
 * - thread_mutex_init()
 * - thread_mutex_destroy()
 * - thread_mutex_lock()
 * - thread_mutex_unlock()
 */

int fini = 0;
thread_mutex_t lock;

static void * thfunc(void *dummy __attribute__((unused)))
{
    unsigned i;

    /* on incremente progressivement fini jusque 5 pour debloquer le main */
    for(i=0; i<5; i++) {
      printf("  le fils yield sans le mutex et incrémente le compteur %u\n", fini);
      thread_yield();
      fini++;
    }

    /* on attend que main remette à 0 */
    thread_mutex_lock(&lock);
    while (fini != 0) {
      printf("  le fils yield avec le mutex en attendant que le compteur %u soit 0\n", fini);
      thread_yield();
    }
    thread_mutex_unlock(&lock);

    thread_exit(NULL);
}

int main(void)
{
  thread_t th;
  int err, i;

  /* on cree le mutex et le thread */
  if (thread_mutex_init(&lock) != 0) {
      fprintf(stderr, "thread_mutex_init failed\n");
      return EXIT_FAILURE;
  }
  err = thread_create(&th, thfunc, NULL);
  assert(!err);

  /* on prend le lock puis on attend que l'autre mette fini = 5 */
  thread_mutex_lock(&lock);
  while (fini != 5) {
    printf("le père yield avec le mutex en attendant que le compteur %u soit 5\n", fini);
    thread_yield();
  }
  thread_mutex_unlock(&lock);

  /* on baisse progressivement jusque 0 */
  for(i=0; i<5; i++) {
    printf("le père yield sans le mutex et décrémente le compteur %u\n", fini);
    thread_yield();
    fini--;
  }

  /* fini */
  err = thread_join(th, NULL);
  assert(!err);
  thread_mutex_destroy(&lock);

  printf("terminé OK\n");
  return EXIT_SUCCESS;
}