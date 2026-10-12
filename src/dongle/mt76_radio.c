/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from dlundqvist/xone transport/mt76.c (xone_mt76_init_radio and the
 * functions it calls), (c) Severin von Wnuck-Lipinski, GPL-2.0-or-later. */
#include "dongle/mt76_radio.h"
#include "log.h"

#include <string.h>
#include <unistd.h>

#include "dongle/mt76_init_regs.h"

/* Receive filters: the normal one, and the wider one used while pairing and
 * while evaluating channels. */
#define RX_FILTER_NORMAL 0x017f17
#define RX_FILTER_WIDE   0x014f13

/* Idle time after which the firmware drops a silent controller. xone sets it
 * to stop idle controllers disconnecting. */
#define IDLE_TIME 64

/* xone_mt76_channels: number, power-table column, bandwidth, 5 GHz group,
 * scan. Power is filled in from the EFUSE. */
#define CH_2G_LOW  1
#define CH_2G_MID  2
#define CH_2G_HIGH 3
#define CH_5G_LOW  1
#define CH_5G_HIGH 2

/* A channel number to use instead of choosing one (config adapter_channel). */
static int g_forced_channel;

void mt76_radio_force_channel(int channel) { g_forced_channel = channel; }

static const mt76_channel g_channels[MT76_NUM_CHANNELS] = {
    { 0x01, CH_2G_LOW,  MT_PHY_BW_20, 0,               1, 0 },
    { 0x06, CH_2G_MID,  MT_PHY_BW_20, 0,               1, 0 },
    { 0x0b, CH_2G_HIGH, MT_PHY_BW_20, 0,               1, 0 },
    { 0x24, CH_5G_LOW,  MT_PHY_BW_40, MT_CH_5G_UNII_1, 1, 0 },
    { 0x28, CH_5G_LOW,  MT_PHY_BW_40, MT_CH_5G_UNII_1, 0, 0 },
    { 0x2c, CH_5G_HIGH, MT_PHY_BW_40, MT_CH_5G_UNII_1, 1, 0 },
    { 0x30, CH_5G_HIGH, MT_PHY_BW_40, MT_CH_5G_UNII_1, 0, 0 },
    { 0x95, CH_5G_LOW,  MT_PHY_BW_80, MT_CH_5G_UNII_3, 1, 0 },
    { 0x99, CH_5G_LOW,  MT_PHY_BW_80, MT_CH_5G_UNII_3, 0, 0 },
    { 0x9d, CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, 1, 0 },
    { 0xa1, CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, 0, 0 },
    { 0xa5, CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, 0, 0 },
};

static void put_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int step_failed(mt76 *m, const char *step)
{
    log_line("  radio: %s failed (error %d)", step, m->err);
    return 0;
}

/* ---- microcontroller commands ------------------------------------------ */

static int cmd2(mt76 *m, unsigned cmd, uint32_t a, uint32_t b)
{
    unsigned char p[8];
    put_le32(p, a);
    put_le32(p + 4, b);
    return mt76_send_command(m, cmd, p, sizeof p);
}

static int cmd1(mt76 *m, unsigned cmd, uint32_t a)
{
    unsigned char p[4];
    put_le32(p, a);
    return mt76_send_command(m, cmd, p, sizeof p);
}

static int select_function(mt76 *m, uint32_t func, uint32_t val)
{
    return cmd2(m, MT_CMD_FUN_SET_OP, func, val);
}

static int set_power_mode(mt76 *m, uint32_t mode)
{
    return cmd1(m, MT_CMD_POWER_SAVING_OP, mode);
}

static int load_cr(mt76 *m, uint8_t mode)
{
    unsigned char p[4] = { mode, 0, 0, 0 };   /* mode, temperature, channel */
    return mt76_send_command(m, MT_CMD_LOAD_CR, p, sizeof p);
}

static int calibrate(mt76 *m, uint32_t what, uint32_t val)
{
    return cmd2(m, MT_CMD_CALIBRATION_OP, what, val);
}

