#ifndef LOOP_WAKE_H
#define LOOP_WAKE_H

#include <stdbool.h>
#include <stdint.h>

/* Wakes the main (LVGL) loop out of its idle sleep. With the screen off and
 * nothing playing the loop sleeps for seconds at a time (see main.c), so
 * every background thread that queues something for the GUI thread to
 * consume -- a key press, an AVRCP command, a remote-control request, a
 * power event -- calls loop_wake_signal() after queuing it. Safe from any
 * thread; costs at most one write() per wake of the loop. */
void loop_wake_signal(void);

/* Main loop only: sleeps up to timeout_ms or until loop_wake_signal().
 * Returns true when woken by a signal (pending signals are consumed). */
bool loop_wake_wait(uint32_t timeout_ms);

#endif /* LOOP_WAKE_H */
