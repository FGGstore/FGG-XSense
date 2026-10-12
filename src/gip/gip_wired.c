#include "gip/gip_wired.h"
#include "gip/gip_proto.h"
#include "log.h"
#include "pads.h"
#include "usb/ugen.h"

#include <dev/usb/usb.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GIP_IFACE_CLASS    0xFF
#define GIP_IFACE_SUBCLASS 0x47
#define GIP_IFACE_PROTOCOL 0xD0

#define XFER_BUF  256
#define SLOT_IN   0
#define SLOT_OUT  1
#define SLOTS     2
#define CFG_MAX   1024

/* How long an initialisation write may take to land, in milliseconds. */
#define SEND_TIMEOUT_MS 200

/* Silence, in seconds, before concluding the controller is really gone.
 *
 * This has to be measured in time rather than in failed reads. Failed reads
 * are entirely normal here, and a failure returns immediately with no wait, so
 * counting them declares an unplug within microseconds of starting.
 */
#define UNPLUG_SECONDS 30

/* Silence that triggers re-initialisation instead of giving up. */
#define REINIT_SECONDS 3

/* Re-initialise this often even while everything is healthy. Waiting for the
 * controller to fall silent first costs a few seconds of input every time. */
#define KEEPALIVE_SECONDS 20

/* Consecutive fruitless re-initialisations before rebuilding the USB
 * connection, which is the heavier remedy for a wedged endpoint. */
#define REINITS_BEFORE_REBUILD 3

/* How often to summarise progress into the log. */
#define REPORT_SECONDS 60

typedef struct {
    ugen *u;
    char  node[32];
    unsigned vendor, product;
    int   iface, ep_in, ep_out, mps_in;
    int   seq;
    int   battery_level, battery_type;
} wired;

/* Everything the loop counts, kept together so the periodic line and the
 * summary at the end cannot drift apart. */
typedef struct {
    long reports, delivered, failed, acked;
    int  reinits, rebuilds;
} stats;

/* ---- descriptors ------------------------------------------------------- */

int gip_wired_match(unsigned vendor, unsigned product,
                    const unsigned char *cfg, int len)
{
    int i = 0;

    (void)vendor;
    (void)product;

    while (i + 9 <= len) {
        unsigned blen = cfg[i];
        if (blen == 0) break;
        if (cfg[i + 1] == UDESC_INTERFACE &&
            cfg[i + 5] == GIP_IFACE_CLASS &&
            cfg[i + 6] == GIP_IFACE_SUBCLASS &&
            cfg[i + 7] == GIP_IFACE_PROTOCOL) return 1;
        i += (int)blen;
    }
    return 0;
}

/* Finds the GIP interface and the interrupt endpoints belonging to it.
 * Endpoints seen outside that interface are ignored, so a composite device's
 * other functions cannot be mistaken for the gamepad. */
static int scan_layout(wired *w, const unsigned char *buf, int len)
{
    int i = 0, in_gip = 0;

    w->iface = w->ep_in = w->ep_out = -1;
    w->mps_in = 0;

    while (i + 2 <= len) {
        unsigned blen = buf[i], btype = buf[i + 1];
        if (blen == 0 || i + (int)blen > len) break;

        if (btype == UDESC_INTERFACE && blen >= 9) {
            in_gip = (buf[i + 5] == GIP_IFACE_CLASS &&
                      buf[i + 6] == GIP_IFACE_SUBCLASS &&
                      buf[i + 7] == GIP_IFACE_PROTOCOL);
            if (in_gip && w->iface < 0) w->iface = (int)buf[i + 2];
            else in_gip = in_gip && (w->iface == (int)buf[i + 2]);
        } else if (btype == UDESC_ENDPOINT && blen >= 7 && in_gip) {
            unsigned addr = buf[i + 2], attr = buf[i + 3];
            unsigned mps = (unsigned)buf[i + 4] | ((unsigned)buf[i + 5] << 8);

            if ((attr & UE_XFERTYPE) == UE_INTERRUPT) {
                if ((addr & UE_DIR_IN) && w->ep_in < 0) {
                    w->ep_in = (int)addr;
                    w->mps_in = (int)mps;
                } else if (!(addr & UE_DIR_IN) && w->ep_out < 0) {
                    w->ep_out = (int)addr;
                }
            }
        }
        i += (int)blen;
    }

    return w->iface >= 0 && w->ep_in >= 0;
}

/* ---- messages ---------------------------------------------------------- */

