#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stddef.h>
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