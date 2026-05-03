#define _GNU_SOURCE

#include "pool.h"
#include "thread_internal.h"
#include <signal.h>
#include <stddef.h>
#include <sys/time.h>

static struct itimerval timer;
static struct sigaction sa;
static sigset_t sigvtalrm_mask;

/* Block SIGVTALRM delivery on the calling thread.
 * All scheduler data-structure accesses that must not be interrupted
 * mid-operation are wrapped with preem_block() / preem_unblock(). */
void preem_block(void) {
  sigprocmask(SIG_BLOCK, &sigvtalrm_mask, NULL);
}

/* Unblock SIGVTALRM, allowing preemption to fire again. */
void preem_unblock(void) {
  sigprocmask(SIG_UNBLOCK, &sigvtalrm_mask, NULL);
}

/* Build the signal mask used by preem_block/unblock. Called once at init. */
void preem_mask_init(void) {
  sigemptyset(&sigvtalrm_mask);
  sigaddset(&sigvtalrm_mask, SIGVTALRM);
}

/*
 * init_prem — set up timer-based preemption.
 *
 * Registers func as the SIGVTALRM handler with SA_RESTART so that
 * interrupted syscalls are restarted transparently. Then arms a virtual
 * (per-process CPU time) interval timer that fires every `us` microseconds,
 * driving involuntary context switches.
 *
 * Returns 0 on success, -1 on error.
 */
int init_prem(void (*func)(int), int us) {
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

/*
 * sigsegv_handler — SIGSEGV handler for stack overflow detection.
 *
 * Runs on the alternate stack (SA_ONSTACK). Checks whether the faulting
 * address falls inside the current thread's guard page. If so, marks the
 * thread as overflowed and calls thread_exit() to switch away cleanly.
 * For any other SIGSEGV, restores the default handler and re-raises so
 * the process terminates with a proper signal.
 */
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

/*
 * init_stack_overflow_detection — install the SIGSEGV overflow handler.
 *
 * Sets up a static alternate stack via sigaltstack() and installs
 * sigsegv_handler with SA_SIGINFO | SA_ONSTACK so it runs on the
 * alternate stack even when the thread stack is exhausted. SIGVTALRM
 * is blocked during the handler to prevent a preemption context switch
 * while running on the altstack.
 *
 * Idempotent: safe to call multiple times (no-op after first call).
 * Only meaningful when THREAD_ENABLE_GUARD_PAGE=1.
 */
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
  sigaddset(&sa_segv.sa_mask, SIGVTALRM);
  sigaction(SIGSEGV, &sa_segv, NULL);

  overflow_detection_initialized = 1;
}
