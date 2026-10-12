/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from dlundqvist/xone transport/mt76.c (xone_mt76_load_firmware and
 * helpers) and medusalix/xow dongle/mt76.cpp (Mt76::loadFirmware),
 * (c) Severin von Wnuck-Lipinski, GPL-2.0-or-later. */
#include "dongle/mt76_boot.h"
#include "log.h"

#include <string.h>
#include <unistd.h>

#define FW_ILM_OFFSET 0x080000
#define FW_DLM_OFFSET 0x110800

#define DEV_MODE_RESET    0x01
#define DEV_MODE_LOAD_IVB 0x12

/* Bit 19 of the RF patch register must be clear before the firmware starts,
 * or the radio never transmits beacons and no controller finds the adapter. */
#define RF_PATCH_BIT (1u << 19)

/* The chip drops off USB for a moment as its firmware starts; reading it
 * before then can see a false success (xone commit 525a27e). */
#define START_SETTLE_US 500000

#define CFG(r) ((uint32_t)(r) | MT_VEND_TYPE_CFG)

static mt76_boot_result fail(mt76 *m, const char *what)
{
    if (mt76_gone(m)) {
        log_line("  adapter: left the bus during %s", what);
        return MT76_BOOT_GONE;
    }
    log_line("  adapter: %s failed (error %d)", what, m->err);
    return MT76_BOOT_FAILED;
}

static int rf_patch(mt76 *m)
{
    uint32_t v;
    return mt76_rr(m, CFG(MT_RF_PATCH), &v) &&
           mt76_wr(m, CFG(MT_RF_PATCH), v & ~RF_PATCH_BIT);
}

/* Uploads one memory image in chunks, each confirmed by the DMA length
 * register before the next. */
static int send_part(mt76 *m, uint32_t offset, const unsigned char *data,
                     uint32_t len, const char *name)
{
    uint32_t pos, n = 0;

    for (pos = 0; pos < len; pos += MT76_CMD_MAX) {
        uint32_t chunk = len - pos < MT76_CMD_MAX ? len - pos : MT76_CMD_MAX;
        uint32_t padded = (chunk + 3) & ~3u, last = 0;

        if (!mt76_wr(m, CFG(MT_FCE_DMA_ADDR), offset + pos) ||
            !mt76_wr(m, CFG(MT_FCE_DMA_LEN), padded << 16) ||
            !mt76_send_command(m, 0, data + pos, chunk))
            return 0;

        if (!mt76_poll(m, CFG(MT_FCE_DMA_LEN), 0xFFFFFFFF,
                       0xC0000000 | (padded << 16), &last)) {
            log_line("  adapter: %s chunk %u not acknowledged (DMA_LEN %08X)",
                     name, n, (unsigned)last);
            return 0;
        }
        n++;
    }
    log_line("  adapter: %s sent, %u bytes in %u chunk(s)", name, len, n);
    return 1;
}

static mt76_boot_result cold(mt76 *m, const mt76_fw *fw);

/* Firmware already running: reset it, then either upload it again or let it
 * carry on, depending on what the reset left.
 *
 * Two outcomes have been seen on a 02E6 adapter on this console. Shortly
 * after a cold start, FCE_DMA_ADDR clears to 0: the firmware is gone and is
 * uploaded again, as xow does. Once the radio has been running, it settles
 * at 0x80000000 instead -- the state xone waits for after restarting the
 * firmware in place, and treats as running. Uploading over it is sometimes
 * refused and then locks the adapter's DMA until it is replugged, so in that
 * case nothing is uploaded and the radio set-up follows directly. */
static mt76_boot_result warm(mt76 *m, const mt76_fw *fw)
{
    mt76_boot_result r;
    uint32_t last = 0;

    if (!rf_patch(m)) return fail(m, "RF patch");
    if (!mt76_dev_mode(m, DEV_MODE_RESET)) return fail(m, "firmware reset");

    mt76_poll(m, CFG(MT_FCE_DMA_ADDR), 0xFFFFFFFF, 0, &last);
    if (mt76_gone(m)) return fail(m, "firmware reset");
    log_line("  adapter: after reset FCE_DMA_ADDR %08X", (unsigned)last);

    if (last == 0) {
        r = cold(m, fw);
        return r == MT76_BOOT_COLD ? MT76_BOOT_WARM : r;
    }
    if (last & 0x80000000) {
        usleep(START_SETTLE_US);
        return MT76_BOOT_RESTARTED;
    }

    log_line("  adapter: unexpected state after reset -- unplug and replug it");
    return MT76_BOOT_FAILED;
}

static mt76_boot_result cold(mt76 *m, const mt76_fw *fw)
{
    uint32_t last = 0;

    /* DMA set-up, then the FCE and packet DMA engines. */
    if (!mt76_wr(m, CFG(MT_USB_U3DMA_CFG),
                 MT_USB_DMA_CFG_TX_BULK_EN | MT_USB_DMA_CFG_RX_BULK_EN) ||
        !mt76_wr(m, MT_FCE_PSE_CTRL, 0x01) ||
        !mt76_wr(m, MT_TX_CPU_FROM_FCE_BASE_PTR, 0x00400230) ||
        !mt76_wr(m, MT_TX_CPU_FROM_FCE_MAX_COUNT, 0x01) ||
        !mt76_wr(m, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, 0x01) ||
        !mt76_wr(m, MT_FCE_PDMA_GLOBAL_CONF, 0x44) ||
        !mt76_wr(m, MT_FCE_SKIP_FS, 0x03))
        return fail(m, "DMA set-up");

    if (!send_part(m, FW_ILM_OFFSET, fw->ilm, fw->ilm_len, "ILM"))
        return fail(m, "ILM upload");
    if (!send_part(m, FW_DLM_OFFSET, fw->dlm, fw->dlm_len, "DLM"))
        return fail(m, "DLM upload");

    if (!mt76_wr(m, CFG(MT_FCE_DMA_ADDR), 0)) return fail(m, "DMA reset");
    if (!rf_patch(m)) return fail(m, "RF patch");
    if (!mt76_dev_mode(m, DEV_MODE_LOAD_IVB)) return fail(m, "firmware start");

    usleep(START_SETTLE_US);

    if (!mt76_poll(m, CFG(MT_FCE_DMA_ADDR), 0x01, 0x01, &last)) {
        if (mt76_gone(m)) return fail(m, "firmware start");
        log_line("  adapter: firmware start not confirmed (FCE_DMA_ADDR %08X)",
                 (unsigned)last);
        return MT76_BOOT_FAILED;
    }
    return MT76_BOOT_COLD;
}

mt76_boot_result mt76_boot(mt76 *m, const mt76_fw *fw)
{
    uint32_t state;

    if (!mt76_rr(m, CFG(MT_FCE_DMA_ADDR), &state))
        return fail(m, "reading the firmware state");

    log_line("  adapter: FCE_DMA_ADDR %08X -- %s", (unsigned)state,
             state ? "firmware already running, resetting it" :
                     "no firmware, uploading");

    return state ? warm(m, fw) : cold(m, fw);
}

const char *mt76_boot_name(mt76_boot_result r)
{
    switch (r) {
    case MT76_BOOT_COLD:   return "cold start: firmware uploaded and running";
    case MT76_BOOT_WARM:   return "warm start: firmware reset, re-uploaded and running";
    case MT76_BOOT_RESTARTED: return "warm start: firmware restarted in place";
    case MT76_BOOT_GONE:   return "left the bus";
    case MT76_BOOT_FAILED: return "failed";
    }
    return "?";
}
