#ifndef THREAD_SEM_H
#define THREAD_SEM_H

#include "thread.h"

#ifndef USE_PTHREAD

typedef struct {
    int count;
    struct thread_queue waiting_queue;
} thread_sem_t;

int thread_sem_init(thread_sem_t *sem, int value);
int thread_sem_wait(thread_sem_t *sem);
int thread_sem_post(thread_sem_t *sem);
int thread_sem_destroy(thread_sem_t *sem);

#else

#include <semaphore.h>

#define thread_sem_t sem_t
#define thread_sem_init(_sem, _value) sem_init((_sem), 0, (_value))
#define thread_sem_wait sem_wait
#define thread_sem_post sem_post
#define thread_sem_destroy sem_destroy

#endif

#endif
