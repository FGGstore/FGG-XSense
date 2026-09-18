/* FGG-XSense -- use an Xbox-protocol controller on a PlayStation 5.
 *
 * Reads a USB GIP (Xbox) gamepad directly and republishes it as a virtual
 * PlayStation pad, so games and the system see an ordinary controller. It runs
 * entirely as a payload: nothing in the system software is patched, nothing is
 * installed, and nothing persists after it exits.
 *
 * Both halves were established by probing a console rather than by assumption.
 *
 *   Reading.  The PS5 enumerates a GIP controller but never configures it, and
 *   the per-endpoint /dev/ugenX.Y.Z nodes do not exist, so transfers go through
 *   the USB_FS_* ioctls on /dev/ugenX.Y. The device needs a power-on message
 *   before it will stream input, an authentication message if it is not made by
 *   Microsoft, and a separate message to light its guide LED. Every button and
 *   axis offset below was verified against live hardware, one control at a
 *   time, against the raw bytes.
 *
 *   Writing.  libScePad's virtual-device functions turn out to be thin wrappers
 *   over two ioctls on /dev/hid, and neither checks who is calling -- only that
 *   the device is open:
 *
 *       AddDevice     ioctl(fd, 0xC018482A, &req)
 *       InsertData    ioctl(fd, 0x8018482C, &req)
 *       DeleteDevice  ioctl(fd, 0x80104850, &req)
 *
 *   So a payload can register a virtual pad and feed it input with no patching
 *   of SceShellCore or anything else. Confirmed on firmware 11.60, where all
 *   eleven device types were accepted.
 *
 *   The report the kernel reads is not ScePadData. libScePad translates its
 *   caller's ScePadData into a differently shaped buffer first, and passing a
 *   ScePadData straight through appears to work -- the ioctl validates only its
 *   24-byte header -- while the kernel reads the buttons out of what was
 *   actually the orientation quaternion. The VP_* offsets below are the layout
 *   it genuinely expects.
 *
 * The PS button is deliberately not mapped; see the note above the guide-button
 * handling for why, and for what was ruled out.
 */
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH  STATE_DIR "/xsense.log"
#define LOCK_PATH STATE_DIR "/xsense.lock"

/* How stale the lock has to be before a new instance claims it. The running
 * bridge refreshes it every second, so anything older than this belongs to a
 * copy that died without cleaning up. */
#define LOCK_STALE_SECONDS 15

/* Silence, in seconds, before concluding the controller is gone.
 *
 * This has to be measured in time rather than in failed reads. Failed reads
 * are entirely normal -- USB_FS_START refuses often enough that a healthy
 * controller only delivers a fraction of its reports -- and a failure returns
 * immediately with no wait, so counting iterations declares an unplug within
 * microseconds of starting. */
#define UNPLUG_SECONDS 30

/* Silence that triggers a re-initialisation rather than giving up. Cheap to
 * do, and it recovers a controller that powered itself down without needing
 * the payload restarted. */
#define REINIT_SECONDS 3

/* Resend the initialisation periodically even while everything is healthy.
 * Reacting only once the controller has already gone quiet means losing input
 * for a few seconds each time; keeping it fed costs three small writes. */
#define KEEPALIVE_SECONDS 20

/* Consecutive fruitless reinitialisations before rebuilding the USB side.
 * Resending GIP messages cannot help if the endpoint itself has wedged. */
#define REINITS_BEFORE_REBUILD 3

/* Hold View+Menu this long to shut the bridge down deliberately. */
#define QUIT_HOLD_SECONDS 2

/* The guide (Xbox) button is not mapped to anything, because on a PlayStation
 * it cannot be.
 *
 * The PS button is intercepted by the system above the pad layer, precisely so
 * that no pad -- real or virtual -- can synthesise a home-button press. It
 * appears in neither the published button constants nor anywhere in libScePad
 * itself, and the virtual pad report has been eliminated as a carrier for it
 * exhaustively rather than by assumption: all sixteen undocumented bits of the
 * button word, including 0x80000000 (which the PS4 headers name
 * SCE_PAD_BUTTON_INTERCEPTED), and all seventeen report bytes that libScePad
 * never writes, were each held for a second and a half on hardware. None
 * produced any reaction at all.
 *
 * The button is still read and acknowledged below -- the controller stops
 * talking to a host that ignores it -- it simply has nothing to be mapped to.
 * Use the DualSense for the PS button.
 */

