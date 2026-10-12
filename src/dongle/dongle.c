#include "dongle/dongle.h"
#include "dongle/mt76.h"
#include "dongle/mt76_boot.h"
#include "dongle/mt76_fw.h"
#include "dongle/mt76_radio.h"
#include "dongle/link.h"
#include "log.h"
#include "pads.h"
#include "usb/ugen.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MS_VENDOR 0x045E

/* Attempts at starting the firmware before giving up until a replug. */
#define BOOT_ATTEMPTS 3

/* How often to check that the adapter is still there once running. */
#define WATCH_SECONDS 5

/* Reads kept in flight on each incoming endpoint (xone keeps 12 per
 * endpoint), and their sizes: microcontroller events are small, radio
 * frames may arrive aggregated. */
#define CMD_READS   4
#define WLAN_READS  8
#define CMD_BUF     0x654
#define WLAN_BUF    0x8400
#define SLOT_FIRST_READ (MT76_SLOT_CMD + 1)
#define SLOTS (SLOT_FIRST_READ + CMD_READS + WLAN_READS)

static int g_trace;

void dongle_set_trace(int on) { g_trace = on; }

int dongle_match(unsigned vendor, unsigned product,
                 const unsigned char *cfg, int cfglen)
{
    (void)cfg;
    (void)cfglen;

    return vendor == MS_VENDOR &&
           (product == 0x02E6 || product == 0x02FE ||
            product == 0x02F9 || product == 0x091E);
}

/* Sleeps in short steps so a stop is noticed promptly. */
static void nap(int seconds)
{
    int i;
    for (i = 0; i < seconds * 10 && !pads_stopping(); i++) usleep(100000);
}

/* Keeps reads queued on both incoming endpoints and hands whatever arrives
 * to the link, checking every few seconds that the adapter is still
 * connected. Returns when it is unplugged or the payload stops. */
static void run_radio(mt76 *m, mt76_radio *r, const char *node)
{
    time_t last_check = time(NULL);
    dlink l;
    int i;

    link_init(&l, m, r);

    /* Pairing opens by itself at first, as dlundqvist/xone does, so a
     * controller already paired with this adapter reconnects and a new one
     * can pair without touching anything. */
    link_set_pairing(&l, 1, "adapter started");

    while (!pads_stopping()) {
        ugen_completion c;
        time_t now;
        int busy = 0;

        /* What arrived while a command was being sent comes first, before
         * its slot is reused for the next read. */
        while (mt76_take_deferred(m, &c) || ugen_fs_complete(m->u, &c)) {
            busy = 1;
            if (c.slot < SLOT_FIRST_READ) continue;
            if (c.status != 0) {
                l.errors++;
                continue;
            }
            if (c.frames > 0) link_handle(&l, ugen_slot_buf(m->u, c.slot), c.len);
        }

        for (i = SLOT_FIRST_READ; i < SLOTS; i++) {
            uint32_t size = i < SLOT_FIRST_READ + CMD_READS ? CMD_BUF : WLAN_BUF;
            ugen_fs_start(m->u, i, size);
        }

        now = time(NULL);
        link_tick(&l, now);

        if (now - last_check >= WATCH_SECONDS) {
            uint32_t v;
            last_check = now;
            if (access(node, F_OK) != 0 ||
                (!mt76_rr(m, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, &v) && mt76_gone(m))) {
                log_line("  adapter %s: unplugged", node);
                link_close(&l, 0);
                return;
            }
        }

        if (!busy) usleep(2000);
    }
    link_close(&l, 1);
}

/* Holds the adapter, checking every few seconds that it is still connected.
 * Returns when it is unplugged or the payload stops. */
static void watch(mt76 *m, const char *node)
{
    while (!pads_stopping()) {
        uint32_t v;

        nap(WATCH_SECONDS);
        if (pads_stopping()) break;

        if (access(node, F_OK) != 0 ||
            (!mt76_rr(m, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, &v) && mt76_gone(m))) {
            log_line("  adapter %s: unplugged", node);
            return;
        }
    }
}

