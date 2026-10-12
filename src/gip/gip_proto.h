/* The GIP wire protocol, independent of the transport carrying it.
 *
 * A wired pad delivers GIP messages straight from its USB endpoint; a pad on
 * the wireless adapter delivers the same messages inside radio frames. Both
 * build, acknowledge and decode them through this file, which touches no
 * device and so is also what the host tests exercise.
 */
#ifndef FGG_GIP_PROTO_H
#define FGG_GIP_PROTO_H

#include "pad.h"

/* Message types. Only the ones this program acts on are named. */
#define GIP_CMD_ACK              0x01
#define GIP_CMD_ANNOUNCE         0x02
#define GIP_CMD_STATUS           0x03
#define GIP_CMD_SET_DEVICE_STATE 0x05
#define GIP_CMD_AUTHENTICATE     0x06
#define GIP_CMD_GUIDE_BUTTON     0x07
#define GIP_CMD_LED              0x0A
#define GIP_LL_INPUT_REPORT      0x20

/* Options byte flags. SYSTEM marks a system message; ACK means the controller
 * wants this message acknowledged and will wait until it is. */
#define GIP_OPT_SYSTEM 0x20
#define GIP_OPT_ACK    0x10

#define GIP_HEADER_SIZE 4

/* An input report's payload: buttons, two triggers, four sticks. */
#define GIP_INPUT_MIN (GIP_HEADER_SIZE + 14)

/* One message of the initialisation sequence. */
typedef struct {
    unsigned char cmd;
    unsigned char len;
    unsigned char payload[3];
} gip_init_msg;

/* Power on, authenticate, light the guide LED.
 *
 * The authentication message is the one that matters for third-party pads --
 * Microsoft controllers do not need it, others power themselves down a few
 * seconds after connecting without it. Sent again as the keepalive. */
extern const gip_init_msg gip_init_seq[];
extern const int gip_init_count;

/* Writes a system message into `buf`. Returns its length, or -1 if it does
 * not fit. */
int gip_build(unsigned char *buf, int cap, unsigned cmd, unsigned seq,
              const unsigned char *payload, int plen);

/* Whether a received message asks to be acknowledged. The controller waits
 * for the acknowledgement and stops talking to a host that ignores it. */
int gip_wants_ack(const unsigned char *msg);

/* Builds the acknowledgement for `msg`. Returns its length, or -1. Follows
 * xone's gip_acknowledge_pkt. */
int gip_build_ack(unsigned char *buf, int cap, const unsigned char *msg);

/* Power modes for GIP_CMD_SET_DEVICE_STATE (xone enum gip_power_mode). */
#define GIP_POWER_ON  0x00
#define GIP_POWER_OFF 0x04

/* Builds the message that switches a controller off. Returns its length. */
int gip_build_power_off(unsigned char *buf, int cap, unsigned seq);

/* Decodes a controller status report's two-bit battery level and type. The
 * level is 0=low, 1=normal, 2=high, 3=full.  Type 0 is external power or
 * charging, 1 standard batteries, and 2 a rechargeable pack. */
int gip_decode_battery(const unsigned char *msg, int n, int *level, int *type);
const char *gip_battery_level_name(int level);
const char *gip_battery_type_name(int type);

/* Whether a reading means the batteries need attention: level low while
 * running on batteries, not while charging or on external power. */
int gip_battery_is_low(int level, int type);

/* Decodes an input report. Returns 1 and fills `out` if `msg` is one, 0 if
 * it is some other message or too short. */
int gip_decode_input(const unsigned char *msg, int n, pad_state *out);

#endif
