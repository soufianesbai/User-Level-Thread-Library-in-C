#include "thread.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <ucontext.h>

#define STACK_SIZE (64 * 1024)
#define THREAD_READY 0
#define THREAD_RUNNING 1
#define THREAD_TERMINATED 2

typedef struct thread {
  int id;                     // Thread ID
  ucontext_t context;         // Context for the thread
  void *(*start_fun)(void *); // Function pointer for the thread's start routine
  void *arg;                  // Argument to pass to the start routine
  STAILQ_ENTRY(thread) entries; // Queue entries for the ready queue
  int state;                    // Thread state: READY, RUNNING, TERMINATED
  void * retval;				   // Return value from the thread
} thread;

// Queue to hold the ready threads
STAILQ_HEAD(thread_queue, thread);
static struct thread_queue ready_queue;

// The main thread is initialized at the start of the program and will be used
// as the initial context for the main execution flow.
static thread main_thread = {0, .state = THREAD_RUNNING};
static thread *current_thread = &main_thread;
static int next_thread_id = 1;
static int scheduler_initialized = 0;

/*
    This function serves as the entry point for new threads. It will call the
   start function with the provided argument and then exit the thread when the
   function returns.
*/
static void thread_entry(void) {
  // Call the start function of the current thread with its argument and store
  // the return value.
  void *retval = current_thread->start_fun(current_thread->arg);
  thread_exit(retval);
}

/*
    This function retrieves the identifier of the current thread.
*/
thread_t thread_self(void) { return (thread_t)current_thread; }

int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg) {
  thread *newth;

  if (!scheduler_initialized) {
    STAILQ_INIT(&ready_queue);
    STAILQ_INSERT_TAIL(&ready_queue, &main_thread, entries);
    scheduler_initialized = 1;
  }

  if (newthread == NULL || func == NULL) {
    errno = EINVAL; // Invalid argument
    return -1;
  }

  newth = malloc(sizeof(*newth));
  if (newth == NULL) {
    errno = ENOMEM; // Out of memory
    return -1;
  }

  newth->context.uc_stack.ss_sp = malloc(STACK_SIZE);
  if (newth->context.uc_stack.ss_sp == NULL) {
    free(newth);
    errno = ENOMEM; // Out of memory
    return -1;
  }

  newth->id = next_thread_id++;
  newth->start_fun = func;
  newth->arg = funcarg;
  newth->state = THREAD_READY;

  if (getcontext(&newth->context) == -1) {
    free(newth->context.uc_stack.ss_sp);
    free(newth);
    return -1;
  }

  newth->context.uc_stack.ss_size = STACK_SIZE;
  newth->context.uc_link = NULL;
  makecontext(&newth->context, thread_entry, 0);

  STAILQ_INSERT_TAIL(&ready_queue, newth, entries);
  *newthread = (thread_t)newth;
  return 0;
}

int thread_yield(void) {
  thread *next;
  thread *prev = current_thread;

  // To verify !!!
  while ((next = STAILQ_FIRST(&ready_queue)) &&
         next->state == THREAD_TERMINATED) {
    STAILQ_REMOVE_HEAD(&ready_queue, entries);
    if (next != &main_thread) {
      free(next);
    }
  }

  next = STAILQ_FIRST(&ready_queue);
  if (!next) {
    // No other thread is ready to run, so we just return.
    return 0;
  }

  // Move the current thread to the end of the ready queue and switch to the
  // next thread.
  STAILQ_REMOVE_HEAD(&ready_queue, entries);
  STAILQ_INSERT_TAIL(&ready_queue, prev, entries);
  current_thread = next;
  swapcontext(&prev->context, &next->context);
  return 0;
}

void thread_exit(void *retval) {
  current_thread->retval = retval;
  current_thread->state = THREAD_TERMINATED;

  if (current_thread != &main_thread) {
    free(current_thread->context.uc_stack.ss_sp);
  }

  // Yield to the scheduler to run the next thread. If there are no more
  // threads, the program will exit.
  thread_yield();

  // If we reach here, it means there are no more threads to run, so we exit the
  // program.
  abort();
}

/* attendre la fin d'exécution d'un thread.
 * la valeur renvoyée par le thread est placée dans *retval.
 * si retval est NULL, la valeur de retour est ignorée.
 */
extern int thread_join(thread_t thread_handle, void **retval) {
  if (thread_handle == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (retval != NULL) {
    *retval = ((thread *)thread_handle)->retval;
  }

  return 0;
}