/* ---- GIP (Xbox) side --------------------------------------------------- */

#define XFER_BUF 256
#define EP_IN    0
#define EP_OUT   1
#define EP_MAX   2

#define GIP_IFACE_CLASS    0xFF
#define GIP_IFACE_SUBCLASS 0x47
#define GIP_IFACE_PROTOCOL 0xD0

#define GIP_CMD_SET_DEVICE_STATE 0x05
#define GIP_CMD_LED              0x0A
#define GIP_CMD_ACK              0x01
#define GIP_CMD_ANNOUNCE         0x02
#define GIP_CMD_AUTHENTICATE     0x06

/* Set in a message's options byte when the controller wants that message
 * acknowledged. Leaving one unanswered makes the device wait, retry, and
 * eventually stop talking until it is prodded again -- which is exactly the
 * three-second stall-and-recover cycle the bridge has been fighting. */
#define GIP_OPT_ACK              0x10
#define GIP_CMD_GUIDE_BUTTON     0x07
#define GIP_LL_INPUT_REPORT      0x20
#define GIP_FLAG_SYSTEM          0x20
#define GIP_LED_GUIDE            0x00
#define GIP_LED_GUIDE_ON         0x01

/* Verified against hardware: each of these was pressed in isolation and the
 * raw payload checked, rather than taken from the Xbox layout on faith. */
#define GIP_MENU   0x0004
#define GIP_VIEW   0x0008
#define GIP_A      0x0010
#define GIP_B      0x0020
#define GIP_X      0x0040
#define GIP_Y      0x0080
#define GIP_UP     0x0100
#define GIP_DOWN   0x0200
#define GIP_LEFT   0x0400
#define GIP_RIGHT  0x0800
#define GIP_LB     0x1000
#define GIP_RB     0x2000
#define GIP_LS     0x4000
#define GIP_RS     0x8000

/* ---- PlayStation side -------------------------------------------------- */

#define HID_ADD_DEVICE  0xC018482AUL   /* _IOWR('H', 0x2A, 24) */
#define HID_INSERT_DATA 0x8018482CUL   /* _IOW ('H', 0x2C, 24) */
#define HID_DEL_DEVICE  0x80104850UL   /* _IOW ('H', 0x50, 16) */

#define HID_CMD_ADD 3
#define HID_CMD_DEL 5

#define VDEV_TYPE  1      /* standard pad */
#define VDEV_PROTO 3
#define VDEV_CLASS 1

#define SCE_L3       0x000002
#define SCE_R3       0x000004
#define SCE_OPTIONS  0x000008
#define SCE_UP       0x000010
#define SCE_RIGHT    0x000020
#define SCE_DOWN     0x000040
#define SCE_LEFT     0x000080
#define SCE_L2       0x000100
#define SCE_R2       0x000200
#define SCE_L1       0x000400
#define SCE_R1       0x000800
#define SCE_TRIANGLE 0x001000
#define SCE_CIRCLE   0x002000
#define SCE_CROSS    0x004000
#define SCE_SQUARE   0x008000
#define SCE_TOUCHPAD 0x100000

/* The report the KERNEL reads -- not ScePadData.
 *
 * libScePad takes a ScePadData from its caller and copies the fields into a
 * differently shaped buffer before handing that to the ioctl. Passing a
 * ScePadData straight through looks like it works (the ioctl only validates
 * its 24-byte header, never the contents) while the kernel reads buttons out
 * of what was actually the orientation quaternion. Every offset below comes
 * from the copies InsertData performs.
 */
#define VPAD_REPORT_SIZE 0xA0

#define VP_BUTTONS   0x0C   /* u32 */
#define VP_STICKS    0x10   /* u8 lx, ly, rx, ry */
#define VP_L2        0x14
#define VP_R2        0x15
#define VP_ACTIVE    0x17   /* the library hardcodes 1 here */
#define VP_TOUCHNUM  0x1C
#define VP_MOTION    0x78   /* 8 floats */

typedef struct { uint32_t cmd, reserved; void *data; void *out; } hid_req;
typedef struct { uint8_t cmd; uint8_t pad[7]; uint32_t handle; uint32_t pad2; } hid_del;