void *dongle_run(void *arg)
{
    const char *node = arg;
    unsigned vendor = 0, product = 0;
    mt76_boot_result r;
    mt76_radio radio;
    char why[160];
    int i;
    mt76_fw fw;
    mt76 m;

    memset(&m, 0, sizeof m);
    memset(&fw, 0, sizeof fw);
    m.trace = g_trace;

    m.u = ugen_open(node);
    if (!m.u || !ugen_ids(m.u, &vendor, &product)) {
        log_line("  adapter %s: could not open it (%s)", node, strerror(errno));
        goto done;
    }
    log_line("  wireless adapter %s  %04X:%04X", node, vendor, product);

    if (!mt76_fw_load(product, &fw, why, (int)sizeof why)) {
        log_line("  adapter firmware: %s", why);
        notify("FGG-XSense: adapter found, firmware missing - see README");
        /* Held anyway, so it is not rediscovered and reported every scan. */
        watch(&m, node);
        goto done;
    }
    log_line("  adapter firmware: checksum ok, ILM %u + DLM %u, built \"%s\"",
             fw.ilm_len, fw.dlm_len, fw.build_time);

    ugen_set_config(m.u, 0);
    ugen_claim(m.u, 0);
    if (!ugen_fs_init(m.u, SLOTS) ||
        !ugen_fs_open(m.u, MT76_SLOT_CMD, MT76_EP_OUT, MT76_CMD_SLOT_SIZE, 1000)) {
        log_line("  adapter %s: transfer setup failed: %s", node, strerror(errno));
        goto done;
    }
    for (i = SLOT_FIRST_READ; i < SLOTS; i++) {
        int cmd = i < SLOT_FIRST_READ + CMD_READS;
        /* Reads never time out: they wait for the adapter to send. */
        if (!ugen_fs_open(m.u, i, cmd ? MT76_EP_IN_CMD : MT76_EP_IN_WLAN,
                          cmd ? CMD_BUF : WLAN_BUF, 0)) {
            log_line("  adapter %s: read slot %d: %s", node, i, strerror(errno));
            goto done;
        }
    }

    /* A start that fails is retried: each attempt begins with a reset. */
    for (i = 1; i <= BOOT_ATTEMPTS; i++) {
        r = mt76_boot(&m, &fw);
        log_line("  adapter %s: %s", node, mt76_boot_name(r));
        if (r != MT76_BOOT_FAILED || i == BOOT_ATTEMPTS) break;
        log_line("  adapter %s: retrying (attempt %d of %d)", node, i + 1,
                 BOOT_ATTEMPTS);
        usleep(500000);
    }

    if (r == MT76_BOOT_GONE) {
        /* Expected on some adapters: it re-enumerates, possibly on another
         * node, and the next scan finds it with the firmware warm. */
        log_line("  adapter %s: waiting for it to reappear", node);
        goto done;
    }
    if (r == MT76_BOOT_FAILED) {
        notify("FGG-XSense: adapter did not start - unplug and replug it");
        watch(&m, node);
        goto done;
    }

    if (!mt76_radio_init(&m, &radio, r == MT76_BOOT_RESTARTED)) {
        notify("FGG-XSense: adapter radio did not start - unplug and replug it");
        watch(&m, node);
        goto done;
    }

    /* Firmware restarted in place keeps its client table from the last
     * run, and a controller reconnecting into a stale slot has been seen to
     * associate but then send nothing. Every slot is cleared first. */
    if (r == MT76_BOOT_RESTARTED) {
        for (i = 1; i <= MT76_MAX_CLIENTS; i++) mt76_radio_remove_client(&m, i);
        log_line("  adapter: cleared the firmware's client table");
    }

    notify("FGG-XSense: wireless adapter radio on");
    run_radio(&m, &radio, node);

done:
    mt76_fw_free(&fw);
    ugen_close(m.u);
    return NULL;
}