static int switch_channel(mt76 *m, const mt76_channel *ch)
{
    /* xone_mt76_msg_switch_channel: channel, 3 pad, TX/RX streams (le16),
     * 10 pad, bandwidth, power, scan, unknown. */
    unsigned char p[20];

    memset(p, 0, sizeof p);
    p[0] = ch->index;
    put_le16(p + 4, 0x0101);   /* TX and RX stream 1 */
    p[16] = ch->bandwidth;
    p[17] = ch->power;
    p[18] = ch->scan;
    return mt76_send_command(m, MT_CMD_SWITCH_CHANNEL_OP, p, sizeof p);
}

static int set_wow_traffic_to_firmware(mt76 *m)
{
    unsigned char p[5];
    put_le32(p, 0x03);   /* XONE_MT_WOW_TRAFFIC */
    p[4] = 0x00;         /* XONE_MT_WOW_TO_FIRMWARE */
    return mt76_send_command(m, MT_CMD_WOW_FEATURE, p, sizeof p);
}

static int set_wow_enable(mt76 *m, int enable, uint8_t channel)
{
    unsigned char p[6];
    put_le32(p, 0x01);   /* XONE_MT_WOW_ENABLE */
    p[4] = (unsigned char)enable;
    p[5] = channel;
    return mt76_send_command(m, MT_CMD_WOW_FEATURE, p, sizeof p);
}

/* ---- steps ------------------------------------------------------------- */

static uint16_t chip_id(mt76 *m)
{
    uint8_t id[4];
    if (!mt76_read_efuse(m, MT_EE_CHIP_ID, id, sizeof id)) return 0;
    return (uint16_t)((id[1] << 8) | id[2]);
}

static int init_registers(mt76 *m)
{
    size_t i;

    for (i = 0; i < sizeof g_init_regs / sizeof g_init_regs[0]; i++)
        if (!mt76_wr(m, g_init_regs[i].reg, g_init_regs[i].val)) return 0;
    return 1;
}

static int calibrate_crystal(mt76 *m)
{
    uint8_t trim[4];
    uint16_t val;
    int8_t offset;
    uint32_t ctrl;

    if (!mt76_read_efuse(m, MT_EE_XTAL_TRIM_2, trim, sizeof trim)) return 0;

    val = (uint16_t)((trim[3] << 8) | trim[2]);
    offset = (int8_t)(val & GENMASK(6, 0));
    if ((val & 0xff) == 0xff)  offset = 0;
    else if (val & BIT(7))     offset = (int8_t)-offset;

    val >>= 8;
    if (!val || val == 0xff) {
        if (!mt76_read_efuse(m, MT_EE_XTAL_TRIM_1, trim, sizeof trim)) return 0;
        val = (uint16_t)(((trim[3] << 8) | trim[2]) & 0xff);
        if (!val || val == 0xff) val = 0x14;
    }

    val = (uint16_t)((val & GENMASK(6, 0)) + offset);
    log_line("  radio: crystal trim %u", val);

    return mt76_rr(m, MT_XO_CTRL5 | MT_VEND_TYPE_CFG, &ctrl) &&
           mt76_wr(m, MT_XO_CTRL5 | MT_VEND_TYPE_CFG,
                   (ctrl & ~MT_XO_CTRL5_C2_VAL) | ((uint32_t)val << 8)) &&
           mt76_wr(m, MT_XO_CTRL6 | MT_VEND_TYPE_CFG, MT_XO_CTRL6_C2_CTRL) &&
           mt76_wr(m, MT_CMB_CTRL, 0x0091a7ff);
}

static int init_address(mt76 *m, mt76_radio *r)
{
    uint8_t *a = r->address;

    if (!mt76_read_efuse(m, MT_EE_MAC_ADDR, a, 6)) return 0;
    log_line("  radio: factory address %02x:%02x:%02x:%02x:%02x:%02x",
             a[0], a[1], a[2], a[3], a[4], a[5]);

    if (a[0] == 0x62 && a[1] == 0x45 && a[2] == 0xb4 && a[3] == 0xe7 &&
        a[4] == 0xa4 && a[5] == 0xef)
        log_line("  radio: this address is shared by counterfeit adapters");

    /* Controllers only connect to 62:45:bx:xx:xx:xx. */
    if (a[0] != 0x62) {
        a[0] = 0x62;
        a[1] = 0x45;
        a[2] = 0xbd;
    }
    log_line("  radio: address %02x:%02x:%02x:%02x:%02x:%02x",
             a[0], a[1], a[2], a[3], a[4], a[5]);

    return mt76_write_burst(m, MT_MAC_ADDR_DW0, a, 6) &&
           mt76_write_burst(m, MT_MAC_BSSID_DW0, a, 6) &&
           mt76_send_ms_command(m, MT76_MS_SET_MAC_ADDRESS, a, 6);
}

