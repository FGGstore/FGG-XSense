/* probe -- read-only survey of the USB hardware wireless support depends on.
 *
 * Answers, from the console itself, the questions the wireless backends are
 * built on: the Xbox Wireless Adapter's real endpoint layout, whether vendor
 * control requests and large bulk slots work through ugen on this firmware,
 * and how the console's own Bluetooth chip is laid out.
 *
 * Nothing here changes the state of a radio. The Bluetooth chip is only read
 * for descriptors -- no configuration, no claim, no transfer slots, and not a
 * single HCI command, so the system's Bluetooth and the DualSense on it are
 * left alone. The adapter gets register reads, a claim and slot opens, but no
 * transfer is ever started, so no firmware is sent and nothing is written to
 * the chip.
 */
#include "log.h"

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR "/data/fgg-xsense"
#define LOG_PATH  STATE_DIR "/probe.log"
#define LOCK_PATH STATE_DIR "/xsense.lock"

/* Matches the payload's own staleness rule: a lock refreshed this recently
 * belongs to a running copy, which may be holding devices open. */
#define LOCK_STALE_SECONDS 15

#ifndef USB_SHORT_XFER_OK
#define USB_SHORT_XFER_OK 0x0004
#endif

/* Microsoft's wireless adapters. 02FE is the slim model 1914. */
#define MS_VENDOR 0x045E
static const unsigned adapter_pids[] = { 0x02E6, 0x02FE, 0x02F9, 0x091E };

/* Adapter vendor requests (xow dongle/mt76.h). */
#define MT_VEND_MULTI_READ 0x07
#define MT_VEND_READ_CFG   0x47

/* Bluetooth controllers present interface class E0 (wireless controller),
 * subclass 01, protocol 01. */
#define IFACE_CLASS_WIRELESS 0xE0

#define CFG_MAX   4096
#define FS_SLOTS  24
#define MAX_EPS   16

typedef struct {
    unsigned iface, alt, addr, type, mps;
} ep_info;

static unsigned le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void set16(uByte *w, unsigned v)
{
    w[0] = (uByte)(v & 0xFF);
    w[1] = (uByte)(v >> 8);
}

static const char *ep_type(unsigned attr)
{
    static const char *names[] = { "control", "isoc", "bulk", "interrupt" };
    return names[attr & 0x03];
}

static void hexdump(const char *what, const unsigned char *p, int len)
{
    char line[3 * 32 + 1];
    int i, o;

    log_line("  %s: %d bytes", what, len);
    for (o = 0; o < len; o += 32) {
        int n = len - o < 32 ? len - o : 32;
        for (i = 0; i < n; i++)
            snprintf(line + 3 * i, 4, "%02X ", p[o + i]);
        log_line("    %04X  %s", o, line);
    }
}

/* Walks a configuration descriptor, logging every function, interface,
 * alternate setting and endpoint, and collecting the endpoints. */
static int parse_config(const unsigned char *buf, int len,
                        ep_info *eps, int maxeps, int *has_wireless)
{
    int i = 0, n = 0;
    unsigned iface = 0, alt = 0;

    *has_wireless = 0;

    while (i + 2 <= len) {
        unsigned blen = buf[i], type = buf[i + 1];
        if (blen < 2 || i + (int)blen > len) break;

        if (type == UDESC_CONFIG && blen >= 9) {
            log_line("  config value %u: %u interface(s), %u bytes total",
                     buf[i + 5], buf[i + 4], le16(buf + i + 2));
        } else if (type == 0x0B && blen >= 8) {
            log_line("  function: interfaces %u..%u class %02X/%02X/%02X",
                     buf[i + 2], buf[i + 2] + buf[i + 3] - 1,
                     buf[i + 4], buf[i + 5], buf[i + 6]);
        } else if (type == UDESC_INTERFACE && blen >= 9) {
            iface = buf[i + 2];
            alt = buf[i + 3];
            if (buf[i + 5] == IFACE_CLASS_WIRELESS) *has_wireless = 1;
            log_line("  interface %u alt %u: %u endpoint(s) class %02X/%02X/%02X",
                     iface, alt, buf[i + 4], buf[i + 5], buf[i + 6], buf[i + 7]);
        } else if (type == UDESC_ENDPOINT && blen >= 7) {
            unsigned addr = buf[i + 2], attr = buf[i + 3];
            unsigned mps = le16(buf + i + 4) & 0x7FF;
            log_line("    endpoint %02X %s %s, max packet %u, interval %u",
                     addr, (addr & UE_DIR_IN) ? "IN " : "OUT", ep_type(attr),
                     mps, buf[i + 6]);
            if (n < maxeps) {
                eps[n].iface = iface;
                eps[n].alt = alt;
                eps[n].addr = addr;
                eps[n].type = attr & 0x03;
                eps[n].mps = mps;
                n++;
            }
        }
        i += (int)blen;
    }
    return n;
}