/* Queues a message. Dropping one whose predecessor is still in flight is
 * safe: every message sent from here is either idempotent or an
 * acknowledgement that will be asked for again. */
static int send_msg(wired *w, const unsigned char *msg, int len)
{
    if (w->ep_out < 0 || len < 0) return 0;
    if (ugen_slot_pending(w->u, SLOT_OUT)) return 0;

    memcpy(ugen_slot_buf(w->u, SLOT_OUT), msg, (size_t)len);
    return ugen_fs_start(w->u, SLOT_OUT, (uint32_t)len);
}

/* Sends a message and waits for the write itself to complete.
 *
 * The initialisation messages must actually go out rather than be dropped
 * because the previous write is outstanding, so this one waits for the write.
 *
 * Completions arrive on one shared queue, so an input report that lands during
 * the wait is consumed here and lost. That is deliberate and it is cheap: this
 * runs three times per initialisation, reports arrive around ninety times a
 * second, and the alternative is buffering an input report inside the send
 * path to hand back later. Worth knowing it happens, not worth preventing.
 */
static void send_sync(wired *w, const unsigned char *msg, int len)
{
    ugen_completion c;
    int waited = 0;

    if (!send_msg(w, msg, len)) return;

    while (ugen_slot_pending(w->u, SLOT_OUT) && waited < SEND_TIMEOUT_MS) {
        ugen_fs_complete(w->u, &c);
        usleep(1000);
        waited++;
    }
}

static void init_pad(wired *w)
{
    unsigned char msg[GIP_HEADER_SIZE + 8];
    int i;

    for (i = 0; i < gip_init_count; i++) {
        int len = gip_build(msg, (int)sizeof msg, gip_init_seq[i].cmd,
                            (unsigned)(w->seq++ & 0xFF), gip_init_seq[i].payload,
                            gip_init_seq[i].len);
        send_sync(w, msg, len);
    }
}

static void acknowledge(wired *w, const unsigned char *msg)
{
    unsigned char ack[GIP_HEADER_SIZE + 16];
    send_msg(w, ack, gip_build_ack(ack, (int)sizeof ack, msg));
}

static void report_battery(wired *w, int pad, const unsigned char *msg, int n)
{
    int level, type;

    if (!gip_decode_battery(msg, n, &level, &type) ||
        (level == w->battery_level && type == w->battery_type))
        return;
    pads_battery(pad, level, type);
    w->battery_level = level;
    w->battery_type = type;
    log_line("  %s: battery %s (%s)", w->node, gip_battery_level_name(level),
             gip_battery_type_name(type));
}

/* Collects the next message, if one has arrived: its length, 0 when nothing
 * is ready, -1 on a transfer error. Errors are ordinary here -- the transfer
 * layer refuses often enough that a healthy controller still only delivers a
 * fraction of its reports -- so only sustained silence means anything. */
static int read_msg(wired *w, const unsigned char **msg)
{
    ugen_completion c;

    ugen_fs_start(w->u, SLOT_IN, (uint32_t)(w->mps_in ? w->mps_in : 64));
    if (!ugen_fs_complete(w->u, &c) || c.slot != SLOT_IN) return 0;
    if (c.status != 0) return -1;
    if (c.frames == 0) return 0;

    *msg = ugen_slot_buf(w->u, SLOT_IN);
    return (int)c.len;
}

/* ---- lifecycle --------------------------------------------------------- */

/* Opens, configures, claims and initialises the controller. */
static int bring_up(wired *w, char *why, int whylen)
{
    unsigned char cfg[CFG_MAX];
    int len;

    w->u = ugen_open(w->node);
    if (!w->u) {
        snprintf(why, (size_t)whylen, "open: %s", strerror(errno));
        return 0;
    }

    len = ugen_config_desc(w->u, cfg, (int)sizeof cfg);
    if (len <= 0 || !scan_layout(w, cfg, len)) {
        snprintf(why, (size_t)whylen, "no usable GIP endpoints");
        return 0;
    }
    ugen_ids(w->u, &w->vendor, &w->product);

    ugen_set_config(w->u, 0);
    ugen_claim(w->u, w->iface);

    if (!ugen_fs_init(w->u, SLOTS) ||
        !ugen_fs_open(w->u, SLOT_IN, (unsigned)w->ep_in, XFER_BUF, 1000)) {
        snprintf(why, (size_t)whylen, "transfer setup failed: %s", strerror(errno));
        return 0;
    }
    if (w->ep_out >= 0)   /* input alone is still useful */
        ugen_fs_open(w->u, SLOT_OUT, (unsigned)w->ep_out, XFER_BUF, 1000);

    init_pad(w);
    snprintf(why, (size_t)whylen, "interface %d, IN %02X, OUT %02X",
             w->iface, w->ep_in, w->ep_out);
    return 1;
}

