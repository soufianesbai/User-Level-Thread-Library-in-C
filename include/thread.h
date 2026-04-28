#ifndef __THREAD_H__
#define __THREAD_H__

/* Scheduling policies (set via -DTHREAD_SCHED_POLICY=<value> at compile time) */
#define THREAD_SCHED_FIFO 0 /* round-robin: threads run in arrival order */
#define THREAD_SCHED_PRIO 1 /* priority-based: highest priority runs first */

/* Priority bounds and defaults for THREAD_SCHED_PRIO */
#define THREAD_DEFAULT_PRIORITY 10
#define THREAD_MAX_PRIORITY     100
#define THREAD_MIN_PRIORITY     0

/* Under THREAD_SCHED_PRIO, on each yield:
 * - THREAD_AGING is subtracted from the running thread's priority (prevents monopolization).
 * - THREAD_WAIT  is added to every waiting thread's priority (prevents starvation). */
#define THREAD_AGING 2
#define THREAD_WAIT  1

#ifndef THREAD_SCHED_POLICY
#define THREAD_SCHED_POLICY THREAD_SCHED_FIFO
#endif

#ifdef ENABLE_SIGNAL
#include <stdint.h>
#endif

#ifndef USE_PTHREAD

#include <sys/queue.h>

/* Doubly-linked tail queue used for ready, zombie, and wait queues. */
TAILQ_HEAD(thread_queue, thread);

/* Opaque thread handle returned by thread_create and thread_self. */
typedef void *thread_t;

/* Return the handle of the calling thread. */
extern thread_t thread_self(void);

/* Create a new thread executing func(funcarg).
 * The handle is stored in *newthread.
 * Returns 0 on success, -1 on error. */
extern int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg);

/* Yield the CPU to another ready thread. */
extern int thread_yield(void);

/* Yield the CPU directly to target if it is ready; falls back to thread_yield() otherwise. */
extern int thread_yield_to(thread_t target);

/* Block until thread terminates and store its return value in *retval.
 * If retval is NULL the return value is discarded.
 * Returns 0 on success, EDEADLK if a join cycle is detected, -1 on error. */
extern int thread_join(thread_t thread, void **retval);

/* Terminate the calling thread and make retval available to thread_join().
 * Never returns. */
extern void thread_exit(void *retval) __attribute__((__noreturn__));

typedef struct thread_mutex {
  int locked;                        /* 0 = free, 1 = held */
  struct thread_queue waiting_queue; /* threads blocked waiting to acquire this mutex */
} thread_mutex_t;

/* Initialize mutex to the unlocked state. Returns 0 on success, -1 on error. */
int thread_mutex_init(thread_mutex_t *mutex);

/* Destroy mutex. Returns -1 if threads are still waiting. */
int thread_mutex_destroy(thread_mutex_t *mutex);

/* Acquire mutex, blocking the calling thread if it is already held. */
int thread_mutex_lock(thread_mutex_t *mutex);

/* Release mutex and wake one waiting thread if any. */
int thread_mutex_unlock(thread_mutex_t *mutex);

/* Set the scheduling priority of thread t (THREAD_MIN_PRIORITY..THREAD_MAX_PRIORITY).
 * Only meaningful under THREAD_SCHED_PRIO. Returns 0 on success, -1 on error. */
int thread_set_priority(thread_t t, int prio);
int thread_set_concurrency(int nworkers);
int thread_set_affinity(thread_t thread, int worker_id);

#ifdef ENABLE_SIGNAL
/* Bitmask of up to 32 user-defined internal signals (bit N = signal N). */
typedef uint32_t thread_sigset_t;

/* Deliver signal sig to target thread. Returns 0 on success, -1 on error. */
int thread_signal_send(thread_t target, int sig);

/* Block until one of the signals in set is pending; store the signal number in *sig.
 * Returns 0 on success, -1 on error. */
int thread_sigwait(thread_sigset_t set, int *sig);
#endif

#else /* USE_PTHREAD — redirect to pthreads for compatibility */

#include <pthread.h>
#include <sched.h>
#define thread_t pthread_t
#define thread_self pthread_self
#define thread_create(th, func, arg) pthread_create(th, NULL, func, arg)
#define thread_yield sched_yield
#define thread_join pthread_join
#define thread_exit pthread_exit
#define thread_set_priority(t, prio) ((void)(t), (void)(prio), (void)0)

static inline int thread_set_concurrency(int nworkers) {
  (void)nworkers;
  return 0;
}

static inline int thread_set_affinity(thread_t thread, int worker_id) {
  (void)thread;
  (void)worker_id;
  return 0;
}
#define thread_mutex_t pthread_mutex_t
#define thread_mutex_init(_mutex) pthread_mutex_init(_mutex, NULL)
#define thread_mutex_destroy pthread_mutex_destroy
#define thread_mutex_lock pthread_mutex_lock
#define thread_mutex_unlock pthread_mutex_unlock

#ifdef ENABLE_SIGNAL
/* Signal functions are not supported under USE_PTHREAD; calls return -1. */
typedef uint32_t thread_sigset_t;
#define thread_signal_send(target, sig) ((void)(target), (void)(sig), -1)
#define thread_sigwait(set, sig) ((void)(set), (void)(sig), -1)
#endif

#endif /* USE_PTHREAD */

#endif /* __THREAD_H__ */