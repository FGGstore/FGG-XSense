/* Talking to the Xbox Wireless Adapter's MediaTek MT76 chip.
 *
 * Registers are read and written with vendor control requests; commands for
 * the chip's microcontroller, firmware included, go out on bulk endpoint 04
 * behind a four-byte header. Ported from medusalix/xow and dlundqvist/xone
 * (both GPL-2.0-or-later); register numbers come from xone's mt76_defs.h,
 * vendored unchanged in third_party/xone.
 */
#ifndef FGG_MT76_H
#define FGG_MT76_H

#include "usb/ugen.h"

#include "mt76_defs.h"

#include <stdint.h>

/* The RF patch register (xone XONE_MT_RF_PATCH). */
#define MT_RF_PATCH 0x0130

/* Endpoints: commands and firmware out, microcontroller events and radio
 * frames in. */
#define MT76_EP_OUT     0x04
#define MT76_EP_IN_CMD  0x85
#define MT76_EP_IN_WLAN 0x84

/* The command slot. */
#define MT76_SLOT_CMD  0

/* Largest command payload: a firmware chunk. The slot carries it plus the
 * header, up to three bytes of padding and the four-byte trailer. */
#define MT76_CMD_MAX   0x3800
#define MT76_CMD_SLOT_SIZE (4 + MT76_CMD_MAX + 4 + 4)

/* Commands understood by Microsoft's firmware, sent as MT_CMD_INIT_GAIN_OP
 * (xone enum xone_mt76_ms_command). */
#define MT76_MS_SET_MAC_ADDRESS     0x00
#define MT76_MS_ADD_CLIENT          0x01
#define MT76_MS_REMOVE_CLIENT       0x02
#define MT76_MS_SET_IDLE_TIME       0x05
#define MT76_MS_SET_CHAN_CANDIDATES 0x07

/* Completions of other slots that arrive while a command waits for its own
 * write. They are kept, not dropped, and handed back by mt76_take_deferred. */
#define MT76_DEFERRED_MAX 64

typedef struct {
    ugen *u;
    int   err;     /* errno of the last failed request, 0 if none */
    int   trace;   /* log every register access */

    ugen_completion deferred[MT76_DEFERRED_MAX];
    int             ndeferred;
    long            dropped;   /* completions lost because the queue was full */
    long            stalls_cleared;
} mt76;

/* Hands back one completion that arrived while a command was waiting.
 * Returns 0 when there are none left. */
int mt76_take_deferred(mt76 *m, ugen_completion *c);

int mt76_rr(mt76 *m, uint32_t addr, uint32_t *val);
int mt76_wr(mt76 *m, uint32_t addr, uint32_t val);

/* Reads `addr` until (value & mask) == want, up to 50 times 10-20 ms apart
 * as xone does. Returns 1 on a match; `last` receives the final value. */
int mt76_poll(mt76 *m, uint32_t addr, uint32_t mask, uint32_t want,
              uint32_t *last);

/* The DEV_MODE vendor request: 0x01 resets the firmware, 0x12 starts it
 * (loads the interrupt vector block). */
int mt76_dev_mode(mt76 *m, unsigned mode);

/* Sends a command to the chip's microcontroller and waits for the write. */
int mt76_send_command(mt76 *m, unsigned cmd, const void *payload, unsigned len);

/* Sends an 802.11 frame (header onwards) through the radio, behind a TXWI
 * that asks for acknowledgement (xone xone_mt76_send_wlan). */
int mt76_send_wlan(mt76 *m, const void *frame, unsigned len);

/* The adapter's LED: 0 blinking, 1 on, 2 off. */
#define MT76_LED_BLINK 0x00
#define MT76_LED_ON    0x01
#define MT76_LED_OFF   0x02
int mt76_set_led(mt76 *m, uint32_t mode);

/* A command for Microsoft's firmware: the command word, then `data`. */
int mt76_send_ms_command(mt76 *m, uint32_t cmd, const void *data, unsigned len);

/* Writes `len` bytes into the chip's memory at register offset `idx`. */
int mt76_write_burst(mt76 *m, uint32_t idx, const void *data, unsigned len);

/* Reads `len` bytes of the chip's EFUSE (its factory configuration) from
 * `addr`. */
int mt76_read_efuse(mt76 *m, uint16_t addr, void *data, int len);

/* Whether the last failure means the device has left the bus, as it does
 * briefly once its firmware starts. */
int mt76_gone(const mt76 *m);

#endif
