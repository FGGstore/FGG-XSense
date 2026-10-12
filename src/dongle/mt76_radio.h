/* Bringing up the adapter's radio once its firmware runs.
 *
 * Follows xone's xone_mt76_init_radio step for step: power, the register
 * table, crystal and radio calibration, the MAC address controllers expect,
 * picking a channel, and finally the beacon that lets controllers find the
 * adapter. Each step is logged, so a failure names the step that failed.
 */
#ifndef FGG_MT76_RADIO_H
#define FGG_MT76_RADIO_H

#include "dongle/mt76.h"

#define MT76_NUM_CHANNELS 12

typedef struct {
    uint8_t index;       /* channel number */
    uint8_t band;        /* power table column */
    uint8_t bandwidth;   /* MT_PHY_BW_* */
    uint8_t group;       /* 5 GHz calibration group */
    uint8_t scan;
    uint8_t power;
} mt76_channel;

typedef struct {
    uint16_t     chip_id;
    uint8_t      address[6];
    mt76_channel channels[MT76_NUM_CHANNELS];
    int          channel;   /* index into channels */
} mt76_radio;

/* Uses `channel` (e.g. 1, 6, 11, 36, 149) instead of choosing; 0 chooses
 * the best 5 GHz channel. Takes effect at the next radio start. */
void mt76_radio_force_channel(int channel);

/* Returns 0 if a step failed; the log says which. `warm` is set when the
 * firmware was restarted in place rather than uploaded afresh. */
int mt76_radio_init(mt76 *m, mt76_radio *r, int warm);

/* Opens or closes the pairing window: the beacon advertises it, and the
 * receive filter is widened to let pairing requests in. */
int mt76_radio_set_pairing(mt76 *m, mt76_radio *r, int enable);

/* Clients are numbered by WCID, 1..MT76_MAX_CLIENTS. */
#define MT76_MAX_CLIENTS 16

/* Admits a controller that asked to associate: records its address in the
 * WCID table, tells the firmware, and answers the request. */
int mt76_radio_associate(mt76 *m, mt76_radio *r, int wcid, const uint8_t *addr);

/* Answers a controller's pairing request. */
int mt76_radio_pair_client(mt76 *m, mt76_radio *r, const uint8_t *addr);

/* Forgets a client. */
int mt76_radio_remove_client(mt76 *m, int wcid);

#endif
