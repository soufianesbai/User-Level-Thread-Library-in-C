#ifndef THREAD_SEM_H
#define THREAD_SEM_H

#include "thread.h"

typedef struct {
    int count;
    struct thread_queue waiting_queue;
} thread_sem_t;

int thread_sem_init(thread_sem_t *sem, int value);
int thread_sem_wait(thread_sem_t *sem);
int thread_sem_post(thread_sem_t *sem);
int thread_sem_destroy(thread_sem_t *sem);

#endif