static int read_config(int fd, unsigned char *buf, int maxlen)
{
    struct usb_gen_descriptor gd;

    memset(&gd, 0, sizeof gd);
    gd.ugd_data = buf;
    gd.ugd_maxlen = (uint16_t)maxlen;
    gd.ugd_config_index = 0;

    if (ioctl(fd, USB_GET_FULL_DESC, &gd) != 0) {
        log_line("  GET_FULL_DESC: %s", strerror(errno));
        return 0;
    }
    return (int)gd.ugd_actlen;
}

/* A four-byte vendor read from the adapter: wValue carries the upper half of
 * the address and wIndex the lower, as xone does (xow's 16-bit addresses are
 * the same thing with wValue 0). */
static void vendor_read(int fd, const char *name, unsigned req, uint32_t addr)
{
    struct usb_ctl_request r;
    unsigned char data[4];

    memset(&r, 0, sizeof r);
    memset(data, 0, sizeof data);
    r.ucr_data = data;
    r.ucr_flags = USB_SHORT_XFER_OK;
    r.ucr_request.bmRequestType = UT_READ_VENDOR_DEVICE;
    r.ucr_request.bRequest = (uByte)req;
    set16(r.ucr_request.wValue, addr >> 16);
    set16(r.ucr_request.wIndex, addr & 0xFFFF);
    set16(r.ucr_request.wLength, sizeof data);

    if (ioctl(fd, USB_DO_REQUEST, &r) != 0) {
        log_line("  %s (req %02X @%04X): %s", name, req, (unsigned)addr,
                 strerror(errno));
        return;
    }
    log_line("  %s (req %02X @%04X): %08X  (%u bytes)", name, req,
             (unsigned)addr,
             (unsigned)data[0] | ((unsigned)data[1] << 8) |
             ((unsigned)data[2] << 16) | ((unsigned)data[3] << 24),
             r.ucr_actlen);
}

/* Claims the adapter's first interface and opens a transfer slot on each of
 * its endpoints, sized for what the driver will need. No transfer is started:
 * this only proves the slots can be had. */
static void adapter_slots(int fd, const ep_info *eps, int neps)
{
    struct usb_fs_endpoint fsep[FS_SLOTS];
    struct usb_fs_init init;
    struct usb_fs_uninit un;
    struct pollfd pfd;
    int cfg = 0, iface = 0, i, slot = 0, rc;
    struct timespec t0, t1;

    if (ioctl(fd, USB_SET_CONFIG, &cfg) != 0)
        log_line("  SET_CONFIG 0: %s", strerror(errno));
    if (ioctl(fd, USB_CLAIM_INTERFACE, &iface) != 0)
        log_line("  CLAIM_INTERFACE 0: %s", strerror(errno));

    memset(fsep, 0, sizeof fsep);
    memset(&init, 0, sizeof init);
    init.pEndpoints = fsep;
    init.ep_index_max = FS_SLOTS;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) {
        log_line("  FS_INIT %d slots: %s", FS_SLOTS, strerror(errno));
        goto release;
    }
    log_line("  FS_INIT %d slots: ok", FS_SLOTS);

    for (i = 0; i < neps; i++) {
        struct usb_fs_open op;

        if (eps[i].iface != 0 || eps[i].alt != 0) continue;

        memset(&op, 0, sizeof op);
        /* Firmware goes out in 0x3800-byte chunks plus an 8-byte frame;
         * the WLAN IN pipe may carry aggregated frames up to 0x8400. */
        if (!(eps[i].addr & UE_DIR_IN))  op.max_bufsize = 0x3808;
        else if (eps[i].addr == 0x84)    op.max_bufsize = 0x8400;
        else                             op.max_bufsize = 0x654;
        op.max_frames = 1;
        op.ep_index = (uint8_t)slot;
        op.ep_no = (uint8_t)eps[i].addr;

        if (ioctl(fd, USB_FS_OPEN, &op) != 0) {
            log_line("  FS_OPEN %02X buf %u: %s", eps[i].addr, op.max_bufsize,
                     strerror(errno));
            continue;
        }
        log_line("  FS_OPEN %02X buf %u: ok, max packet %u", eps[i].addr,
                 op.max_bufsize, op.max_packet_length);
        slot++;
    }

    /* A second slot on the WLAN IN pipe: the driver wants many reads in
     * flight on one endpoint. */
    for (i = 0; i < neps; i++) {
        struct usb_fs_open op;
        if (eps[i].addr != 0x84 || eps[i].iface != 0 || eps[i].alt != 0) continue;
        memset(&op, 0, sizeof op);
        op.max_bufsize = 0x8400;
        op.max_frames = 1;
        op.ep_index = (uint8_t)slot;
        op.ep_no = 0x84;
        rc = ioctl(fd, USB_FS_OPEN, &op);
        log_line("  FS_OPEN 84 again (slot %d): %s", slot,
                 rc == 0 ? "ok" : strerror(errno));
        break;
    }

    /* Nothing is queued, so this should simply time out; what matters is
     * that poll() on a ugen node behaves at all. */
    pfd.fd = fd;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = poll(&pfd, 1, 100);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    log_line("  poll (nothing queued): rc %d revents %04X errno %s, %ld ms",
             rc, (unsigned)pfd.revents, rc < 0 ? strerror(errno) : "-",
             (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                    (t1.tv_nsec - t0.tv_nsec) / 1000000));

    memset(&un, 0, sizeof un);
    if (ioctl(fd, USB_FS_UNINIT, &un) != 0)
        log_line("  FS_UNINIT: %s", strerror(errno));

