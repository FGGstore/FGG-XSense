/* The Xbox Wireless Adapter.
 *
 * So far the backend gets the adapter's firmware running and watches for it
 * to be unplugged; bringing the radio up is the next milestone. The adapter's
 * node stays owned for as long as the backend runs, so no other backend's
 * scan touches it.
 */
#ifndef FGG_DONGLE_H
#define FGG_DONGLE_H

/* Accepts Microsoft's wireless adapters: 02E6 (the original), 02FE (the
 * slim one), 02F9 and 091E (built into some laptops). */
int dongle_match(unsigned vendor, unsigned product,
                 const unsigned char *cfg, int cfglen);

/* Serves the adapter on the node named by `arg` (a char array that must
 * outlive the thread) until it is unplugged or the payload stops. The caller
 * still owns, and must disown, the node. `trace` logs every register access. */
void *dongle_run(void *arg);
void dongle_set_trace(int on);

#endif
