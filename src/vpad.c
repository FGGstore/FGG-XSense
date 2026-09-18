#include "vpad.h"
#include "gip.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The three ioctls libScePad issues on /dev/hid, with the command word each
 * expects in the first field of its request. */
#define HID_ADD_DEVICE  0xC018482AUL   /* _IOWR('H', 0x2A, 24) */
#define HID_INSERT_DATA 0x8018482CUL   /* _IOW ('H', 0x2C, 24) */
#define HID_DEL_DEVICE  0x80104850UL   /* _IOW ('H', 0x50, 16) */

#define HID_CMD_ADD 3
#define HID_CMD_DEL 5

/* Device descriptor passed to AddDevice. The protocol and class bytes are the
 * pair the library writes for a standard pad; the user id says who owns it. */
#define VDEV_DESC_SIZE 168
#define VDEV_PROTO_OFF 0x00
#define VDEV_USER_OFF  0x1C
#define VDEV_CLASS_OFF 0x20
#define VDEV_PROTO     3
#define VDEV_CLASS     1

/* Field offsets within the report the kernel reads. Every one of these is a
 * position libScePad copies a ScePadData field to, not a ScePadData offset. */
#define VP_BUTTONS 0x0C   /* uint32 */
#define VP_STICKS  0x10   /* lx, ly, rx, ry */
#define VP_L2      0x14
#define VP_R2      0x15
#define VP_ACTIVE  0x17   /* the library hardcodes 1 here */
#define VP_MOTION  0x78   /* eight floats */

/* PlayStation button bits. */
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

/* Trigger travel past which the digital bit is also reported. */
#define TRIGGER_DIGITAL 128

/* The 24-byte request all three ioctls take. */
typedef struct {
    uint32_t  cmd;
    uint32_t  reserved;
    void     *data;
    void     *out;
} hid_req;

/* The 16-byte request DeleteDevice takes instead. */
typedef struct {
    uint8_t  cmd;
    uint8_t  pad[7];
    uint32_t handle;
    uint32_t pad2;
} hid_del;

struct vpad {
    int     fd;
    int32_t handle;
};

static struct vpad g_pad;

vpad *vpad_open(uint32_t user, char *why, int whylen)
{
    unsigned char desc[VDEV_DESC_SIZE];
    struct vpad *pad = &g_pad;
    hid_req req;

    pad->fd = open("/dev/hid", O_RDWR);
    if (pad->fd < 0) {
        snprintf(why, (size_t)whylen, "/dev/hid: %s", strerror(errno));
        return NULL;
    }

    memset(desc, 0, sizeof desc);
    desc[VDEV_PROTO_OFF] = VDEV_PROTO;
    memcpy(desc + VDEV_USER_OFF, &user, sizeof user);
    desc[VDEV_CLASS_OFF] = VDEV_CLASS;

    pad->handle = -1;

    memset(&req, 0, sizeof req);
    req.cmd = HID_CMD_ADD;
    req.data = desc;
    req.out = &pad->handle;

    if (ioctl(pad->fd, HID_ADD_DEVICE, &req) != 0) {
        snprintf(why, (size_t)whylen, "AddDevice: %s", strerror(errno));
        close(pad->fd);
        pad->fd = -1;
        return NULL;
    }

    snprintf(why, (size_t)whylen, "handle %d", pad->handle);
    return pad;
}

int vpad_submit(vpad *pad, const unsigned char *report)
{
    hid_req req;

    memset(&req, 0, sizeof req);
    req.cmd = (uint32_t)pad->handle;
    req.data = (void *)(uintptr_t)report;
    req.out = NULL;

    return ioctl(pad->fd, HID_INSERT_DATA, &req) == 0;
}

void vpad_close(vpad *pad)
{
    hid_del del;

    if (!pad || pad->fd < 0) return;

    memset(&del, 0, sizeof del);
    del.cmd = HID_CMD_DEL;
    del.handle = (uint32_t)pad->handle;
    ioctl(pad->fd, HID_DEL_DEVICE, &del);

    close(pad->fd);
    pad->fd = -1;
}

int vpad_handle(const vpad *pad) { return pad->handle; }

/* ---- translation ------------------------------------------------------- */

static int16_t le16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* GIP sticks are signed 16-bit with Y increasing upwards; PlayStation sticks
 * are unsigned 8-bit with Y increasing downwards, so Y is inverted here rather
 * than left for every game to be confused by. */
static uint8_t axis(int16_t v)     { return (uint8_t)(((int)v + 32768) >> 8); }
static uint8_t axis_inv(int16_t v) { return (uint8_t)((32767 - (int)v) >> 8); }

void vpad_translate(const unsigned char *gip_report, unsigned char *report)
{
    const unsigned char *d = gip_report + 4;   /* past the message header */
    unsigned m  = (unsigned)d[0] | ((unsigned)d[1] << 8);
    unsigned lt = (unsigned)d[2] | ((unsigned)d[3] << 8);
    unsigned rt = (unsigned)d[4] | ((unsigned)d[5] << 8);
    uint32_t b = 0;
    float ident = 1.0f;

    memset(report, 0, VPAD_REPORT_SIZE);

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
    if (lt > TRIGGER_DIGITAL) b |= SCE_L2;
    if (rt > TRIGGER_DIGITAL) b |= SCE_R2;

    memcpy(report + VP_BUTTONS, &b, sizeof b);

    report[VP_STICKS + 0] = axis(le16(d + 6));
    report[VP_STICKS + 1] = axis_inv(le16(d + 8));
    report[VP_STICKS + 2] = axis(le16(d + 10));
    report[VP_STICKS + 3] = axis_inv(le16(d + 12));

    report[VP_L2] = (uint8_t)(lt >> 2);
    report[VP_R2] = (uint8_t)(rt >> 2);
    report[VP_ACTIVE] = 1;

    /* Identity quaternion, so the motion block is at least well formed. */
    memcpy(report + VP_MOTION + 12, &ident, sizeof ident);
}
