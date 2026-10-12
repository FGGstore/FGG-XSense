/* A controller's state, independent of how it reached the console.
 *
 * Every backend -- a wired pad, a pad on the wireless adapter, a Bluetooth
 * pad -- decodes its own reports into this, and only this is translated into
 * what the virtual pad publishes. Values follow the Xbox convention the wired
 * protocol uses, so the wired path needs no conversion at all: sticks are
 * signed with Y increasing upwards, triggers run 0..1023.
 */
#ifndef FGG_PAD_H
#define FGG_PAD_H

#include <stdint.h>

/* Buttons. The first fourteen share their values with the GIP input report. */
#define PAD_MENU   0x00000004
#define PAD_VIEW   0x00000008
#define PAD_A      0x00000010
#define PAD_B      0x00000020
#define PAD_X      0x00000040
#define PAD_Y      0x00000080
#define PAD_UP     0x00000100
#define PAD_DOWN   0x00000200
#define PAD_LEFT   0x00000400
#define PAD_RIGHT  0x00000800
#define PAD_LB     0x00001000
#define PAD_RB     0x00002000
#define PAD_LS     0x00004000
#define PAD_RS     0x00008000
#define PAD_GUIDE  0x00010000
#define PAD_SHARE  0x00020000

#define PAD_TRIGGER_MAX 1023

typedef struct {
    uint32_t buttons;
    int16_t  lx, ly, rx, ry;
    uint16_t lt, rt;
} pad_state;

#endif
