#include "thread.h"
#include <assert.h>
#include <stdio.h>

/* Test de détection de débordement de pile.
 *
 * Un thread provoque un débordement de pile par récursion infinie avec de
 * grandes trames. Avec THREAD_ENABLE_GUARD_PAGE=1 et
 * THREAD_ENABLE_OVERFLOW_DETECTION, le handler SIGSEGV doit terminer le thread
 * sans tuer le programme entier.
 *
 * Doit être compilé avec la bibliothèque overflow (make overflow-tests).
 *
 * support nécessaire:
 * - thread_create(), thread_join()
 * - guard page (THREAD_ENABLE_GUARD_PAGE=1)
 * - THREAD_ENABLE_OVERFLOW_DETECTION
 */

#if THREAD_ENABLE_GUARD_PAGE && defined(THREAD_ENABLE_OVERFLOW_DETECTION)

static volatile char sink;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static void overflow(int depth) {
  char buf[512];
  buf[0] = (char)depth;
  sink = buf[0]; /* read forces the frame to exist; suppresses unused-variable warning */
  overflow(depth + 1);
}
#pragma GCC diagnostic pop

static void *overflow_thread(void *arg __attribute__((unused))) {
  overflow(0);
  return NULL; /* unreachable */
}

int main(void) {
  thread_t th;
  int err;

  err = thread_create(&th, overflow_thread, NULL);
  assert(!err);

  /* The SIGSEGV handler terminates the thread cleanly via thread_exit(). */
  err = thread_join(th, NULL);
  assert(!err);

  printf("72-stack-overflow: OK\n");
  return 0;
}

#else

int main(void) {
  printf("72-stack-overflow: skipped (THREAD_ENABLE_GUARD_PAGE or "
         "THREAD_ENABLE_OVERFLOW_DETECTION not enabled)\n");
  return 0;
}

#endif
