/* SPDX-License-Identifier: GPL-3.0-or-later
 * Portions ported from dlundqvist/xone transport/mt76.c and medusalix/xow
 * dongle/mt76.cpp, (c) Severin von Wnuck-Lipinski, GPL-2.0-or-later. */
#include "dongle/mt76.h"
#include "log.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define REQ_VENDOR_IN  0xC0
#define REQ_VENDOR_OUT 0x40

#define BULK_TIMEOUT_MS 1000
#define POLL_RETRIES    50

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

int mt76_rr(mt76 *m, uint32_t addr, uint32_t *val)
{
    unsigned req = MT_VEND_MULTI_READ;
    unsigned char d[4] = { 0 };

    if (addr & MT_VEND_TYPE_CFG) {
        req = MT_VEND_READ_CFG;
        addr &= ~MT_VEND_TYPE_CFG;
    }

    if (ugen_ctrl(m->u, REQ_VENDOR_IN, req, addr >> 16, addr & 0xFFFF,
                  d, sizeof d) != (int)sizeof d) {
        m->err = errno ? errno : EIO;
        if (m->trace) log_line("    rr %04X: failed (%d)", (unsigned)addr, m->err);
        return 0;
    }

    *val = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
           ((uint32_t)d[3] << 24);
    if (m->trace) log_line("    rr %04X = %08X", (unsigned)addr, (unsigned)*val);
    return 1;
}

int mt76_wr(mt76 *m, uint32_t addr, uint32_t val)
{
    unsigned req = MT_VEND_MULTI_WRITE;
    unsigned char d[4];

    if (addr & MT_VEND_TYPE_CFG) {
        req = MT_VEND_WRITE_CFG;
        addr &= ~MT_VEND_TYPE_CFG;
    }

    put_le32(d, val);
    if (m->trace) log_line("    wr %04X = %08X", (unsigned)addr, (unsigned)val);

    if (ugen_ctrl(m->u, REQ_VENDOR_OUT, req, addr >> 16, addr & 0xFFFF,
                  d, sizeof d) != (int)sizeof d) {
        m->err = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

int mt76_poll(mt76 *m, uint32_t addr, uint32_t mask, uint32_t want,
              uint32_t *last)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < POLL_RETRIES; i++) {
        if (!mt76_rr(m, addr, &v)) {
            if (mt76_gone(m)) break;
        } else if ((v & mask) == want) {
            if (last) *last = v;
            return 1;
        }
        usleep(15000);
    }
    if (last) *last = v;
    return 0;
}

int mt76_dev_mode(mt76 *m, unsigned mode)
{
    if (m->trace) log_line("    dev_mode %02X", mode);
    if (ugen_ctrl(m->u, REQ_VENDOR_OUT, MT_VEND_DEV_MODE, mode, 0, NULL, 0) < 0) {
        m->err = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

/* Sends one message on the command endpoint: a four-byte header, the
 * payload padded to four bytes, then a four-byte zero trailer (xone
 * xone_mt76_prep_message). Waits for the write, keeping any other slot's
 * completion that arrives meanwhile. */
static int send_message_once(mt76 *m, uint32_t info, const void *payload, unsigned len);

/* A refused command can leave the endpoint stalled, after which every
 * command fails; clearing it and sending once more turns a single refusal
 * into a recovered one. */
static int send_message(mt76 *m, uint32_t info, const void *payload, unsigned len)
{
    if (send_message_once(m, info, payload, len)) return 1;
    if (m->err != EIO) return 0;
    if (!ugen_fs_clear_stall(m->u, MT76_SLOT_CMD)) return 0;
    m->stalls_cleared++;
    usleep(10000);
    return send_message_once(m, info, payload, len);
}

static int send_message_once(mt76 *m, uint32_t info, const void *payload, unsigned len)
{
    unsigned char *buf;
    unsigned padded, total;
    int waited;

    if (len > MT76_CMD_MAX) {
        m->err = EINVAL;
        return 0;
    }

    padded = (len + 3) & ~3u;
    total = 4 + padded + 4;

    buf = ugen_slot_buf(m->u, MT76_SLOT_CMD);
    put_le32(buf, info | FIELD_PREP(MT_MCU_MSG_LEN, padded));
    memcpy(buf + 4, payload, len);
    memset(buf + 4 + len, 0, total - 4 - len);

    if (!ugen_fs_start(m->u, MT76_SLOT_CMD, total)) {
        m->err = errno ? errno : EIO;
        return 0;
    }

    for (waited = 0; waited <= BULK_TIMEOUT_MS + 200; waited++) {
        ugen_completion c;

        while (ugen_fs_complete(m->u, &c)) {
            if (c.slot != MT76_SLOT_CMD) {
                if (m->ndeferred < MT76_DEFERRED_MAX)
                    m->deferred[m->ndeferred++] = c;
                else
                    m->dropped++;
                continue;
            }
            if (c.status != 0) {
                m->err = EIO;
                if (m->trace) log_line("    message %08X: status %d",
                                       (unsigned)info, c.status);
                return 0;
            }
            return 1;
        }
        usleep(1000);
    }
    m->err = ETIMEDOUT;
    return 0;
}

int mt76_send_command(mt76 *m, unsigned cmd, const void *payload, unsigned len)
{
    return send_message(m, MT_MCU_MSG_TYPE_CMD |
                           FIELD_PREP(MT_MCU_MSG_PORT, MT_CPU_TX_PORT) |
                           FIELD_PREP(MT_MCU_MSG_CMD_TYPE, cmd),
                        payload, len);
}

int mt76_send_wlan(mt76 *m, const void *frame, unsigned len)
{
    unsigned char buf[20 + 128];
    unsigned char *txwi = buf;

    if (len > sizeof buf - 20) {
        m->err = EINVAL;
        return 0;
    }

    /* Acknowledged, OFDM, no particular client (xone xone_mt76_send_wlan). */
    memset(txwi, 0, 20);
    put_le32(txwi, FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY, 5) |   /* 4 us */
                   (FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM) << 16));
    txwi[4] = MT_TXWI_ACK_CTL_REQ;
    txwi[5] = 0xff;
    txwi[6] = (unsigned char)len;
    txwi[7] = (unsigned char)(len >> 8);
    memcpy(buf + 20, frame, len);

    return send_message(m, FIELD_PREP(MT_TXD_INFO_DPORT, MT_WLAN_PORT) |
                           FIELD_PREP(MT_TXD_INFO_QSEL, MT_QSEL_EDCA) |
                           MT_TXD_INFO_WIV | MT_TXD_INFO_80211,
                        buf, 20 + len);
}