static int set_idle_time(mt76 *m)
{
    unsigned char t[4];
    put_le32(t, IDLE_TIME);
    return mt76_send_ms_command(m, MT76_MS_SET_IDLE_TIME, t, sizeof t);
}

static int calibrate_radio(mt76 *m)
{
    /* Automatic gain control, then a reset xone notes is needed for
     * reliable associations, then the three calibrations. */
    return mt76_wr(m, MT_BBP(AGC, 8), 0x18365efa) &&
           mt76_wr(m, MT_BBP(AGC, 9), 0x18365efa) &&
           mt76_wr(m, MT_MAC_SYS_CTRL, 0) &&
           mt76_wr(m, MT_RF_BYPASS_0, 0) &&
           mt76_wr(m, MT_RF_SETTING_0, 0) &&
           calibrate(m, MT_MCU_CAL_TEMP_SENSOR, 0) &&
           calibrate(m, MT_MCU_CAL_RXDCOC, 1) &&
           calibrate(m, MT_MCU_CAL_RC, 0) &&
           mt76_wr(m, MT_MAC_SYS_CTRL,
                   MT_MAC_SYS_CTRL_ENABLE_RX | MT_MAC_SYS_CTRL_ENABLE_TX);
}

static int channel_power(mt76 *m, mt76_channel *ch)
{
    uint8_t entry[8], target, offset, idx;
    uint16_t addr;

    if (ch->bandwidth == MT_PHY_BW_20) {
        addr = MT_EE_TX_POWER_0_START_2G;
        idx = 4;
    } else {
        addr = (uint16_t)(MT_EE_TX_POWER_0_START_5G +
                          ch->group * MT_TX_POWER_GROUP_SIZE_5G);
        idx = 5;
    }

    if (!mt76_read_efuse(m, addr, entry, sizeof entry)) return 0;

    target = entry[idx];
    offset = entry[idx + ch->band];

    /* Raise or lower by the offset, in half-decibel steps. */
    if (offset & BIT(7))
        ch->power = (uint8_t)((offset & BIT(6)) ? target + (offset & GENMASK(5, 0))
                                                : target - (offset & GENMASK(5, 0)));
    else
        ch->power = target;
    return 1;
}

static int set_channel_candidates(mt76 *m, mt76_radio *r)
{
    unsigned char p[4 * (3 + MT76_NUM_CHANNELS)];
    uint8_t best = r->channels[r->channel].index;
    unsigned n = 0;
    int i;

    put_le32(p + 4 * n++, 1);
    put_le32(p + 4 * n++, best);
    put_le32(p + 4 * n++, MT76_NUM_CHANNELS - 1);
    for (i = 0; i < MT76_NUM_CHANNELS; i++)
        if (r->channels[i].index != best)
            put_le32(p + 4 * n++, r->channels[i].index);

    return mt76_send_ms_command(m, MT76_MS_SET_CHAN_CANDIDATES, p, 4 * n);
}

