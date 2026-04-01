#include "thread.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/* test de plein de switch pendant que N-1 threads sont bloqués dans join
 *
 * La durée du programme doit etre proportionnelle au nombre de yields
 * mais ne pas varier avec le nombre de threads donné en argument.
 *
 * support nécessaire:
 * - thread_create()
 * - thread_yield() depuis ou vers le main
 * - retour sans thread_exit()
 * - thread_join() avec récupération de la valeur de retour
 */

static unsigned long nbthrd;
static int nbyield;

static void *thfunc(void *_nbth) {
  int nbth = (intptr_t)_nbth;
  if ((unsigned long)nbth > 0) {
    thread_t th;
    int err;
    void *res;
    err = thread_create(&th, thfunc, ((char *)_nbth) - 1);
    assert(!err);
    err = thread_join(th, &res);
    assert(!err);
    assert(res == ((char *)_nbth) - 1);
  } else {
    int i;
    struct timeval tv1, tv2;
    unsigned long us;
    gettimeofday(&tv1, NULL);
    for (i = 0; i < nbyield; i++) {
      thread_yield();
    }
    gettimeofday(&tv2, NULL);
    us = (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec);
    printf("%d yield avec plein de threads dans join: %ld us\n", nbyield, us);
    printf("%ld threads créés et détruits\n", nbthrd);
    printf("GRAPH;32;%ld;%d;%lu\n", nbthrd, nbyield, us);
  }
  return _nbth;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("arguments manquants: nombre de threads, puis nombre de yield\n");
    return EXIT_FAILURE;
  }

  nbthrd = atoi(argv[1]);
  nbyield = atoi(argv[2]);

  thfunc((void *)nbthrd);

  return 0;
}