#include "gip.h"
#include "log.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The interface signature every Xbox-protocol controller presents, and that
 * nothing else does. */
#define GIP_IFACE_CLASS    0xFF
#define GIP_IFACE_SUBCLASS 0x47
#define GIP_IFACE_PROTOCOL 0xD0

#define XFER_BUF 256
#define EP_IN    0
#define EP_OUT   1
#define EP_MAX   2

/* Guide LED message payload: which LED, the pattern, and a brightness. */
#define GIP_LED_GUIDE    0x00
#define GIP_LED_GUIDE_ON 0x01
#define GIP_LED_LEVEL    0x14

/* How long gip_init waits for each write to land, in milliseconds. */
#define SEND_TIMEOUT_MS 200

struct gip_device {
    int  fd;
    char node[64];
    unsigned vendor, product;

    int iface, ep_in, ep_out, mps_in;
    int seq;

    struct usb_fs_endpoint eps[EP_MAX];
    void     *bufptr[EP_MAX][1];
    uint32_t  buflen[EP_MAX][1];
    unsigned char data[EP_MAX][XFER_BUF];
    int pending[EP_MAX];
};

static gip_device g_dev;

static unsigned word(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* ---- descriptors ------------------------------------------------------- */

/* Walks a configuration descriptor for a GIP interface and the interrupt
 * endpoints belonging to it. Endpoints seen outside that interface are
 * ignored, so a composite device's other functions cannot be mistaken for the
 * gamepad. */
static int scan_layout(gip_device *dev, const unsigned char *buf, int len)
{
    int i = 0, in_gip = 0;

    dev->iface = dev->ep_in = dev->ep_out = -1;
    dev->mps_in = 0;

    while (i + 2 <= len) {
        unsigned blen = buf[i], btype = buf[i + 1];
        if (blen == 0 || i + (int)blen > len) break;

        if (btype == UDESC_INTERFACE && blen >= 9) {
            in_gip = (buf[i + 5] == GIP_IFACE_CLASS &&
                      buf[i + 6] == GIP_IFACE_SUBCLASS &&
                      buf[i + 7] == GIP_IFACE_PROTOCOL);
            if (in_gip && dev->iface < 0) dev->iface = (int)buf[i + 2];
            else in_gip = in_gip && (dev->iface == (int)buf[i + 2]);
        } else if (btype == UDESC_ENDPOINT && blen >= 7 && in_gip) {
            unsigned addr = buf[i + 2], attr = buf[i + 3];
            unsigned mps = (unsigned)buf[i + 4] | ((unsigned)buf[i + 5] << 8);

            if ((attr & UE_XFERTYPE) == UE_INTERRUPT) {
                if ((addr & UE_DIR_IN) && dev->ep_in < 0) {
                    dev->ep_in = (int)addr;
                    dev->mps_in = (int)mps;
                } else if (!(addr & UE_DIR_IN) && dev->ep_out < 0) {
                    dev->ep_out = (int)addr;
                }
            }
        }
        i += (int)blen;
    }

    return dev->iface >= 0 && dev->ep_in >= 0;
}

/* ---- transfers --------------------------------------------------------- */

static int fs_setup(gip_device *dev)
{
    struct usb_fs_init init;
    struct usb_fs_open op;
    int i;

    memset(dev->eps, 0, sizeof dev->eps);
    memset(dev->pending, 0, sizeof dev->pending);

    for (i = 0; i < EP_MAX; i++) {
        dev->bufptr[i][0] = dev->data[i];
        dev->buflen[i][0] = XFER_BUF;
        dev->eps[i].ppBuffer = dev->bufptr[i];
        dev->eps[i].pLength = dev->buflen[i];
        dev->eps[i].nFrames = 1;
        dev->eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
        dev->eps[i].timeout = 1000;
    }

    memset(&init, 0, sizeof init);
    init.pEndpoints = dev->eps;
    init.ep_index_max = EP_MAX;
    if (ioctl(dev->fd, USB_FS_INIT, &init) != 0) return 0;

    memset(&op, 0, sizeof op);
    op.max_bufsize = XFER_BUF;
    op.max_frames = 1;
    op.ep_index = EP_IN;
    op.ep_no = (uint8_t)dev->ep_in;
    if (ioctl(dev->fd, USB_FS_OPEN, &op) != 0) return 0;

    if (dev->ep_out >= 0) {
        memset(&op, 0, sizeof op);
        op.max_bufsize = XFER_BUF;
        op.max_frames = 1;
        op.ep_index = EP_OUT;
        op.ep_no = (uint8_t)dev->ep_out;
        ioctl(dev->fd, USB_FS_OPEN, &op);   /* input alone is still useful */
    }
    return 1;
}

/* Queues a transfer unless one is already outstanding on that endpoint. */
static int fs_start(gip_device *dev, int index, int length)
{
    struct usb_fs_start start;

    if (dev->pending[index]) return 1;

    dev->buflen[index][0] = (uint32_t)length;
    dev->eps[index].nFrames = 1;
    dev->eps[index].aFrames = 0;
    dev->eps[index].status = 0;

    memset(&start, 0, sizeof start);
    start.ep_index = (uint8_t)index;
    if (ioctl(dev->fd, USB_FS_START, &start) != 0) return 0;

    dev->pending[index] = 1;
    return 1;
}

/* Collects one completion, for whichever endpoint finished.
 *
 * USB_FS_COMPLETE reports a completed endpoint rather than the one the caller
 * is interested in, so the index it returns has to be honoured -- reading the
 * IN endpoint's state after an OUT completion silently produces nonsense.
 */
static int fs_poll(gip_device *dev)
{
    struct usb_fs_complete comp;
    int index;

    memset(&comp, 0, sizeof comp);
    if (ioctl(dev->fd, USB_FS_COMPLETE, &comp) != 0) return 0;

    index = (int)comp.ep_index;
    if (index < 0 || index >= EP_MAX) return 0;

    dev->pending[index] = 0;

    if (index != EP_IN) return 0;
    if (dev->eps[EP_IN].status != 0) return -1;
    if (dev->eps[EP_IN].aFrames == 0) return 0;

    return (int)dev->buflen[EP_IN][0];
}

/* ---- messages ---------------------------------------------------------- */

static int gip_send(gip_device *dev, unsigned char cmd, unsigned char seq,
                    const unsigned char *payload, int plen)
{
    unsigned char *p = dev->data[EP_OUT];

    if (dev->ep_out < 0) return 0;

    /* Dropping a message whose predecessor is still in flight is safe: every
     * message sent from here is either idempotent or an acknowledgement that
     * will be asked for again. */
    if (dev->pending[EP_OUT]) return 0;

    p[0] = cmd;
    p[1] = GIP_OPT_SYSTEM;
    p[2] = seq;
    p[3] = (unsigned char)plen;
    if (plen > 0) memcpy(p + 4, payload, (size_t)plen);

    return fs_start(dev, EP_OUT, 4 + plen);
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
static void gip_send_sync(gip_device *dev, unsigned char cmd,
                          const unsigned char *payload, int plen)
{
    struct usb_fs_complete comp;
    int waited = 0;

    if (!gip_send(dev, cmd, (unsigned char)dev->seq++, payload, plen)) return;

    while (dev->pending[EP_OUT] && waited < SEND_TIMEOUT_MS) {
        memset(&comp, 0, sizeof comp);
        if (ioctl(dev->fd, USB_FS_COMPLETE, &comp) == 0) {
            int index = (int)comp.ep_index;
            if (index >= 0 && index < EP_MAX) dev->pending[index] = 0;
        }
        usleep(1000);
        waited++;
    }
}

void gip_init(gip_device *dev)
{
    static const unsigned char power_on[] = { 0x00 };
    static const unsigned char auth[]     = { 0x01, 0x00 };
    static const unsigned char led_on[]   = { GIP_LED_GUIDE, GIP_LED_GUIDE_ON,
                                              GIP_LED_LEVEL };

    gip_send_sync(dev, GIP_CMD_SET_DEVICE_STATE, power_on, (int)sizeof power_on);
    gip_send_sync(dev, GIP_CMD_AUTHENTICATE, auth, (int)sizeof auth);
    gip_send_sync(dev, GIP_CMD_LED, led_on, (int)sizeof led_on);
}

void gip_acknowledge(gip_device *dev, const unsigned char *msg)
{
    unsigned char body[9];

    /* The payload carries the command, options and length of the message being
     * answered; the header echoes its sequence number. Mirrors the packet
     * Linux xpad sends in xpadone_ack_mode_report. */
    memset(body, 0, sizeof body);
    body[1] = msg[0];
    body[2] = msg[1];
    body[3] = msg[3];

    gip_send(dev, GIP_CMD_ACK, msg[2], body, (int)sizeof body);
}

int gip_read(gip_device *dev, const unsigned char **msg)
{
    int n;

    fs_start(dev, EP_IN, dev->mps_in ? dev->mps_in : 64);
    n = fs_poll(dev);
    if (n > 0) *msg = dev->data[EP_IN];
    return n;
}

/* ---- lifecycle --------------------------------------------------------- */

/* Configures, claims and initialises an already-open device node. */
static int bring_up(gip_device *dev)
{
    int cfg = 0;

    ioctl(dev->fd, USB_SET_CONFIG, &cfg);
    ioctl(dev->fd, USB_CLAIM_INTERFACE, &dev->iface);

    if (!fs_setup(dev)) return 0;

    gip_init(dev);
    return 1;
}

static int read_descriptors(int fd, unsigned char *cfg, int cfglen, int *actlen)
{
    struct usb_gen_descriptor gd;

    memset(&gd, 0, sizeof gd);
    gd.ugd_data = cfg;
    gd.ugd_maxlen = (uint16_t)cfglen;
    gd.ugd_config_index = 0;

    if (ioctl(fd, USB_GET_FULL_DESC, &gd) != 0 || gd.ugd_actlen == 0) return 0;
    *actlen = (int)gd.ugd_actlen;
    return 1;
}

static int is_gip(const unsigned char *cfg, int len)
{
    int i = 0;
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

gip_device *gip_open(char *why, int whylen)
{
    gip_device *dev = &g_dev;
    unsigned char cfg[1024];
    int bus, unit, found = 0, cfg_len = 0;

    memset(dev, 0, sizeof *dev);
    dev->fd = -1;

    for (bus = 0; bus < 8 && !found; bus++) {
        for (unit = 1; unit < 16 && !found; unit++) {
            struct usb_device_descriptor dd;
            int fd;

            snprintf(dev->node, sizeof dev->node, "/dev/ugen%d.%d", bus, unit);
            if (access(dev->node, F_OK) != 0) continue;

            fd = open(dev->node, O_RDWR);
            if (fd < 0) continue;

            if (!read_descriptors(fd, cfg, (int)sizeof cfg, &cfg_len) ||
                !is_gip(cfg, cfg_len)) {
                close(fd);
                continue;
            }

            memset(&dd, 0, sizeof dd);
            ioctl(fd, USB_GET_DEVICE_DESC, &dd);
            dev->vendor = word((const unsigned char *)&dd.idVendor);
            dev->product = word((const unsigned char *)&dd.idProduct);

            dev->fd = fd;
            found = 1;
        }
    }

    if (!found) {
        snprintf(why, (size_t)whylen, "no Xbox-protocol controller connected");
        return NULL;
    }

    /* The descriptor's own length, not the buffer's: walking past the end
     * reads uninitialised stack and can invent endpoints that do not exist. */
    if (!scan_layout(dev, cfg, cfg_len)) {
        snprintf(why, (size_t)whylen, "%s has no usable GIP endpoints", dev->node);
        close(dev->fd);
        dev->fd = -1;
        return NULL;
    }

    if (!bring_up(dev)) {
        snprintf(why, (size_t)whylen, "transfer setup failed: %s", strerror(errno));
        close(dev->fd);
        dev->fd = -1;
        return NULL;
    }

    snprintf(why, (size_t)whylen, "interface %d, IN %02X, OUT %02X",
             dev->iface, dev->ep_in, dev->ep_out);
    return dev;
}

int gip_rebuild(gip_device *dev)
{
    struct usb_fs_uninit un;

    memset(&un, 0, sizeof un);
    ioctl(dev->fd, USB_FS_UNINIT, &un);
    close(dev->fd);

    dev->fd = open(dev->node, O_RDWR);
    if (dev->fd < 0) return 0;

    return bring_up(dev);
}

void gip_close(gip_device *dev)
{
    struct usb_fs_uninit un;

    if (!dev || dev->fd < 0) return;

    memset(&un, 0, sizeof un);
    ioctl(dev->fd, USB_FS_UNINIT, &un);
    close(dev->fd);
    dev->fd = -1;
}

const char *gip_node(const gip_device *dev)    { return dev->node; }
unsigned    gip_vendor(const gip_device *dev)  { return dev->vendor; }
unsigned    gip_product(const gip_device *dev) { return dev->product; }
