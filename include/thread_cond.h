#ifndef THREAD_COND_H
#define THREAD_COND_H

#include "thread.h"

#ifndef USE_PTHREAD

typedef struct {
    struct thread_queue waiting_queue;
} thread_cond_t;

int thread_cond_init(thread_cond_t *cond);
int thread_cond_wait(thread_cond_t *cond, thread_mutex_t *mutex);
int thread_cond_signal(thread_cond_t *cond);
int thread_cond_broadcast(thread_cond_t *cond);
int thread_cond_destroy(thread_cond_t *cond);

#else

#define thread_cond_t pthread_cond_t
#define thread_cond_init(_cond) pthread_cond_init((_cond), NULL)
#define thread_cond_wait pthread_cond_wait
#define thread_cond_signal pthread_cond_signal
#define thread_cond_broadcast pthread_cond_broadcast
#define thread_cond_destroy pthread_cond_destroy

#endif

#endif
