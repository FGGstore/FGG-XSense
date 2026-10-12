/* Keeping to one running copy.
 *
 * Launching twice is easy to do by accident, and two copies is not merely
 * wasteful: both claim the same USB interface, both queue reads on the same
 * endpoint, and each consumes completions belonging to the other. It presents
 * exactly as failing hardware -- a controller that constantly goes quiet and
 * gets reinitialised -- which took a long time to recognise for what it was.
 */
#ifndef FGG_LOCK_H
#define FGG_LOCK_H

#define LOCK_PATH "/data/fgg-xsense/xsense.lock"

/* How stale the lock must be before another copy claims it. The running
 * payload refreshes it every second, so anything older belongs to a copy that
 * died without cleaning up. */
#define LOCK_STALE_SECONDS 15

/* Returns 0 when another copy is already running. */
int  lock_take(void);
void lock_refresh(void);
void lock_release(void);

#endif
