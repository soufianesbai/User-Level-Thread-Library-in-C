#include "preemption.h"
#include "thread.h"
#include "thread_internal.h"
#include <errno.h>
#include <stddef.h>

#ifdef ENABLE_SIGNAL
#define THREAD_SIGNAL_MIN 1
#define THREAD_SIGNAL_MAX 32

static unsigned int thread_signal_bit(int sig) {
  return 1u << (unsigned int)(sig - 1);
}

static int thread_signal_pick_one(unsigned int sigset) {
  for (int sig = THREAD_SIGNAL_MIN; sig <= THREAD_SIGNAL_MAX; ++sig) {
    if (sigset & thread_signal_bit(sig)) {
      return sig;
    }
  }
  return -1;
}

int thread_signal_send(thread_t target_handle, int sig) {
  if (target_handle == NULL || sig < THREAD_SIGNAL_MIN || sig > THREAD_SIGNAL_MAX) {
    errno = EINVAL;
    return -1;
  }

  thread *target = (thread *)target_handle;
  unsigned int bit = thread_signal_bit(sig);

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  target->pending_signals |= bit;

  if (target->state == THREAD_BLOCKED && target->waiting_for_signal &&
      (target->waited_signals & bit) != 0) {
    target->waiting_for_signal = 0;
    target->waited_signals = 0;
    target->state = THREAD_READY;
    thread_scheduler_enqueue(target);
  }

#ifdef ENABLE_PREEMPTION
  preem_unblock();
#endif

  return 0;
}

int thread_sigwait(thread_sigset_t set, int *sig) {
  if (set == 0 || sig == NULL) {
    errno = EINVAL;
    return -1;
  }

#ifdef ENABLE_PREEMPTION
  preem_block();
#endif

  while (1) {
    thread *current = thread_get_current_thread();
    unsigned int available = current->pending_signals & (unsigned int)set;
    available &= ~current->blocked_signals;

    if (available != 0) {
      int chosen = thread_signal_pick_one(available);
      if (chosen < 0) {
#ifdef ENABLE_PREEMPTION
        preem_unblock();
#endif
        errno = EINVAL;
        return -1;
      }
      current->pending_signals &= ~thread_signal_bit(chosen);
      *sig = chosen;
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      return 0;
    }

    current->waiting_for_signal = 1;
    current->waited_signals = (unsigned int)set;
    current->state = THREAD_BLOCKED;

    thread *prev = current;
    thread *next = thread_scheduler_pick_next();

    if (next == NULL) {
      current->waiting_for_signal = 0;
      current->waited_signals = 0;
      current->state = THREAD_RUNNING;
#ifdef ENABLE_PREEMPTION
      preem_unblock();
#endif
      errno = EDEADLK;
      return -1;
    }

    swap_thread(prev, next);
  }
}

#endif
