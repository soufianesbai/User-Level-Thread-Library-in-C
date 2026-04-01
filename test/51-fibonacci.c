#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/* fibonacci.
 *
 * la durée doit être proportionnel à la valeur du résultat.
 * valgrind doit être content.
 * jusqu'à quelle valeur cela fonctionne-t-il ?
 *
 * support nécessaire:
 * - thread_create()
 * - thread_join() avec récupération de la valeur de retour
 * - retour sans thread_exit()
 */

static void *fibo(void *_value) {
  thread_t th, th2;
  int err;
  void *res = NULL, *res2 = NULL;
  unsigned long value = (unsigned long)_value;

  /* on passe un peu la main aux autres pour eviter de faire uniquement la
   * partie gauche de l'arbre */
  thread_yield();

  if (value < 3)
    return (void *)1;

  err = thread_create(&th, fibo, (void *)(value - 1));
  assert(!err);
  err = thread_create(&th2, fibo, (void *)(value - 2));
  assert(!err);

  err = thread_join(th, &res);
  assert(!err);
  err = thread_join(th2, &res2);
  assert(!err);

  return (void *)((unsigned long)res + (unsigned long)res2);
}

unsigned long fibo_checker(unsigned long n) {
  unsigned long a = 1;
  unsigned long b = 1;
  unsigned long c, i;

  if (n <= 2) {
    return 1;
  }

  for (i = 2; i < n; i++) {
    c = a + b;
    a = b;
    b = c;
  }
  return c;
}

int main(int argc, char *argv[]) {
  unsigned long value, res;
  struct timeval tv1, tv2;
  double s;

  if (argc < 2) {
    printf("argument manquant: entier x pour lequel calculer fibonacci(x)\n");
    return EXIT_FAILURE;
  }

  value = atoi(argv[1]);
  gettimeofday(&tv1, NULL);
  res = (unsigned long)fibo((void *)value);
  gettimeofday(&tv2, NULL);
  s = (tv2.tv_sec - tv1.tv_sec) + (tv2.tv_usec - tv1.tv_usec) * 1e-6;

  if (res != fibo_checker(value)) {
    printf("fibo(%lu) != %lu (FAILED)\n", value, fibo_checker(value));
    printf("GRAPH;%lu;%lu;%e;%s\n", value, res, s, "FAILED");
    return EXIT_FAILURE;
  } else {
    printf("fibo(%lu) = %lu en %e s\n", value, res, s);
    printf("GRAPH;%lu;%lu;%e;%s\n", value, res, s, "SUCCESS");
    return EXIT_SUCCESS;
  }
}