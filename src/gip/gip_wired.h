/* A GIP controller on a USB cable.
 *
 * The PS5 enumerates a GIP gamepad and then leaves it alone -- unconfigured,
 * unlit and silent. This backend configures it, keeps it awake, and feeds its
 * input to the pad table for as long as it stays connected.
 */
#ifndef FGG_GIP_WIRED_H
#define FGG_GIP_WIRED_H

/* Accepts devices presenting the GIP interface signature -- class FF,
 * subclass 47, protocol D0 -- rather than a list of vendor IDs, so an
 * untested controller still works and, just as importantly, nothing else on
 * the bus is ever configured or claimed. */
int gip_wired_match(unsigned vendor, unsigned product,
                    const unsigned char *cfg, int cfglen);

/* Serves the controller on the node named by `arg` (a char array that must
 * outlive the thread) until it is unplugged or the payload stops. Returns
 * when done; the caller still owns, and must disown, the node. */
void *gip_wired_run(void *arg);

#endif
