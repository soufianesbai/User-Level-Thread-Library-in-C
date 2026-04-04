#include "thread_sem.h"
#include "preemption.h"
#include "thread_internal.h"
#include <stddef.h>
#include <sys/queue.h>
#include <ucontext.h>

int thread_sem_init(thread_sem_t *sem, int value) {
  if (sem == NULL || value < 0)
    return -1;
  sem->count = value;
  TAILQ_INIT(&sem->waiting_queue);
  return 0;
}

int thread_sem_destroy(thread_sem_t *sem) {
  if (sem == NULL || !TAILQ_EMPTY(&sem->waiting_queue))
    return -1;
  return 0;
}

/*
 * thread_sem_wait — P() operation (décrémente).
 *
 * Si count > 0 : décrémente et continue immédiatement.
 * Si count == 0 : bloque le thread courant dans la waiting_queue jusqu'à
 * ce qu'un thread_sem_post() le réveille.
 */
int thread_sem_wait(thread_sem_t *sem) {
  if (sem == NULL)
    return -1;

#ifdef PREEM_ENABLED
  preem_block();
#endif

  if (sem->count > 0) {
    // Fast path : la ressource est disponible
    sem->count--;
#ifdef PREEM_ENABLED
    preem_unblock();
#endif
    return 0;
  }

  // Slow path : on se bloque jusqu'à ce qu'un post() nous réveille
  struct thread_queue *ready_queue = thread_get_ready_queue();
  thread *prev = thread_get_current_thread();
  thread *next = TAILQ_FIRST(ready_queue);

  if (next == NULL) {
    // Aucun thread ne peut faire un post() — deadlock
#ifdef PREEM_ENABLED
    preem_unblock();
#endif
    return -1;
  }

  TAILQ_REMOVE(ready_queue, next, entries);

  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&sem->waiting_queue, prev, entries);

  thread_set_current_thread(next);
  next->state = THREAD_RUNNING;
  swapcontext(&prev->context, &next->context);

  // Quand on revient ici, post() nous a réveillés et a déjà décrémenté count.
#ifdef PREEM_ENABLED
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_sem_post — V() operation (incrémente).
 *
 * Si des threads attendent : réveille le premier de la file et lui
 * transfère directement la ressource (count reste inchangé).
 * Sinon : incrémente count.
 */
int thread_sem_post(thread_sem_t *sem) {
  if (sem == NULL)
    return -1;

#ifdef PREEM_ENABLED
  preem_block();
#endif

  if (!TAILQ_EMPTY(&sem->waiting_queue)) {
    // Transfert direct : le thread réveillé obtient la ressource,
    // count ne change pas (même logique que mutex_unlock).
    struct thread_queue *ready_queue = thread_get_ready_queue();
    thread *revived = TAILQ_FIRST(&sem->waiting_queue);
    TAILQ_REMOVE(&sem->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    TAILQ_INSERT_TAIL(ready_queue, revived, entries);
  } else {
    sem->count++;
  }

#ifdef PREEM_ENABLED
  preem_unblock();
#endif
  return 0;
}