static int init_channels(mt76 *m, mt76_radio *r)
{
    int i, best = -1, pow = 0;
    mt76_channel *ch;

    memcpy(r->channels, g_channels, sizeof g_channels);

    if (!mt76_wr(m, MT_RX_FILTR_CFG, RX_FILTER_WIDE)) return 0;

    /* Pick the highest-power channel, the first one seen on a tie -- among
     * the 5 GHz channels unless told otherwise. xone takes the highest power
     * overall, which is a 2.4 GHz channel; on a PS5 that band is shared with
     * the console's own Bluetooth (the DualSense) and Wi-Fi, and controllers
     * were seen dropping there once the DualSense was in use. */
    for (i = 0; i < MT76_NUM_CHANNELS; i++) {
        ch = &r->channels[i];
        if (!channel_power(m, ch) || !switch_channel(m, ch)) return 0;
        if (g_forced_channel ? ch->index == g_forced_channel
                             : ch->bandwidth != MT_PHY_BW_20 && ch->power > pow) {
            best = i;
            pow = ch->power;
        }
    }
    if (best < 0 && g_forced_channel)
        log_line("  radio: channel %d is not one the adapter offers -- choosing",
                 g_forced_channel);
    if (best < 0)
        for (i = 0; i < MT76_NUM_CHANNELS; i++)
            if (r->channels[i].power > pow) {
                best = i;
                pow = r->channels[i].power;
            }
    r->channel = best >= 0 ? best : MT76_NUM_CHANNELS - 1;
    ch = &r->channels[r->channel];

    log_line("  radio: channel powers %u %u %u %u %u %u %u %u %u %u %u %u -> channel %u",
             r->channels[0].power, r->channels[1].power, r->channels[2].power,
             r->channels[3].power, r->channels[4].power, r->channels[5].power,
             r->channels[6].power, r->channels[7].power, r->channels[8].power,
             r->channels[9].power, r->channels[10].power, r->channels[11].power,
             ch->index);

    if (!mt76_wr(m, MT_RX_FILTR_CFG, RX_FILTER_NORMAL)) return 0;

    ch->scan = 1;
    if (!switch_channel(m, ch) || !set_power_mode(m, MT_RADIO_OFF)) return 0;
    usleep(50000);
    if (!set_power_mode(m, MT_RADIO_ON)) return 0;
    ch->scan = 0;
    if (!switch_channel(m, ch)) return 0;

    return set_channel_candidates(m, r);
}

/* The beacon: a TXWI, an 802.11 beacon header, and Microsoft's information
 * element announcing the adapter, whether it is pairing, and its channel. */
static int write_beacon(mt76 *m, mt76_radio *r, int pair)
{
    unsigned char b[20 + 36 + 20];
    unsigned char *txwi = b, *mgmt = b + 20, *ie = b + 56;
    static const unsigned char ms_ie[20] = {
        0x00, 0x00, 0xdd, 0x10, 0x00, 0x50, 0xf2, 0x11,
        0x01, 0x10, 0x00, 0xa5, 0x30, 0x99, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };

    memset(b, 0, sizeof b);

    /* Hardware timestamp and sequence control, OFDM rate. */
    put_le16(txwi + 0, MT_TXWI_FLAGS_TS);
    put_le16(txwi + 2, (uint16_t)FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM));
    txwi[4] = MT_TXWI_ACK_CTL_NSEQ;
    put_le16(txwi + 6, 36 + 20);

    /* Beacon from our address to everyone, 100 TU apart, capabilities as
     * the original driver sends them. */
    put_le16(mgmt + 0, 0x0080);
    memset(mgmt + 4, 0xff, 6);
    memcpy(mgmt + 10, r->address, 6);
    memcpy(mgmt + 16, r->address, 6);
    put_le16(mgmt + 32, 100);
    put_le16(mgmt + 34, 0xc631);

    memcpy(ie, ms_ie, sizeof ms_ie);
    ie[10] = (unsigned char)(pair ? 1 : 0);
    ie[14] = r->channels[r->channel].index;

    return mt76_write_burst(m, MT_BEACON_BASE, b, sizeof b);
}

int mt76_radio_set_pairing(mt76 *m, mt76_radio *r, int enable)
{
    return mt76_wr(m, MT_RX_FILTR_CFG, enable ? RX_FILTER_WIDE : RX_FILTER_NORMAL) &&
           write_beacon(m, r, enable) &&
           mt76_wr(m, MT_BEACON_TIME_CFG,
                   MT_BEACON_TIME_CFG_BEACON_TX | MT_BEACON_TIME_CFG_TBTT_EN |
                   MT_BEACON_TIME_CFG_SYNC_MODE | MT_BEACON_TIME_CFG_TIMER_EN |
                   FIELD_PREP(MT_BEACON_TIME_CFG_INTVAL, 0x0640));
}

/* A three-address 802.11 header from us to `to`. */
static void hdr3(unsigned char *h, uint16_t fc, const uint8_t *to,
                 const mt76_radio *r)
{
    memset(h, 0, 24);
    put_le16(h, fc);
    memcpy(h + 4, to, 6);
    memcpy(h + 10, r->address, 6);
    memcpy(h + 16, r->address, 6);
}