release:
    if (ioctl(fd, USB_RELEASE_INTERFACE, &iface) != 0)
        log_line("  RELEASE_INTERFACE 0: %s", strerror(errno));
}

static int is_adapter(unsigned vid, unsigned pid)
{
    unsigned i;
    if (vid != MS_VENDOR) return 0;
    for (i = 0; i < sizeof adapter_pids / sizeof adapter_pids[0]; i++)
        if (pid == adapter_pids[i]) return 1;
    return 0;
}

static void probe_node(const char *node, int *adapters, int *radios)
{
    static unsigned char cfg[CFG_MAX];
    struct usb_device_descriptor dd;
    ep_info eps[MAX_EPS];
    unsigned vid, pid;
    int fd, len, neps, wireless, adapter;

    fd = open(node, O_RDWR);
    if (fd < 0) {
        log_line("%s: open: %s", node, strerror(errno));
        return;
    }

    /* USB_GET_DEVICEINFO is refused here (ENOTTY); the device descriptor
     * is what FGG-XSense already reads successfully. */
    memset(&dd, 0, sizeof dd);
    if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) != 0) {
        log_line("%s: GET_DEVICE_DESC: %s", node, strerror(errno));
        close(fd);
        return;
    }
    vid = le16(dd.idVendor);
    pid = le16(dd.idProduct);

    log_line("%s: %04X:%04X class %02X/%02X/%02X USB %X.%02X release %X.%02X, %u config(s)",
             node, vid, pid, dd.bDeviceClass, dd.bDeviceSubClass,
             dd.bDeviceProtocol, dd.bcdUSB[1], dd.bcdUSB[0],
             dd.bcdDevice[1], dd.bcdDevice[0], dd.bNumConfigurations);

    len = read_config(fd, cfg, (int)sizeof cfg);
    adapter = is_adapter(vid, pid);

    /* Every device's layout is logged in brief; the two that matter get the
     * raw bytes too. */
    neps = len > 0 ? parse_config(cfg, len, eps, MAX_EPS, &wireless) : 0;
    if (len <= 0) wireless = 0;

    if (wireless && !adapter) {
        (*radios)++;
        log_line("  ^ Bluetooth controller: descriptors only, nothing sent");
        hexdump("configuration descriptor", cfg, len);
    }

    if (adapter) {
        (*adapters)++;
        log_line("  ^ Xbox Wireless Adapter");
        hexdump("configuration descriptor", cfg, len);
        vendor_read(fd, "firmware loaded (FCE_DMA_ADDR)", MT_VEND_READ_CFG, 0x0230);
        vendor_read(fd, "ASIC version", MT_VEND_MULTI_READ, 0x0000);
        vendor_read(fd, "MAC_CSR0", MT_VEND_MULTI_READ, 0x1000);
        adapter_slots(fd, eps, neps);
    }

    close(fd);
}

int main(void)
{
    struct stat stv;
    int bus, unit, adapters = 0, radios = 0;

    if (!log_open(STATE_DIR, LOG_PATH, NULL)) return 1;

    log_line("========================================");
    log_line("FGG-XSense probe -- read-only USB survey");

    if (stat(LOCK_PATH, &stv) == 0 &&
        time(NULL) - stv.st_mtime < LOCK_STALE_SECONDS) {
        log_line("FGG-XSense is running -- stop it (View+Menu) and probe again");
        notify("Probe: stop FGG-XSense first (hold View+Menu)");
        log_close();
        return 1;
    }

    for (bus = 0; bus < 8; bus++) {
        for (unit = 1; unit < 16; unit++) {
            char node[32];
            snprintf(node, sizeof node, "/dev/ugen%d.%d", bus, unit);
            if (access(node, F_OK) == 0) probe_node(node, &adapters, &radios);
        }
    }

    log_line("done: %d adapter(s), %d Bluetooth controller(s)", adapters, radios);
    notify("Probe done: %d adapter(s), %d Bluetooth chip(s) - see probe.log",
           adapters, radios);
    log_close();
    return 0;
}
