#ifndef PREEMPTION_H
#define PREEMPTION_H

/**
 * two functions to (un)block the preemption signal, so that the current thread can safely
 * manipulate its state
 */
void preem_block(void);
void preem_unblock(void);

/*
 * init_prem — initializes the preemption mechanism by setting up a timer to
 * send SIGVTALRM signals at regular intervals and registering the given handler
 * for those signals.
 * takes us as an argument to specify the interval in microseconds between preemption signals.
 */
int init_prem(void (*func)(int), int us);

#endif // PREEMPTION_H