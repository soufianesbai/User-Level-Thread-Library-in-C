#define _GNU_SOURCE

#include "pool.h"
#include "thread_internal.h"
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/time.h>

static struct itimerval timer;
static struct sigaction sa;
static sigset_t sigvtalrm_mask;

void preem_block(void) {
  sigprocmask(SIG_BLOCK, &sigvtalrm_mask, NULL);
}

void preem_unblock(void) {
  sigprocmask(SIG_UNBLOCK, &sigvtalrm_mask, NULL);
}

// Call once at init, before arming the timer
void preem_mask_init(void) {
  sigemptyset(&sigvtalrm_mask);
  sigaddset(&sigvtalrm_mask, SIGVTALRM);
}

int init_prem(void (*func)(int), int us) {
  printf("Initializing preemption with interval %d us\n", us);
  sa.sa_handler = func;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGVTALRM, &sa, NULL);

  timer.it_value.tv_sec = 0;
  timer.it_value.tv_usec = us;

  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = us;

  preem_mask_init();

  setitimer(ITIMER_VIRTUAL, &timer, NULL);
  return 0;
}

/* ------------------------------------------------------------------ *
 * Stack overflow detection                                            *
 * ------------------------------------------------------------------ */

/* Static alternate stack for the SIGSEGV handler.
 * Required because the thread's own stack may be full when the fault fires.
 * Fixed size: SIGSTKSZ is not a compile-time constant on glibc >= 2.34. */
#define ALTSTACK_SIZE (64 * 1024)
static char altstack_mem[ALTSTACK_SIZE];
static int overflow_detection_initialized = 0;

static void sigsegv_handler(int sig, siginfo_t *info, void *uctx) {
  (void)sig;
  (void)uctx;

  thread *t = current_thread;
  char *fault = (char *)info->si_addr;

  /* Check if the fault address falls inside the current thread's guard page. */
  if (t != NULL && t->stack_map != NULL && fault >= (char *)t->stack_map &&
      fault < (char *)t->stack_map + GUARD_SIZE) {
    t->stack_overflow = 1;
    /* thread_exit() switches to the next thread's stack permanently.
     * The altstack frame is abandoned but never reused while we are in it. */
    thread_exit(NULL);
    __builtin_unreachable();
  }

  /* Not a guard page hit — genuine memory error. Restore default and re-raise. */
  signal(SIGSEGV, SIG_DFL);
  raise(SIGSEGV);
}

void init_stack_overflow_detection(void) {
  if (overflow_detection_initialized)
    return;

  stack_t ss = {
      .ss_sp = altstack_mem,
      .ss_size = ALTSTACK_SIZE,
      .ss_flags = 0,
  };
  if (sigaltstack(&ss, NULL) == -1)
    return;

  struct sigaction sa_segv;
  sa_segv.sa_sigaction = sigsegv_handler;
  sa_segv.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa_segv.sa_mask);
  /* Block SIGVTALRM during the handler to prevent a preemption context switch
   * while we are already handling a fault on the altstack. */
  sigaddset(&sa_segv.sa_mask, SIGVTALRM);
  sigaction(SIGSEGV, &sa_segv, NULL);

  overflow_detection_initialized = 1;
}