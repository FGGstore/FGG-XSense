#include "usb/ugen.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef USB_SHORT_XFER_OK
#define USB_SHORT_XFER_OK 0x0004
#endif

struct ugen {
    int  fd;
    char node[32];
    int  nslots;

    struct usb_fs_endpoint eps[UGEN_MAX_SLOTS];
    void          *bufptr[UGEN_MAX_SLOTS][1];
    uint32_t       buflen[UGEN_MAX_SLOTS][1];
    unsigned char *data[UGEN_MAX_SLOTS];
    uint32_t       cap[UGEN_MAX_SLOTS];
    int            pending[UGEN_MAX_SLOTS];
};

static unsigned word(const uByte *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void set_word(uByte *w, unsigned v)
{
    w[0] = (uByte)(v & 0xFF);
    w[1] = (uByte)(v >> 8);
}

ugen *ugen_open(const char *node)
{
    ugen *u = calloc(1, sizeof *u);

    if (!u) return NULL;

    u->fd = open(node, O_RDWR);
    if (u->fd < 0) {
        int e = errno;
        free(u);
        errno = e;
        return NULL;
    }
    snprintf(u->node, sizeof u->node, "%s", node);
    return u;
}

void ugen_close(ugen *u)
{
    int i;

    if (!u) return;

    if (u->nslots > 0) {
        struct usb_fs_uninit un;
        memset(&un, 0, sizeof un);
        ioctl(u->fd, USB_FS_UNINIT, &un);
    }
    close(u->fd);

    for (i = 0; i < UGEN_MAX_SLOTS; i++) free(u->data[i]);
    free(u);
}

const char *ugen_node(const ugen *u) { return u->node; }

int ugen_ids(ugen *u, unsigned *vendor, unsigned *product)
{
    struct usb_device_descriptor dd;

    memset(&dd, 0, sizeof dd);
    if (ioctl(u->fd, USB_GET_DEVICE_DESC, &dd) != 0) return 0;
    *vendor = word(dd.idVendor);
    *product = word(dd.idProduct);
    return 1;
}

int ugen_config_desc(ugen *u, unsigned char *buf, int maxlen)
{
    struct usb_gen_descriptor gd;

    memset(&gd, 0, sizeof gd);
    gd.ugd_data = buf;
    gd.ugd_maxlen = (uint16_t)maxlen;
    gd.ugd_config_index = 0;

    if (ioctl(u->fd, USB_GET_FULL_DESC, &gd) != 0) return 0;
    return (int)gd.ugd_actlen;
}

int ugen_set_config(ugen *u, int index)
{
    return ioctl(u->fd, USB_SET_CONFIG, &index) == 0;
}

int ugen_claim(ugen *u, int iface)
{
    return ioctl(u->fd, USB_CLAIM_INTERFACE, &iface) == 0;
}

int ugen_ctrl(ugen *u, unsigned type, unsigned request, unsigned value,
              unsigned index, void *data, unsigned len)
{
    struct usb_ctl_request r;

    memset(&r, 0, sizeof r);
    r.ucr_data = data;
    r.ucr_flags = USB_SHORT_XFER_OK;
    r.ucr_request.bmRequestType = (uByte)type;
    r.ucr_request.bRequest = (uByte)request;
    set_word(r.ucr_request.wValue, value);
    set_word(r.ucr_request.wIndex, index);
    set_word(r.ucr_request.wLength, len);

    if (ioctl(u->fd, USB_DO_REQUEST, &r) != 0) return -1;
    return (int)r.ucr_actlen;
}

int ugen_fs_init(ugen *u, int nslots)
{
    struct usb_fs_init init;

    if (nslots < 1 || nslots > UGEN_MAX_SLOTS) {
        errno = EINVAL;
        return 0;
    }

    memset(u->eps, 0, sizeof u->eps);
    memset(u->pending, 0, sizeof u->pending);

    memset(&init, 0, sizeof init);
    init.pEndpoints = u->eps;
    init.ep_index_max = (uint8_t)nslots;
    if (ioctl(u->fd, USB_FS_INIT, &init) != 0) return 0;

    u->nslots = nslots;
    return 1;
}

int ugen_fs_open(ugen *u, int slot, unsigned ep, uint32_t bufsize, int timeout_ms)
{
    struct usb_fs_open op;

    if (slot < 0 || slot >= u->nslots) {
        errno = EINVAL;
        return 0;
    }

    if (u->cap[slot] < bufsize) {
        unsigned char *p = realloc(u->data[slot], bufsize);
        if (!p) return 0;
        u->data[slot] = p;
        u->cap[slot] = bufsize;
    }

    u->bufptr[slot][0] = u->data[slot];
    u->buflen[slot][0] = bufsize;
    u->eps[slot].ppBuffer = u->bufptr[slot];
    u->eps[slot].pLength = u->buflen[slot];
    u->eps[slot].nFrames = 1;
    u->eps[slot].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    u->eps[slot].timeout = (uint32_t)timeout_ms;

    memset(&op, 0, sizeof op);
    op.max_bufsize = bufsize;
    op.max_frames = 1;
    op.ep_index = (uint8_t)slot;
    op.ep_no = (uint8_t)ep;
    return ioctl(u->fd, USB_FS_OPEN, &op) == 0;
}

unsigned char *ugen_slot_buf(ugen *u, int slot) { return u->data[slot]; }

int ugen_slot_pending(const ugen *u, int slot) { return u->pending[slot]; }

int ugen_fs_start(ugen *u, int slot, uint32_t len)
{
    struct usb_fs_start start;

    if (u->pending[slot]) return 1;
    if (len > u->cap[slot]) len = u->cap[slot];

    u->buflen[slot][0] = len;
    u->eps[slot].nFrames = 1;
    u->eps[slot].aFrames = 0;
    u->eps[slot].status = 0;

    memset(&start, 0, sizeof start);
    start.ep_index = (uint8_t)slot;
    if (ioctl(u->fd, USB_FS_START, &start) != 0) return 0;

    u->pending[slot] = 1;
    return 1;
}

int ugen_fs_complete(ugen *u, ugen_completion *c)
{
    struct usb_fs_complete comp;
    int slot;

    memset(&comp, 0, sizeof comp);
    if (ioctl(u->fd, USB_FS_COMPLETE, &comp) != 0) return 0;

    slot = (int)comp.ep_index;
    if (slot < 0 || slot >= u->nslots) return 0;

    u->pending[slot] = 0;
    c->slot = slot;
    c->status = (int)u->eps[slot].status;
    c->frames = (int)u->eps[slot].aFrames;
    c->len = u->buflen[slot][0];
    return 1;
}

int ugen_fs_wait(ugen *u, int slot, int timeout_ms, ugen_completion *c)
{
    int waited;

    for (waited = 0; waited <= timeout_ms; waited++) {
        while (ugen_fs_complete(u, c))
            if (c->slot == slot) return 1;
        usleep(1000);
    }
    return 0;
}

int ugen_fs_clear_stall(ugen *u, int slot)
{
    struct usb_fs_clear_stall_sync cs;

    memset(&cs, 0, sizeof cs);
    cs.ep_index = (uint8_t)slot;
    return ioctl(u->fd, USB_FS_CLEAR_STALL_SYNC, &cs) == 0;
}