#define FC_ASSOC_RESP 0x0010
#define FC_RESERVED   0x0070   /* management, the subtype Microsoft uses */
#define WLAN_RESERVED 0x70
#define CLIENT_PAIR_RESP 0x02

int mt76_radio_associate(mt76 *m, mt76_radio *r, int wcid, const uint8_t *addr)
{
    /* xone_mt76_associate_client: the firmware's client slot is wcid - 1. */
    unsigned char add[8] = { (unsigned char)(wcid - 1), 0, 0, 0, 0x40, 0x1f, 0, 0 };
    unsigned char f[24 + 6 + 8];

    /* Association response: capabilities 0, then the status code and
     * association ID the original driver uses, then eight zero bytes. */
    memset(f, 0, sizeof f);
    hdr3(f, FC_ASSOC_RESP, addr, r);
    put_le16(f + 26, 0x0110);
    put_le16(f + 28, 0x0f00);

    return mt76_write_burst(m, MT_WCID_ADDR(wcid), addr, 6) &&
           mt76_send_ms_command(m, MT76_MS_ADD_CLIENT, add, sizeof add) &&
           mt76_send_wlan(m, f, sizeof f);
}

int mt76_radio_pair_client(mt76 *m, mt76_radio *r, const uint8_t *addr)
{
    static const unsigned char data[9] = {
        0x00, 0x45, 0x55, 0x01, 0x0f, 0x8f, 0xff, 0x87, 0x1f,
    };
    unsigned char f[24 + 2 + sizeof data];

    hdr3(f, FC_RESERVED, addr, r);
    f[24] = WLAN_RESERVED;
    f[25] = CLIENT_PAIR_RESP;
    memcpy(f + 26, data, sizeof data);

    return mt76_send_wlan(m, f, sizeof f);
}

int mt76_radio_remove_client(mt76 *m, int wcid)
{
    unsigned char rm[4] = { (unsigned char)(wcid - 1), 0, 0, 0 };
    static const uint8_t zero[6];

    return mt76_send_ms_command(m, MT76_MS_REMOVE_CLIENT, rm, sizeof rm) &&
           mt76_write_burst(m, MT_WCID_ADDR(wcid), zero, sizeof zero);
}

int mt76_radio_init(mt76 *m, mt76_radio *r, int warm)
{
    memset(r, 0, sizeof *r);

    r->chip_id = chip_id(m);
    log_line("  radio: chip id %04X", r->chip_id);

    if (!select_function(m, MT_Q_SELECT, 1)) return step_failed(m, "queue select");
    if (!set_power_mode(m, MT_RADIO_ON))     return step_failed(m, "power on");
    if (!load_cr(m, MT_RF_BBP_CR))           return step_failed(m, "load CR");
    if (!init_registers(m))                  return step_failed(m, "register table");
    if (!calibrate_crystal(m))               return step_failed(m, "crystal calibration");
    if (!init_address(m, r))                 return step_failed(m, "MAC address");
    if (!set_idle_time(m))                   return step_failed(m, "idle time");
    if (!calibrate_radio(m)) {
        if (!warm) return step_failed(m, "radio calibration");
        /* After the firmware restarted in place the chip still holds the
         * previous session's calibration, and the calibration commands are
         * refused. Keep it, and turn reception and transmission back on as
         * xone's resume does. */
        log_line("  radio: calibration refused on a restarted radio (error %d) -- "
                 "keeping the previous one", m->err);
        if (!mt76_wr(m, MT_MAC_SYS_CTRL,
                     MT_MAC_SYS_CTRL_ENABLE_RX | MT_MAC_SYS_CTRL_ENABLE_TX))
            return step_failed(m, "radio enable");
    }
    if (!init_channels(m, r))                return step_failed(m, "channels");

    /* Mandatory delay after a channel change. */
    usleep(1000000);

    /* Normal traffic routing, in case wake-on-wireless settings survived a
     * warm reset (xone: harmless otherwise). */
    if (!set_wow_traffic_to_firmware(m) ||
        !set_wow_enable(m, 0, r->channels[r->channel].index))
        return step_failed(m, "wake-on-wireless off");

    if (!mt76_radio_set_pairing(m, r, 0)) return step_failed(m, "beacon");

    log_line("  radio: up on channel %u, beacon on", r->channels[r->channel].index);
    return 1;
}
