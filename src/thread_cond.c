#include "thread_cond.h"
#include "preemption.h"
#include "thread_sync_internal.h"
#include "thread_internal.h"
#include <stddef.h>
#include <sys/queue.h>

int thread_cond_init(thread_cond_t *cond) {
  if (cond == NULL)
    return -1;
  TAILQ_INIT(&cond->waiting_queue);
  return 0;
}

int thread_cond_destroy(thread_cond_t *cond) {
  if (cond == NULL || !TAILQ_EMPTY(&cond->waiting_queue))
    return -1;
  return 0;
}

/*
 * thread_cond_wait — attend que la condition soit signalée.
 *
 * Protocole atomique :
 *   1. Relâche le mutex (pour laisser d'autres threads modifier l'état partagé)
 *   2. Bloque le thread courant dans la waiting_queue de la cond
 *   3. Quand signal/broadcast nous réveille, réacquiert le mutex avant
 *      de retourner à l'appelant.
 *
 * Les étapes 1 et 2 sont faites sous preem_block pour éviter qu'un signal
 * arrive entre le unlock et le blocage (lost wakeup).
 */
int thread_cond_wait(thread_cond_t *cond, thread_mutex_t *mutex) {
  if (cond == NULL || mutex == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  thread *prev = thread_get_current();
  SCHED_LOCK();
  thread *next = thread_scheduler_pick_next_locked();

  // Relâche le mutex avant de se bloquer
  // On appelle la logique interne directement pour rester sous preem_block.
  // Si des threads attendent le mutex, on réveille le premier.
  if (!TAILQ_EMPTY(&mutex->waiting_queue)) {
    thread *revived = TAILQ_FIRST(&mutex->waiting_queue);
    TAILQ_REMOVE(&mutex->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue_locked(revived);
    // locked reste à 1 : le thread réveillé prend le mutex
  } else {
    mutex->locked = 0;
  }

  // Bloque dans la cond, le mutex est déjà relâché ci-dessus
  prev->state = THREAD_BLOCKED;
  TAILQ_INSERT_TAIL(&cond->waiting_queue, prev, entries);

#ifdef THREAD_MULTICORE
  {
    thread *worker_stub = thread_scheduler_get_worker_stub();
    if (worker_stub != NULL) {
      if (next != NULL) {
        next->state = THREAD_READY;
        thread_scheduler_enqueue_locked(next);
      }
      SCHED_UNLOCK();
      fast_swap_context(&prev->context, &worker_stub->context);
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      thread_mutex_lock(mutex);
      return 0;
    }
  }
#endif

  if (next == NULL) {
    TAILQ_REMOVE(&cond->waiting_queue, prev, entries);
    prev->state = THREAD_RUNNING;
    SCHED_UNLOCK();
#ifdef ENABLE_PREEMPTION
    preem_unblock();
#endif
    thread_mutex_lock(mutex);
    return -1;
  }

  SCHED_UNLOCK();

  thread_set_current(next);
  next->state = THREAD_RUNNING;
  fast_swap_context(&prev->context, &next->context);

  // Quand on revient ici, signal/broadcast nous a remis dans la ready_queue.
  // Il faut réacquérir le mutex avant de retourner.
#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  thread_mutex_lock(mutex);
  return 0;
}

/*
 * thread_cond_signal — réveille un thread en attente sur la condition.
 *
 * Le thread réveillé est remis dans la ready_queue. Il devra réacquérir
 * le mutex dans thread_cond_wait() avant de continuer.
 */
int thread_cond_signal(thread_cond_t *cond) {
  if (cond == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  SCHED_LOCK();
  if (!TAILQ_EMPTY(&cond->waiting_queue)) {
    thread *revived = TAILQ_FIRST(&cond->waiting_queue);
    TAILQ_REMOVE(&cond->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue_locked(revived);
  }
  SCHED_UNLOCK();

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}

/*
 * thread_cond_broadcast — réveille tous les threads en attente sur la
 * condition.
 */
int thread_cond_broadcast(thread_cond_t *cond) {
  if (cond == NULL)
    return -1;

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  SCHED_LOCK();
  while (!TAILQ_EMPTY(&cond->waiting_queue)) {
    thread *revived = TAILQ_FIRST(&cond->waiting_queue);
    TAILQ_REMOVE(&cond->waiting_queue, revived, entries);
    revived->state = THREAD_READY;
    thread_scheduler_enqueue_locked(revived);
  }
  SCHED_UNLOCK();

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif
  return 0;
}