/* Tears the USB connection down and rebuilds it on the same device.
 * Resending messages cannot rescue an endpoint that has wedged. */
static int rebuild(wired *w)
{
    char why[160];

    ugen_close(w->u);
    w->u = NULL;
    return bring_up(w, why, (int)sizeof why);
}

/* Returns when the controller is unplugged, the payload stops, or the
 * connection cannot be recovered. */
static void serve(wired *w, int pad, stats *st)
{
    time_t last_input, last_reinit, last_keepalive, last_note;
    int since_input = 0;

    last_input = last_reinit = last_keepalive = last_note = time(NULL);

    while (!pads_stopping()) {
        const unsigned char *msg = NULL;
        time_t now;
        int n = read_msg(w, &msg);

        if (n < 0) {
            /* Back off briefly so a refused transfer does not spin the CPU. */
            st->failed++;
            usleep(2000);
        } else if (n == 0) {
            usleep(1000);
        } else if (n >= GIP_HEADER_SIZE) {
            pad_state state;

            last_input = time(NULL);
            since_input = 0;

            /* Answer anything that asked to be answered before interpreting
             * it: the controller waits, and eventually stops talking to a host
             * that ignores it. */
            if (gip_wants_ack(msg)) {
                acknowledge(w, msg);
                st->acked++;
            }

            if (gip_decode_input(msg, n, &state)) {
                st->reports++;
                if (pads_update(pad, &state)) st->delivered++;
            } else if (msg[0] == GIP_CMD_STATUS) {
                report_battery(w, pad, msg, n);
            }
        }

        /* Housekeeping runs on every pass, including ones where a message
         * arrived. An earlier version skipped it whenever the guide button was
         * pressed, which stalled every timer below. */
        now = time(NULL);

        if (now - last_keepalive >= KEEPALIVE_SECONDS) {
            last_keepalive = now;
            init_pad(w);
        }

        if (now - last_input >= REINIT_SECONDS &&
            now - last_reinit >= REINIT_SECONDS) {
            last_reinit = last_keepalive = now;
            st->reinits++;
            init_pad(w);

            if (++since_input >= REINITS_BEFORE_REBUILD) {
                since_input = 0;
                st->rebuilds++;
                log_line("  %s: re-initialising is not helping -- rebuilding USB",
                         w->node);
                if (!rebuild(w)) {
                    log_line("  %s: rebuild failed -- giving up", w->node);
                    return;
                }
            }
        }

        if (now - last_input >= UNPLUG_SECONDS) {
            log_line("  %s: no input for %d seconds -- controller gone",
                     w->node, UNPLUG_SECONDS);
            return;
        }

        if (now - last_note >= REPORT_SECONDS) {
            last_note = now;
            log_line("  %s: %ld read, %ld delivered, %ld acked, %d reinit(s), "
                     "%d rebuild(s)", w->node,
                     st->reports, st->delivered, st->acked,
                     st->reinits, st->rebuilds);
        }
    }
}

void *gip_wired_run(void *arg)
{
    char why[160], id[16];
    wired w;
    stats st;
    int pad;

    memset(&w, 0, sizeof w);
    memset(&st, 0, sizeof st);
    w.battery_level = w.battery_type = -1;
    snprintf(w.node, sizeof w.node, "%s", (const char *)arg);

    if (!bring_up(&w, why, (int)sizeof why)) {
        log_line("  %s: %s", w.node, why);
        goto done;
    }
    log_line("  controller %s  %04X:%04X  %s",
             w.node, w.vendor, w.product, why);

    snprintf(id, sizeof id, "%04X:%04X", w.vendor, w.product);
    pad = pads_attach("wired", id);
    if (pad < 0) goto done;

    serve(&w, pad, &st);

    log_line("  %s: total %ld read, %ld delivered, %ld failed, %ld acked, "
             "%d reinit(s), %d rebuild(s)", w.node,
             st.reports, st.delivered, st.failed, st.acked,
             st.reinits, st.rebuilds);
    pads_detach(pad);

    if (!pads_stopping())
        notify("FGG-XSense: controller disconnected - waiting");

done:
    ugen_close(w.u);
    return NULL;
}
