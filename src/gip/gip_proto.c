#include "gip/gip_proto.h"

#include <string.h>

/* Guide LED message payload: which LED, the pattern, and a brightness. */
#define GIP_LED_GUIDE    0x00
#define GIP_LED_GUIDE_ON 0x01
#define GIP_LED_LEVEL    0x14

/* The fourteen buttons a GIP input report carries, which pad.h numbers the
 * same way. Each was verified against hardware by pressing it alone and
 * reading the raw bytes, rather than taken from the Xbox layout on trust. */
#define GIP_BUTTON_MASK 0xFFFC

const gip_init_msg gip_init_seq[] = {
    { GIP_CMD_SET_DEVICE_STATE, 1, { 0x00 } },
    { GIP_CMD_AUTHENTICATE,     2, { 0x01, 0x00 } },
    { GIP_CMD_LED,              3, { GIP_LED_GUIDE, GIP_LED_GUIDE_ON, GIP_LED_LEVEL } },
};
const int gip_init_count = (int)(sizeof gip_init_seq / sizeof gip_init_seq[0]);

static int16_t le16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int gip_build(unsigned char *buf, int cap, unsigned cmd, unsigned seq,
              const unsigned char *payload, int plen)
{
    if (plen < 0 || plen > 127 || GIP_HEADER_SIZE + plen > cap) return -1;

    buf[0] = (unsigned char)cmd;
    buf[1] = GIP_OPT_SYSTEM;
    buf[2] = (unsigned char)seq;
    buf[3] = (unsigned char)plen;
    if (plen > 0) memcpy(buf + GIP_HEADER_SIZE, payload, (size_t)plen);
    return GIP_HEADER_SIZE + plen;
}

int gip_wants_ack(const unsigned char *msg)
{
    return (msg[1] & GIP_OPT_ACK) != 0;
}

int gip_build_ack(unsigned char *buf, int cap, const unsigned char *msg)
{
    unsigned char body[9];

    /* The payload carries the command and length of the message being
     * answered, and options of its own: the sender's device ID as an
     * internal message -- not the original options, whose acknowledge flag a
     * controller on the wireless adapter does not accept back (xone
     * gip_acknowledge_pkt). The header echoes the sequence number. */
    memset(body, 0, sizeof body);
    body[1] = msg[0];
    body[2] = (unsigned char)(GIP_OPT_SYSTEM | (msg[1] & 0x0F));
    body[3] = msg[3];

    return gip_build(buf, cap, GIP_CMD_ACK, msg[2], body, (int)sizeof body);
}

int gip_build_power_off(unsigned char *buf, int cap, unsigned seq)
{
    static const unsigned char off[] = { GIP_POWER_OFF };
    return gip_build(buf, cap, GIP_CMD_SET_DEVICE_STATE, seq, off, (int)sizeof off);
}

int gip_decode_battery(const unsigned char *msg, int n, int *level, int *type)
{
    unsigned char status;

    if (n < GIP_HEADER_SIZE + 1 || msg[0] != GIP_CMD_STATUS) return 0;
    status = msg[GIP_HEADER_SIZE];
    *level = status & 0x03;
    *type = (status >> 2) & 0x03;
    return 1;
}

const char *gip_battery_level_name(int level)
{
    static const char *const names[] = { "low", "normal", "high", "full" };
    return level >= 0 && level < 4 ? names[level] : "unknown";
}

const char *gip_battery_type_name(int type)
{
    static const char *const names[] = {
        "charging or external power", "standard battery", "rechargeable pack",
        "unknown power source",
    };
    return type >= 0 && type < 4 ? names[type] : "unknown power source";
}

int gip_battery_is_low(int level, int type)
{
    return level == 0 && (type == 1 || type == 2);
}

int gip_decode_input(const unsigned char *msg, int n, pad_state *out)
{
    const unsigned char *d = msg + GIP_HEADER_SIZE;

    if (n < GIP_INPUT_MIN || msg[0] != GIP_LL_INPUT_REPORT) return 0;

    out->buttons = ((uint32_t)d[0] | ((uint32_t)d[1] << 8)) & GIP_BUTTON_MASK;
    out->lt = (uint16_t)((unsigned)d[2] | ((unsigned)d[3] << 8));
    out->rt = (uint16_t)((unsigned)d[4] | ((unsigned)d[5] << 8));
    out->lx = le16(d + 6);
    out->ly = le16(d + 8);
    out->rx = le16(d + 10);
    out->ry = le16(d + 12);
    return 1;
}
