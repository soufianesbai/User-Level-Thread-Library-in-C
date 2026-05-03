#include "preemption.h"
#include "thread.h"
#include "thread_internal.h"
#include <errno.h>
#include <stddef.h>

#ifdef ENABLE_SIGNAL

#define THREAD_SIGNAL_MIN 1
#define THREAD_SIGNAL_MAX 32

/* Convert a signal number to its bitmask (signal 1 → bit 0, signal 32 → bit 31). */
static unsigned int thread_signal_bit(int sig) {
  return 1u << (unsigned int)(sig - 1);
}

/* Return the lowest-numbered signal set in sigset, or -1 if none. */
static int thread_signal_pick_one(unsigned int sigset) {
  for (int sig = THREAD_SIGNAL_MIN; sig <= THREAD_SIGNAL_MAX; ++sig) {
    if (sigset & thread_signal_bit(sig)) {
      return sig;
    }
  }
  return -1;
}

/*
 * thread_signal_send — deliver signal sig to target.
 *
 * Sets the corresponding bit in target->pending_signals. If target is
 * blocked inside thread_sigwait() waiting for this signal, it is woken
 * immediately: its waited/waiting fields are cleared and it is put back
 * in the ready queue.
 *
 * Returns 0 on success, -1 with errno=EINVAL on bad arguments.
 */
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

  /* Wake target if it is blocked waiting specifically for this signal. */
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

/*
 * thread_sigwait — block until one of the signals in set is pending.
 *
 * Loops checking pending_signals masked against set and blocked_signals.
 * If a matching signal is already pending it is consumed immediately
 * (no blocking). Otherwise the thread parks itself as THREAD_BLOCKED
 * with waiting_for_signal=1 until thread_signal_send() wakes it.
 *
 * On success, stores the received signal number in *sig and returns 0.
 * Returns -1 with errno=EDEADLK if no other thread can deliver a signal,
 * or errno=EINVAL on bad arguments.
 */
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
      /* Consume the lowest-numbered matching pending signal. */
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

    /* No matching signal yet — block and wait for a sender to wake us. */
    current->waiting_for_signal = 1;
    current->waited_signals = (unsigned int)set;
    current->state = THREAD_BLOCKED;

    thread *prev = current;
    thread *next = thread_scheduler_pick_next();

    if (next == NULL) {
      /* No runnable thread can call thread_signal_send() — deadlock. */
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
    /* Loop back to re-check pending signals after being woken. */
  }
}

#endif
