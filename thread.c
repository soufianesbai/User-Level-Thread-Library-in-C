#include "thread.h"
 
#include <errno.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <ucontext.h>
#include <sys/mman.h>  
 
#define STACK_SIZE (128 * 1024)
#define THREAD_READY 0
#define THREAD_RUNNING 1
#define THREAD_TERMINATED 2

typedef struct thread {
  int id; // Thread ID
  ucontext_t context; // Context for the thread
  void *(*start_fun)(void *); // Function pointer for the thread's start routine
  void *arg; // Argument to pass to the start routine
  STAILQ_ENTRY(thread) entries; // Queue entries for the ready queue
  int state; // Thread state: READY, RUNNING, TERMINATED
  void *retval; // Return value from the thread
  int joined; // Flag to detect double-join
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
  // Call the start function of the current thread with its argument and store the return value.
  void *retval = current_thread->start_fun(current_thread->arg);
  thread_exit(retval);
}
 
/*
    This function retrieves the identifier of the current thread.
*/
thread_t thread_self(void) { return (thread_t)current_thread; }
 
int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg) {
  if (!scheduler_initialized) {
    STAILQ_INIT(&ready_queue);
    if (getcontext(&main_thread.context) == -1) {
      return -1;
    }
    scheduler_initialized = 1;
  }
 
  if (newthread == NULL || func == NULL) {
    errno = EINVAL; // Invalid argument
    return -1;
  }
 
  thread *newth = malloc(sizeof(*newth));
  if (newth == NULL) {
    errno = ENOMEM; // Out of memory
    return -1;
  }
 
  void *stack = mmap(NULL, STACK_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                   -1, 0);
if (stack == MAP_FAILED) {
    free(newth);
    return -1;
}
 
  newth->id = next_thread_id++;
  newth->start_fun = func;
  newth->arg = funcarg;
  newth->state = THREAD_READY;
  newth->joined = 0;
  newth->retval = NULL;
 
  if (getcontext(&newth->context) == -1) {
    munmap(stack, STACK_SIZE);
    free(newth);
    return -1;
  }
 
  // getcontext initializes the whole ucontext; stack settings must come after.
  newth->context.uc_stack.ss_sp = stack;
  newth->context.uc_stack.ss_size = STACK_SIZE;
  newth->context.uc_stack.ss_flags = 0;
  newth->context.uc_link = NULL;
  makecontext(&newth->context, thread_entry, 0);
 
  STAILQ_INSERT_TAIL(&ready_queue, newth, entries);
  *newthread = (thread_t)newth;
 
  return 0;
}
 
int thread_yield(void) {
    thread *next = STAILQ_FIRST(&ready_queue);
    if (!next) {
        // No other thread is ready to run, so we just return and continue executing the current thread.
        return 0;
    }

    thread *prev = current_thread;
    STAILQ_REMOVE_HEAD(&ready_queue, entries); // Remove the next thread from the ready queue

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        STAILQ_INSERT_TAIL(&ready_queue, prev, entries); // Put the current thread back in the ready queue if it's still running
    }

    current_thread = next;
    next->state = THREAD_RUNNING;

    swapcontext(&prev->context, &next->context);
    return 0;
}

void thread_exit(void *retval) {
    current_thread->retval = retval;
    current_thread->state = THREAD_TERMINATED;

    // Find the next thread to run
    thread *next = STAILQ_FIRST(&ready_queue);
    if (!next) {
        // No other thread is ready to run, so we just exit the program.
        exit(0);
    }

    // Remove the next thread from the ready queue and switch to it
    STAILQ_REMOVE_HEAD(&ready_queue, entries);
    current_thread = next;
    next->state = THREAD_RUNNING;

    // Switch to the next thread's context. Since the current thread is terminating, we use setcontext instead of swapcontext.
    setcontext(&next->context);

    // If setcontext returns, it failed. Exit with error.
    exit(1);
}

extern int thread_join(thread_t thread_handle, void **retval) {
  if (thread_handle == NULL) {
    errno = EINVAL;
    return -1;
  }
 
  thread *target = (thread *)thread_handle;
 
  // Protection against double-join: if the thread has already been joined, we return an error.
  if (target->joined) {
    errno = EINVAL;
    return -1;
  }
  target->joined = 1;
 
  // Wait for the target thread to terminate
  while (target->state != THREAD_TERMINATED) {
    thread_yield();
  }
 
  if (retval != NULL) {
    *retval = target->retval;
  }
 
  // Free the stack and structure of the thread (except main)
  if (target != &main_thread) {
    munmap(target->context.uc_stack.ss_sp, STACK_SIZE);
    free(target);
  }
 
  return 0;
}
