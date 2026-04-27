#include "thread_sem.h"
#include "preemption.h"
#include "thread_sync_internal.h"
#include "thread_internal.h"
#include <stddef.h>
#include <sys/queue.h>

/*
 * thread_sem_init — Initialize a semaphore.
 *
 * value : number of available resources (must be >= 0).
 *
 * Returns 0 on success, -1 on error.
 */
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
  sem->count = 0;
  return 0;
}

/*
 * thread_sem_wait — P() operation (decrement).
 *
 * If count > 0 : decrement and continue immediately.
 * If count == 0 : block the current thread in the waiting_queue until
 * a thread_sem_post() wakes it up.
 */
int thread_sem_wait(thread_sem_t *sem) {
  if (sem == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  if (sem->count > 0) {
    // Fast path : la ressource est disponible
    SCHED_LOCK();
    sem->count--;
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return 0;
  }

  // Slow path : on se bloque jusqu'à ce qu'un post() nous réveille
  thread *prev = thread_get_current();

  SCHED_LOCK();
  thread *next = thread_scheduler_pick_next_locked();

  if (next == NULL) {
    SCHED_UNLOCK();
    // Aucun thread ne peut faire un post() — deadlock
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    return -1;
  }

  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&sem->waiting_queue, prev, entries);

  SCHED_UNLOCK();

  thread_set_current(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);

  // Quand on revient ici, post() nous a réveillés et a déjà décrémenté count.
#ifdef ENABLE_PREEMPTION
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

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  SCHED_LOCK();
  if (!TAILQ_EMPTY(&sem->waiting_queue)) {
    // Transfert direct : le thread réveillé obtient la ressource,
    // count ne change pas (même logique que mutex_unlock).
    thread *revived = TAILQ_FIRST(&sem->waiting_queue);
    TAILQ_REMOVE(&sem->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue_locked(revived);
  } else {
    sem->count++;
  }
  SCHED_UNLOCK();

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}