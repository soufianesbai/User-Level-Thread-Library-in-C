#ifndef THREAD_COND_H
#define THREAD_COND_H

#include "thread.h"

typedef struct {
    struct thread_queue waiting_queue;
} thread_cond_t;

int thread_cond_init(thread_cond_t *cond);
int thread_cond_wait(thread_cond_t *cond, thread_mutex_t *mutex);
int thread_cond_signal(thread_cond_t *cond);
int thread_cond_broadcast(thread_cond_t *cond);
int thread_cond_destroy(thread_cond_t *cond);

#endif
