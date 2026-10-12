/* A USB device node and its transfer slots.
 *
 * Two details about this platform shape the whole interface. The per-endpoint
 * /dev/ugenX.Y.Z nodes do not exist, so transfers go through the USB_FS_*
 * ioctls on the device node rather than through read() and write(). And a
 * transfer, once queued, must be left pending until it completes: cancelling
 * and requeueing an endpoint several times a second is read by a device as a
 * host that has gone wrong, and it stops talking.
 *
 * A slot is bound to one endpoint and carries one transfer at a time; several
 * slots on the same endpoint keep several transfers in flight. On firmware
 * 13.60 one node takes at most 62 slots. USB_FS_COMPLETE reports whichever
 * slot finished, so completions are handed back by slot, never assumed.
 *
 * Not thread-safe: each node is owned by exactly one thread.
 */
#ifndef FGG_UGEN_H
#define FGG_UGEN_H

#include <stdint.h>

#define UGEN_MAX_SLOTS 62

typedef struct ugen ugen;

typedef struct {
    int      slot;
    int      status;   /* 0 on success */
    int      frames;   /* 0 when nothing was transferred */
    uint32_t len;
} ugen_completion;

/* Opens `node`. Returns NULL with errno set on failure. */
ugen *ugen_open(const char *node);

/* Tears down the slots and closes the node. */
void ugen_close(ugen *u);

const char *ugen_node(const ugen *u);

/* Reads the device descriptor's vendor and product. Returns 0 on failure. */
int ugen_ids(ugen *u, unsigned *vendor, unsigned *product);

/* Reads configuration descriptor 0. Returns its length, 0 on failure. */
int ugen_config_desc(ugen *u, unsigned char *buf, int maxlen);

/* Selects configuration index `index` and claims interface `iface`. Either
 * may legitimately fail on a device that is already set up; returns 0 only if
 * both did. */
int ugen_set_config(ugen *u, int index);
int ugen_claim(ugen *u, int iface);

/* A control transfer on endpoint 0. Returns the bytes transferred, or -1. */
int ugen_ctrl(ugen *u, unsigned type, unsigned request, unsigned value,
              unsigned index, void *data, unsigned len);

/* Prepares `nslots` transfer slots. Returns 0 on failure. */
int ugen_fs_init(ugen *u, int nslots);

/* Binds `slot` to endpoint `ep` with a buffer of `bufsize` bytes. Returns 0
 * on failure. */
int ugen_fs_open(ugen *u, int slot, unsigned ep, uint32_t bufsize, int timeout_ms);

unsigned char *ugen_slot_buf(ugen *u, int slot);
int ugen_slot_pending(const ugen *u, int slot);

/* Queues a transfer of `len` bytes on `slot` -- the bytes to send for an OUT
 * endpoint, the most to accept for an IN one. A slot already in flight is left
 * alone. Returns 0 if the transfer could not be queued. */
int ugen_fs_start(ugen *u, int slot, uint32_t len);

/* Collects one finished transfer, if any. Returns 1 and fills `c` when one
 * was collected, 0 when nothing has finished. */
int ugen_fs_complete(ugen *u, ugen_completion *c);

/* Waits up to `timeout_ms` for `slot` to finish, discarding completions of
 * other slots. For simple synchronous exchanges where nothing else is in
 * flight. Returns 1 with `c` filled, 0 on timeout. */
int ugen_fs_wait(ugen *u, int slot, int timeout_ms, ugen_completion *c);

/* Clears a stall on `slot`'s endpoint, so the next transfer can go through.
 * Returns 0 on failure. */
int ugen_fs_clear_stall(ugen *u, int slot);

#endif