static FILE *g_log;

static void logf_(const char *fmt, ...)
{
    char stamp[32];
    time_t now = time(NULL);
    struct tm tmv;
    va_list ap;

    gmtime_r(&now, &tmv);
    if (strftime(stamp, sizeof stamp, "%H:%M:%S", &tmv) == 0)
        strcpy(stamp, "00:00:00");

    va_start(ap, fmt);
    if (g_log) {
        fprintf(g_log, "%s ", stamp);
        vfprintf(g_log, fmt, ap);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
    va_end(ap);
}

static void notify(const char *fmt, ...)
{
    notify_request_t req;
    char msg[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

static unsigned word(const uByte *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static int16_t le16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Refuses to start when another copy is already running.
 *
 * Launching the bridge twice is easy to do by accident now that it runs until
 * you stop it, and two copies is not merely wasteful: both claim the same USB
 * interface, both queue reads on the same endpoint, and each consumes
 * completions belonging to the other. The result looks exactly like failing
 * hardware -- a controller that constantly goes quiet and gets reinitialised.
 *
 * The lock is a file whose modification time is refreshed while the bridge
 * runs, so a copy killed without cleanup leaves a stale lock rather than
 * blocking every future launch.
 */
static int lock_held(void)
{
    struct stat stv;
    int fd;

    if (stat(LOCK_PATH, &stv) == 0 &&
        time(NULL) - stv.st_mtime < LOCK_STALE_SECONDS)
        return 0;

    fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static void lock_touch(void) { utimes(LOCK_PATH, NULL); }

/* ---- USB plumbing ------------------------------------------------------ */

typedef struct { int iface, ep_in, ep_out, mps_in; } gip_layout;

typedef struct {
    struct usb_fs_endpoint eps[EP_MAX];
    void     *bufptr[EP_MAX][1];
    uint32_t  buflen[EP_MAX][1];
    unsigned char data[EP_MAX][XFER_BUF];
    int pending[EP_MAX];
} fs_state;

static int is_gip_device(const unsigned char *cfg, int len)
{
    int i = 0;
    while (i + 9 <= len) {
        unsigned blen = cfg[i];
        if (blen == 0) break;
        if (cfg[i + 1] == 0x04 &&
            cfg[i + 5] == GIP_IFACE_CLASS &&
            cfg[i + 6] == GIP_IFACE_SUBCLASS &&
            cfg[i + 7] == GIP_IFACE_PROTOCOL) return 1;
        i += (int)blen;
    }
    return 0;
}

static int scan_layout(const unsigned char *buf, int len, gip_layout *out)
{
    int i = 0, in_gip = 0;

    out->iface = out->ep_in = out->ep_out = -1;
    out->mps_in = 0;

    while (i + 2 <= len) {
        unsigned blen = buf[i], btype = buf[i + 1];
        if (blen == 0 || i + (int)blen > len) break;

        if (btype == 0x04 && blen >= 9) {
            in_gip = (buf[i + 5] == GIP_IFACE_CLASS &&
                      buf[i + 6] == GIP_IFACE_SUBCLASS &&
                      buf[i + 7] == GIP_IFACE_PROTOCOL);
            if (in_gip && out->iface < 0) out->iface = (int)buf[i + 2];
            else in_gip = in_gip && (out->iface == (int)buf[i + 2]);
        } else if (btype == 0x05 && blen >= 7 && in_gip) {
            unsigned addr = buf[i + 2], attr = buf[i + 3];
            unsigned mps = (unsigned)buf[i + 4] | ((unsigned)buf[i + 5] << 8);
            if ((attr & 3) == 3) {
                if ((addr & 0x80) && out->ep_in < 0) {
                    out->ep_in = (int)addr; out->mps_in = (int)mps;
                } else if (!(addr & 0x80) && out->ep_out < 0) {
                    out->ep_out = (int)addr;
                }
            }
        }
        i += (int)blen;
    }
    return out->iface >= 0 && out->ep_in >= 0;
}

static int fs_setup(int fd, fs_state *st, const gip_layout *lay)
{
    struct usb_fs_init init;
    struct usb_fs_open op;
    int i;

    memset(st, 0, sizeof *st);
    for (i = 0; i < EP_MAX; i++) {
        st->bufptr[i][0] = st->data[i];
        st->buflen[i][0] = XFER_BUF;
        st->eps[i].ppBuffer = st->bufptr[i];
        st->eps[i].pLength = st->buflen[i];
        st->eps[i].nFrames = 1;
        st->eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
        st->eps[i].timeout = 1000;
    }

    memset(&init, 0, sizeof init);
    init.pEndpoints = st->eps;
    init.ep_index_max = EP_MAX;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) return 0;

    memset(&op, 0, sizeof op);
    op.max_bufsize = XFER_BUF; op.max_frames = 1;
    op.ep_index = EP_IN; op.ep_no = (uint8_t)lay->ep_in;
    if (ioctl(fd, USB_FS_OPEN, &op) != 0) return 0;

    if (lay->ep_out >= 0) {
        memset(&op, 0, sizeof op);
        op.max_bufsize = XFER_BUF; op.max_frames = 1;
        op.ep_index = EP_OUT; op.ep_no = (uint8_t)lay->ep_out;
        ioctl(fd, USB_FS_OPEN, &op);
    }
    return 1;
}

/* Queues the interrupt IN read, if one is not already outstanding.
 *
 * The transfer is left pending until the controller actually answers. An
 * earlier version started a transfer, waited a fixed 100 ms, and called
 * USB_FS_STOP when nothing had arrived yet -- aborting and requeueing the
 * endpoint many times a second. That is what made the controller stop talking
 * after a few seconds: repeatedly cancelling an interrupt endpoint looks like
 * a host that has gone wrong, and the device eventually gives up. Nothing is
 * cancelled now; a queued read simply waits. */
static int fs_start(int fd, fs_state *st, int index, int length)
{
    struct usb_fs_start start;

    if (st->pending[index]) return 1;

    st->buflen[index][0] = (uint32_t)length;
    st->eps[index].nFrames = 1;
    st->eps[index].aFrames = 0;
    st->eps[index].status = 0;

    memset(&start, 0, sizeof start);
    start.ep_index = (uint8_t)index;
    if (ioctl(fd, USB_FS_START, &start) != 0) return 0;

    st->pending[index] = 1;
    return 1;
}

/* Collects one completion, whichever endpoint it belongs to.
 *
 * USB_FS_COMPLETE reports a completed endpoint rather than the one you happen
 * to be interested in, so the index it returns has to be honoured -- reading
 * EP_IN's state after an EP_OUT completion silently produces nonsense.
 *
 * Returns the byte count for a finished IN transfer, -1 if it failed, and 0
 * when there is nothing to report yet.
 */
static int fs_poll(int fd, fs_state *st)
{
    struct usb_fs_complete comp;
    int index;

    memset(&comp, 0, sizeof comp);
    if (ioctl(fd, USB_FS_COMPLETE, &comp) != 0) return 0;

    index = (int)comp.ep_index;
    if (index < 0 || index >= EP_MAX) return 0;

    st->pending[index] = 0;

    if (index != EP_IN) return 0;
    if (st->eps[EP_IN].status != 0) return -1;
    if (st->eps[EP_IN].aFrames == 0) return 0;

    return (int)st->buflen[EP_IN][0];
}

static int gip_send(int fd, fs_state *st, unsigned char cmd, unsigned char seq,
                    const unsigned char *payload, int plen);

/* Acknowledges any message that asked to be acknowledged.
 *
 * The packet mirrors the one Linux xpad sends in xpadone_ack_mode_report: an
 * ACK whose payload carries the command, options and payload length of the
 * message being answered, with that message's sequence number echoed in the
 * header. xpad only ever acknowledges the guide button because that is the
 * only message it cares about, but the controller sets the acknowledge flag on
 * others too, and ignoring those leaves it waiting on us. */
static int gip_ack(int fd, fs_state *st, unsigned char acked_cmd,
                   unsigned char seq, unsigned char acked_opts,
                   unsigned char acked_len)
{
    unsigned char body[9];

    memset(body, 0, sizeof body);
    body[1] = acked_cmd;
    body[2] = acked_opts;
    body[3] = acked_len;

    return gip_send(fd, st, GIP_CMD_ACK, seq, body, (int)sizeof body);
}

static int gip_send(int fd, fs_state *st, unsigned char cmd, unsigned char seq,
                    const unsigned char *payload, int plen)
{
    unsigned char *p = st->data[EP_OUT];

    /* If the previous write has not completed there is nothing useful to do
     * but drop this one -- every message sent here is either idempotent or a
     * button acknowledgement that will come round again. */
    if (st->pending[EP_OUT]) return 0;

    p[0] = cmd; p[1] = GIP_FLAG_SYSTEM; p[2] = seq; p[3] = (unsigned char)plen;
    if (plen > 0) memcpy(p + 4, payload, (size_t)plen);

    return fs_start(fd, st, EP_OUT, 4 + plen);
}

/* Sends a message and waits for the write to complete.
 *
 * The initialisation messages have to go out as a sequence rather than be
 * dropped because the previous write is still outstanding, so unlike the
 * fire-and-forget path this one drains the completion before returning. */
static void gip_send_sync(int fd, fs_state *st, unsigned char cmd,
                          unsigned char seq, const unsigned char *payload,
                          int plen)
{
    int i;

    gip_send(fd, st, cmd, seq, payload, plen);
    for (i = 0; i < 200 && st->pending[EP_OUT]; i++) {
        fs_poll(fd, st);
        usleep(1000);
    }
}

/* Brings the controller up.
 *
 * Power-on starts the input stream and the LED message lights the guide, but
 * the authentication message is what keeps a third-party pad alive: Microsoft
 * controllers do not need it, while others power themselves down a few seconds
 * after connecting without it. The packets match the ones Linux xpad sends for
 * non-Microsoft hardware. */
static void gip_init(int fd, fs_state *st, int *seq)
{
    static const unsigned char power_on[] = { 0x00 };
    static const unsigned char auth[]     = { 0x01, 0x00 };
    static const unsigned char led_on[]   = { GIP_LED_GUIDE, GIP_LED_GUIDE_ON, 0x14 };

    gip_send_sync(fd, st, GIP_CMD_SET_DEVICE_STATE, (unsigned char)(*seq)++,
                  power_on, (int)sizeof power_on);
    gip_send_sync(fd, st, GIP_CMD_AUTHENTICATE, (unsigned char)(*seq)++,
                  auth, (int)sizeof auth);
    gip_send_sync(fd, st, GIP_CMD_LED, (unsigned char)(*seq)++,
                  led_on, (int)sizeof led_on);
}

/* ---- translation ------------------------------------------------------- */

/* GIP sticks are signed 16-bit with Y increasing upwards; PlayStation sticks
 * are unsigned 8-bit with Y increasing downwards, so Y is inverted here rather
 * than left for a game to be confused by. */
static uint8_t axis(int16_t v)       { return (uint8_t)(((int)v + 32768) >> 8); }
static uint8_t axis_inv(int16_t v)   { return (uint8_t)((32767 - (int)v) >> 8); }

static void translate(const unsigned char *gip, unsigned char *buf)
{
    const unsigned char *d = gip + 4;
    unsigned m = (unsigned)d[0] | ((unsigned)d[1] << 8);
    unsigned lt = (unsigned)d[2] | ((unsigned)d[3] << 8);
    unsigned rt = (unsigned)d[4] | ((unsigned)d[5] << 8);
    uint32_t b = 0;
    float ident = 1.0f;

    memset(buf, 0, VPAD_REPORT_SIZE);

    if (m & GIP_A)     b |= SCE_CROSS;
    if (m & GIP_B)     b |= SCE_CIRCLE;
    if (m & GIP_X)     b |= SCE_SQUARE;
    if (m & GIP_Y)     b |= SCE_TRIANGLE;
    if (m & GIP_UP)    b |= SCE_UP;
    if (m & GIP_DOWN)  b |= SCE_DOWN;
    if (m & GIP_LEFT)  b |= SCE_LEFT;
    if (m & GIP_RIGHT) b |= SCE_RIGHT;
    if (m & GIP_LB)    b |= SCE_L1;
    if (m & GIP_RB)    b |= SCE_R1;
    if (m & GIP_LS)    b |= SCE_L3;
    if (m & GIP_RS)    b |= SCE_R3;
    if (m & GIP_MENU)  b |= SCE_OPTIONS;
    if (m & GIP_VIEW)  b |= SCE_TOUCHPAD;

    /* The triggers are analogue on both sides, but PlayStation also expects a
     * digital bit once they are meaningfully pressed. */
    if (lt > 128) b |= SCE_L2;
    if (rt > 128) b |= SCE_R2;

    memcpy(buf + VP_BUTTONS, &b, sizeof b);

    buf[VP_STICKS + 0] = axis(le16(d + 6));
    buf[VP_STICKS + 1] = axis_inv(le16(d + 8));
    buf[VP_STICKS + 2] = axis(le16(d + 10));
    buf[VP_STICKS + 3] = axis_inv(le16(d + 12));

    buf[VP_L2] = (uint8_t)(lt >> 2);
    buf[VP_R2] = (uint8_t)(rt >> 2);
    buf[VP_ACTIVE] = 1;

    /* Identity quaternion so the motion block is at least well formed. */
    memcpy(buf + VP_MOTION + 12, &ident, sizeof ident);
}

/* ---- main -------------------------------------------------------------- */

static int find_controller(char *node, size_t n)
{
    unsigned char cfg[1024];
    struct usb_gen_descriptor gd;
    int bus, dev;

    for (bus = 0; bus < 8; bus++)
        for (dev = 1; dev < 16; dev++) {
            int fd;
            snprintf(node, n, "/dev/ugen%d.%d", bus, dev);
            if (access(node, F_OK) != 0) continue;
            fd = open(node, O_RDWR);
            if (fd < 0) continue;

            memset(&gd, 0, sizeof gd);
            gd.ugd_data = cfg;
            gd.ugd_maxlen = sizeof cfg;
            gd.ugd_config_index = 0;
            if (ioctl(fd, USB_GET_FULL_DESC, &gd) == 0 && gd.ugd_actlen > 0 &&
                is_gip_device(cfg, (int)gd.ugd_actlen)) {
                close(fd);
                return 1;
            }
            close(fd);
        }
    return 0;
}

int main(void)
{
    unsigned char cfg[1024];
    struct usb_gen_descriptor gd;
    struct usb_device_descriptor dd;
    struct usb_fs_uninit un;
    char node[64];
    gip_layout lay;
    fs_state st;
    unsigned char report[VPAD_REPORT_SIZE];
    hid_req req;
    hid_del del;
    int usb_fd, hid_fd, cfgnum = 0, seq = 0;
    int presses = 0;
    long acked = 0;
    unsigned seen[12];
    int seen_count = 0, since_input = 0, rebuilds = 0;
    time_t last_keepalive = 0;
    int32_t handle = -1;
    long sent = 0, reports = 0, failed = 0;
    time_t last_note, last_input, last_reinit = 0, last_beat = 0, quit_since = 0;
    int reinits = 0;

    mkdir(STATE_DIR, 0755);
    g_log = fopen(LOG_PATH, "ab");
    if (!g_log) return 1;

    logf_("========================================");
    logf_("FGG-XSense -- Xbox controller on PlayStation 5");

    if (!lock_held()) {
        logf_("  another copy is already running -- exiting");
        notify("FGG-XSense: already running");
        fclose(g_log);
        return 1;
    }

    if (!find_controller(node, sizeof node)) {
        unlink(LOCK_PATH);
        logf_("  no GIP controller found");
        notify("FGG-XSense: no Xbox-protocol controller connected");
        fclose(g_log);
        return 1;
    }

    usb_fd = open(node, O_RDWR);
    if (usb_fd < 0) { unlink(LOCK_PATH);
        logf_("  cannot reopen %s", node); fclose(g_log); return 1; }

    memset(&dd, 0, sizeof dd);
    ioctl(usb_fd, USB_GET_DEVICE_DESC, &dd);

    memset(&gd, 0, sizeof gd);
    gd.ugd_data = cfg; gd.ugd_maxlen = sizeof cfg; gd.ugd_config_index = 0;
    if (ioctl(usb_fd, USB_GET_FULL_DESC, &gd) != 0 ||
        !scan_layout(cfg, (int)gd.ugd_actlen, &lay)) {
        unlink(LOCK_PATH);
        logf_("  cannot read descriptors from %s", node);
        close(usb_fd); fclose(g_log); return 1;
    }

    logf_("  controller %s  VID:PID %04X:%04X  iface %d IN %02X OUT %02X",
          node, word(dd.idVendor), word(dd.idProduct),
          lay.iface, lay.ep_in, lay.ep_out);

    ioctl(usb_fd, USB_SET_CONFIG, &cfgnum);
    ioctl(usb_fd, USB_CLAIM_INTERFACE, &lay.iface);

    if (!fs_setup(usb_fd, &st, &lay)) {
        unlink(LOCK_PATH);
        logf_("  USB transfer setup failed: %s", strerror(errno));
        close(usb_fd); fclose(g_log); return 1;
    }

    if (lay.ep_out >= 0) {
        gip_init(usb_fd, &st, &seq);
        logf_("  power-on, authentication and guide LED sent");
    }

    hid_fd = open("/dev/hid", O_RDWR);
    if (hid_fd < 0) {
        unlink(LOCK_PATH);
        logf_("  /dev/hid open failed: %s", strerror(errno));
        close(usb_fd); fclose(g_log); return 1;
    }

    {
        unsigned char buf[168];
        uint32_t id = 1;   /* user id, not a product id */

        memset(buf, 0, sizeof buf);
        buf[0x00] = VDEV_PROTO;
        memcpy(buf + 0x1C, &id, sizeof id);
        buf[0x20] = VDEV_CLASS;

        memset(&req, 0, sizeof req);
        req.cmd = HID_CMD_ADD;
        req.data = buf;
        req.out = &handle;

        if (ioctl(hid_fd, HID_ADD_DEVICE, &req) != 0) {
            unlink(LOCK_PATH);
        logf_("  virtual pad registration failed: %s", strerror(errno));
            close(hid_fd); close(usb_fd); fclose(g_log);
            return 1;
        }
    }
    logf_("  virtual pad registered, handle %d", handle);
    notify("FGG-XSense: controller ready - hold View+Menu to stop");

    last_note = last_input = last_reinit = time(NULL);
    logf_("  running -- unplug the controller or hold View+Menu to stop");

    for (;;) {
        time_t now;
        int n;

        fs_start(usb_fd, &st, EP_IN, lay.mps_in ? lay.mps_in : 64);
        n = fs_poll(usb_fd, &st);
        if (n == 0) usleep(1000);

        /* Anything the controller sends counts as it being alive, and anything
         * flagged for acknowledgement gets answered before it is interpreted.
         * This has to come first and cover every message type: the previous
         * version acknowledged only the guide button, and left the controller
         * waiting on everything else. */
        if (n >= 4) {
            unsigned char mcmd = st.data[EP_IN][0];
            unsigned char mopt = st.data[EP_IN][1];
            unsigned char mseq = st.data[EP_IN][2];
            unsigned char mlen = st.data[EP_IN][3];

            last_input = time(NULL);
            since_input = 0;

            if (mopt & GIP_OPT_ACK) {
                gip_ack(usb_fd, &st, mcmd, mseq, mopt, mlen);
                acked++;
            }

            /* Record each distinct message shape once, so what the controller
             * actually asks for is visible rather than assumed. */
            if (mcmd != GIP_LL_INPUT_REPORT && seen_count < 12) {
                int k, known = 0;
                for (k = 0; k < seen_count; k++)
                    if (seen[k] == ((unsigned)mcmd << 8 | mopt)) { known = 1; break; }
                if (!known) {
                    seen[seen_count++] = (unsigned)mcmd << 8 | mopt;
                    logf_("  message cmd %02X opts %02X len %u%s",
                          mcmd, mopt, mlen,
                          (mopt & GIP_OPT_ACK) ? "  [ack sent]" : "");
                }
            }
        }

        /* Acknowledged above along with every other system message; there is
         * no PlayStation button to forward it to. */
        if (n >= 6 && st.data[EP_IN][0] == GIP_CMD_GUIDE_BUTTON) {
            presses++;
            continue;
        }

        if (n >= 4 + 14 && st.data[EP_IN][0] == GIP_LL_INPUT_REPORT) {
            unsigned mask = (unsigned)st.data[EP_IN][4] |
                            ((unsigned)st.data[EP_IN][5] << 8);

            last_input = time(NULL);
            reports++;

            /* Deliberate shutdown, held rather than tapped so it cannot be
             * triggered by accident mid-game. */
            if ((mask & (GIP_VIEW | GIP_MENU)) == (GIP_VIEW | GIP_MENU)) {
                if (quit_since == 0) quit_since = time(NULL);
                else if (time(NULL) - quit_since >= QUIT_HOLD_SECONDS) {
                    logf_("  View+Menu held -- shutting down");
                    break;
                }
            } else {
                quit_since = 0;
            }

            translate(st.data[EP_IN], report);

            memset(&req, 0, sizeof req);
            req.cmd = (uint32_t)handle;
            req.data = report;
            req.out = NULL;

            if (ioctl(hid_fd, HID_INSERT_DATA, &req) == 0) sent++;
            else if (sent == 0 && reports == 1)
                logf_("  first InsertData failed: %s", strerror(errno));
        } else if (n < 0) {
            /* Back off briefly so a refused transfer does not spin the CPU. */
            failed++;
            usleep(2000);
        }

        now = time(NULL);
        if (now != last_beat) { last_beat = now; lock_touch(); }

        /* Keep the controller fed while it is still healthy. Waiting for it to
         * fall silent first means losing input for a few seconds every time it
         * does, whereas resending costs three small writes. */
        if (now - last_keepalive >= KEEPALIVE_SECONDS) {
            last_keepalive = now;
            gip_init(usb_fd, &st, &seq);
        }

        /* A controller that has gone quiet is far more often one that powered
         * itself down than one that was unplugged, and re-running the
         * initialisation brings it straight back. */
        if (now - last_input >= REINIT_SECONDS && now - last_reinit >= REINIT_SECONDS) {
            last_reinit = now;
            last_keepalive = now;
            reinits++;
            gip_init(usb_fd, &st, &seq);
            logf_("  controller quiet -- reinitialised (%d)", reinits);

            /* Resending GIP messages cannot rescue a wedged endpoint, so once
             * several reinitialisations in a row have failed to bring input
             * back, rebuild the USB side from scratch instead of repeating
             * something that is evidently not working. */
            if (++since_input >= REINITS_BEFORE_REBUILD) {
                since_input = 0;
                rebuilds++;
                logf_("  reinit not helping -- rebuilding the USB connection");

                memset(&un, 0, sizeof un);
                ioctl(usb_fd, USB_FS_UNINIT, &un);
                close(usb_fd);

                usb_fd = open(node, O_RDWR);
                if (usb_fd < 0) {
                    logf_("  cannot reopen %s: %s", node, strerror(errno));
                    break;
                }
                ioctl(usb_fd, USB_SET_CONFIG, &cfgnum);
                ioctl(usb_fd, USB_CLAIM_INTERFACE, &lay.iface);
                if (!fs_setup(usb_fd, &st, &lay)) {
                    logf_("  rebuild failed: %s", strerror(errno));
                    break;
                }
                gip_init(usb_fd, &st, &seq);
                logf_("  USB connection rebuilt (%d)", rebuilds);
            }
        }

        if (now - last_input >= UNPLUG_SECONDS) {
            logf_("  no input for %d seconds (%ld read, %ld failed transfers)",
                  UNPLUG_SECONDS, reports, failed);
            break;
        }
        if (now - last_note >= 60) {
            last_note = now;
            logf_("  %ld read, %ld delivered, %ld acked, %d reinit(s), %d rebuild(s)",
                  reports, sent, acked, reinits, rebuilds);
        }
    }

    logf_("  total: %ld read, %ld delivered, %ld failed, %ld acked, "
          "%d guide press(es), %d reinit(s), %d rebuild(s)",
          reports, sent, failed, acked, presses, reinits, rebuilds);

    memset(&del, 0, sizeof del);
    del.cmd = HID_CMD_DEL;
    del.handle = (uint32_t)handle;
    if (ioctl(hid_fd, HID_DEL_DEVICE, &del) != 0)
        logf_("  DeleteDevice failed: %s", strerror(errno));
    else
        logf_("  virtual pad removed");

    unlink(LOCK_PATH);

    memset(&un, 0, sizeof un);
    ioctl(usb_fd, USB_FS_UNINIT, &un);
    close(hid_fd);
    close(usb_fd);

    if (sent > 0)
        notify("FGG-XSense: stopped - %ld inputs delivered", sent);
    else
        notify("FGG-XSense: no input delivered - see log");

    fclose(g_log);
    return 0;
}
