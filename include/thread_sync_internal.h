#ifndef THREAD_SYNC_INTERNAL_H
#define THREAD_SYNC_INTERNAL_H

/*
 * Internal scheduler synchronization abstraction.
 *
 * In monocore mode these macros are no-ops so the historical behavior stays
 * unchanged. In multicore mode they protect all shared queues/state touched by
 * workers and user-level synchronization primitives.
 */
#ifdef THREAD_MULTICORE
#include <pthread.h>

extern pthread_mutex_t scheduler_lock;
extern pthread_cond_t scheduler_cond;

#define SCHED_LOCK() pthread_mutex_lock(&scheduler_lock)
#define SCHED_UNLOCK() pthread_mutex_unlock(&scheduler_lock)
#define SCHED_WAIT() pthread_cond_wait(&scheduler_cond, &scheduler_lock)
#define SCHED_SIGNAL() pthread_cond_signal(&scheduler_cond)
#define SCHED_BROADCAST() pthread_cond_broadcast(&scheduler_cond)
#else
#define SCHED_LOCK() ((void)0)
#define SCHED_UNLOCK() ((void)0)
#define SCHED_WAIT() ((void)0)
#define SCHED_SIGNAL() ((void)0)
#define SCHED_BROADCAST() ((void)0)
#endif

#endif // THREAD_SYNC_INTERNAL_H