int mt76_set_led(mt76 *m, uint32_t mode)
{
    unsigned char p[4];
    put_le32(p, mode);
    return mt76_send_command(m, MT_CMD_LED_MODE_OP, p, sizeof p);
}

int mt76_take_deferred(mt76 *m, ugen_completion *c)
{
    if (m->ndeferred == 0) return 0;
    *c = m->deferred[0];
    m->ndeferred--;
    memmove(m->deferred, m->deferred + 1, (size_t)m->ndeferred * sizeof *c);
    return 1;
}

int mt76_send_ms_command(mt76 *m, uint32_t cmd, const void *data, unsigned len)
{
    unsigned char buf[4 + 64];

    if (len > sizeof buf - 4) {
        m->err = EINVAL;
        return 0;
    }
    put_le32(buf, cmd);
    memcpy(buf + 4, data, len);
    return mt76_send_command(m, MT_CMD_INIT_GAIN_OP, buf, 4 + len);
}

int mt76_write_burst(mt76 *m, uint32_t idx, const void *data, unsigned len)
{
    unsigned char buf[4 + 128];

    if (len > sizeof buf - 4) {
        m->err = EINVAL;
        return 0;
    }
    /* The register offset is given in the chip's WLAN memory map. */
    put_le32(buf, idx + MT_MCU_MEMMAP_WLAN);
    memcpy(buf + 4, data, len);
    if (m->trace) log_line("    burst %04X, %u bytes", (unsigned)idx, len);
    return mt76_send_command(m, MT_CMD_BURST_WRITE, buf, 4 + len);
}

int mt76_read_efuse(mt76 *m, uint16_t addr, void *data, int len)
{
    unsigned char *out = data;
    uint32_t ctrl, val;
    int i;

    /* Kick a read of the 16-byte block holding `addr`, wait for the kick
     * bit to clear, then read the block's words (xone_mt76_read_efuse). */
    if (!mt76_rr(m, MT_EFUSE_CTRL, &ctrl)) return 0;
    ctrl &= ~(MT_EFUSE_CTRL_AIN | MT_EFUSE_CTRL_MODE);
    ctrl |= MT_EFUSE_CTRL_KICK;
    ctrl |= FIELD_PREP(MT_EFUSE_CTRL_AIN, addr & ~0x0fu);
    ctrl |= FIELD_PREP(MT_EFUSE_CTRL_MODE, MT_EE_READ);
    if (!mt76_wr(m, MT_EFUSE_CTRL, ctrl)) return 0;

    if (!mt76_poll(m, MT_EFUSE_CTRL, MT_EFUSE_CTRL_KICK, 0, NULL)) {
        if (!m->err) m->err = ETIMEDOUT;
        return 0;
    }

    for (i = 0; i < len; i += 4) {
        uint32_t offset = (addr & GENMASK(3, 2)) + (uint32_t)i;
        int n = len - i < 4 ? len - i : 4;

        if (!mt76_rr(m, MT_EFUSE_DATA_BASE + offset, &val)) return 0;
        memcpy(out + i, &val, (size_t)n);
    }
    return 1;
}

int mt76_gone(const mt76 *m)
{
    return m->err == ENXIO || m->err == ENODEV;
}
