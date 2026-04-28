#ifndef PREEMPTION_H
#define PREEMPTION_H

/*
 * Block/unblock SIGVTALRM delivery on the calling thread.
 *
 * Use these around any scheduler data structure access that must not be
 * interrupted mid-operation (e.g. run-queue manipulation, thread state
 * transitions). Calls may be nested: each block must be paired with an
 * unblock.
 */
void preem_block(void);
void preem_unblock(void);

/*
 * Initialize the preemption mechanism.
 *
 * Registers func as the SIGVTALRM handler and arms a virtual timer that
 * fires every `us` microseconds. The handler is invoked in the context of
 * whatever thread is running at the time, triggering a voluntary scheduler
 * call from signal context.
 *
 * Returns 0 on success, -1 on error.
 */
int init_prem(void (*func)(int), int us);

#endif /* PREEMPTION_H */