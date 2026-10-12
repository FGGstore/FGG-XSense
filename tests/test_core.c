/* Host tests for the parts that touch no device.
 *
 * The central one pins the refactor down: the wired path now decodes a GIP
 * report into a pad_state and translates that, where it used to translate the
 * raw report directly. The kernel report must come out byte for byte the same,
 * so the old translation is kept here verbatim as the reference.
 */
#include "dongle/mt76_fw.h"
#include "gip/gip_proto.h"
#include "sha256.h"
#include "vpad.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        g_failures++;                                           \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);    \
        fprintf(stderr, __VA_ARGS__);                           \
        fprintf(stderr, "\n");                                  \
    }                                                           \
} while (0)

/* ---- the translation as it was before the refactor (vpad.c, resident) -- */

#define VP_BUTTONS 0x0C
#define VP_STICKS  0x10
#define VP_L2      0x14
#define VP_R2      0x15
#define VP_ACTIVE  0x17
#define VP_MOTION  0x78
#define TRIGGER_DIGITAL 128

static int16_t ref_le16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint8_t ref_axis(int16_t v)     { return (uint8_t)(((int)v + 32768) >> 8); }
static uint8_t ref_axis_inv(int16_t v) { return (uint8_t)((32767 - (int)v) >> 8); }

static void ref_translate(const unsigned char *gip_report, unsigned char *report)
{
    const unsigned char *d = gip_report + 4;
    unsigned m  = (unsigned)d[0] | ((unsigned)d[1] << 8);
    unsigned lt = (unsigned)d[2] | ((unsigned)d[3] << 8);
    unsigned rt = (unsigned)d[4] | ((unsigned)d[5] << 8);
    uint32_t b = 0;
    float ident = 1.0f;

    memset(report, 0, VPAD_REPORT_SIZE);

    if (m & 0x0010) b |= 0x004000;   /* A     -> cross    */
    if (m & 0x0020) b |= 0x002000;   /* B     -> circle   */
    if (m & 0x0040) b |= 0x008000;   /* X     -> square   */
    if (m & 0x0080) b |= 0x001000;   /* Y     -> triangle */
    if (m & 0x0100) b |= 0x000010;   /* up    */
    if (m & 0x0200) b |= 0x000040;   /* down  */
    if (m & 0x0400) b |= 0x000080;   /* left  */
    if (m & 0x0800) b |= 0x000020;   /* right */
    if (m & 0x1000) b |= 0x000400;   /* LB    -> L1 */
    if (m & 0x2000) b |= 0x000800;   /* RB    -> R1 */
    if (m & 0x4000) b |= 0x000002;   /* LS    -> L3 */
    if (m & 0x8000) b |= 0x000004;   /* RS    -> R3 */
    if (m & 0x0004) b |= 0x000008;   /* menu  -> options  */
    if (m & 0x0008) b |= 0x100000;   /* view  -> touchpad */

    if (lt > TRIGGER_DIGITAL) b |= 0x000100;
    if (rt > TRIGGER_DIGITAL) b |= 0x000200;

    memcpy(report + VP_BUTTONS, &b, sizeof b);

    report[VP_STICKS + 0] = ref_axis(ref_le16(d + 6));
    report[VP_STICKS + 1] = ref_axis_inv(ref_le16(d + 8));
    report[VP_STICKS + 2] = ref_axis(ref_le16(d + 10));
    report[VP_STICKS + 3] = ref_axis_inv(ref_le16(d + 12));

    report[VP_L2] = (uint8_t)(lt >> 2);
    report[VP_R2] = (uint8_t)(rt >> 2);
    report[VP_ACTIVE] = 1;

    memcpy(report + VP_MOTION + 12, &ident, sizeof ident);
}

/* ---- tests ------------------------------------------------------------- */

static void make_report(unsigned char *r, uint16_t buttons, uint16_t lt,
                        uint16_t rt, int16_t lx, int16_t ly, int16_t rx,
                        int16_t ry)
{
    uint16_t v[7];
    int i;

    v[0] = buttons; v[1] = lt; v[2] = rt;
    v[3] = (uint16_t)lx; v[4] = (uint16_t)ly;
    v[5] = (uint16_t)rx; v[6] = (uint16_t)ry;

    r[0] = GIP_LL_INPUT_REPORT;
    r[1] = 0x00;
    r[2] = 0x2A;
    r[3] = 14;
    for (i = 0; i < 7; i++) {
        r[4 + 2 * i] = (unsigned char)(v[i] & 0xFF);
        r[5 + 2 * i] = (unsigned char)(v[i] >> 8);
    }
}

static void check_same(const unsigned char *gip, const char *what)
{
    unsigned char want[VPAD_REPORT_SIZE], got[VPAD_REPORT_SIZE];
    pad_state st;

    ref_translate(gip, want);
    CHECK(gip_decode_input(gip, GIP_INPUT_MIN, &st), "%s: not decoded", what);
    vpad_translate(&st, got);
    CHECK(memcmp(want, got, sizeof want) == 0, "%s: report differs", what);
}

static void test_translation_unchanged(void)
{
    unsigned char r[GIP_INPUT_MIN];
    int i;

    /* Edges: everything released, everything pressed, the extremes of every
     * axis, and the triggers either side of the digital threshold. */
    make_report(r, 0, 0, 0, 0, 0, 0, 0);
    check_same(r, "idle");
    make_report(r, 0xFFFF, 1023, 1023, 32767, 32767, 32767, 32767);
    check_same(r, "all max");
    make_report(r, 0xFFFF, 0, 0, -32768, -32768, -32768, -32768);
    check_same(r, "all min");
    make_report(r, 0, 128, 129, 0, 0, 0, 0);
    check_same(r, "trigger threshold");

    srand(1);
    for (i = 0; i < 100000; i++) {
        make_report(r, (uint16_t)rand(), (uint16_t)(rand() % 1024),
                    (uint16_t)(rand() % 1024), (int16_t)rand(), (int16_t)rand(),
                    (int16_t)rand(), (int16_t)rand());
        check_same(r, "random");
        if (g_failures) break;
    }
}

static void test_decode_rejects(void)
{
    unsigned char r[GIP_INPUT_MIN];
    pad_state st;

    make_report(r, 0, 0, 0, 0, 0, 0, 0);
    CHECK(!gip_decode_input(r, GIP_INPUT_MIN - 1, &st), "short report accepted");
    r[0] = GIP_CMD_GUIDE_BUTTON;
    CHECK(!gip_decode_input(r, GIP_INPUT_MIN, &st), "non-input message accepted");
}

static void test_ack(void)
{
    /* A status message (cmd 03) with the ACK option, sequence 7, length 4. */
    const unsigned char msg[] = { 0x03, 0x30, 0x07, 0x04, 1, 2, 3, 4 };
    const unsigned char want[] = { 0x01, 0x20, 0x07, 0x09,
                                   0x00, 0x03, 0x20, 0x04, 0, 0, 0, 0, 0 };
    unsigned char got[32];
    int n;

    CHECK(gip_wants_ack(msg), "ACK option not seen");
    n = gip_build_ack(got, (int)sizeof got, msg);
    CHECK(n == (int)sizeof want && memcmp(got, want, sizeof want) == 0,
          "acknowledgement bytes differ (len %d)", n);
}

static void test_power_off(void)
{
    unsigned char buf[16];
    int n = gip_build_power_off(buf, (int)sizeof buf, 9);

    CHECK(n == 5 && buf[0] == 0x05 && buf[1] == 0x20 && buf[2] == 9 &&
          buf[3] == 1 && buf[4] == 0x04, "power-off message wrong");
}

static void test_battery(void)
{
    const unsigned char status[] = { GIP_CMD_STATUS, 0x20, 3, 1, 0x09 };
    int level = -1, type = -1;

    CHECK(gip_decode_battery(status, (int)sizeof status, &level, &type),
          "battery status not decoded");
    CHECK(level == 1 && type == 2, "battery status = level %d type %d", level, type);
    CHECK(strcmp(gip_battery_level_name(level), "normal") == 0,
          "battery level label wrong");
    CHECK(strcmp(gip_battery_type_name(type), "rechargeable pack") == 0,
          "battery type label wrong");
    CHECK(!gip_decode_battery(status, GIP_HEADER_SIZE, &level, &type),
          "short status accepted");
    CHECK(gip_battery_is_low(0, 1) && gip_battery_is_low(0, 2),
          "low batteries not reported");
    CHECK(!gip_battery_is_low(0, 0), "low while charging reported");
    CHECK(!gip_battery_is_low(1, 1), "normal level reported as low");
    CHECK(!gip_battery_is_low(-1, -1), "no reading reported as low");
}

static void test_init_sequence(void)
{
    /* Power on, authenticate, light the guide LED -- as gip_init sent them. */
    unsigned char buf[16];
    int n;

    CHECK(gip_init_count == 3, "init sequence has %d messages", gip_init_count);
    n = gip_build(buf, (int)sizeof buf, gip_init_seq[0].cmd, 0,
                  gip_init_seq[0].payload, gip_init_seq[0].len);
    CHECK(n == 5 && buf[0] == 0x05 && buf[1] == 0x20 && buf[3] == 1 && buf[4] == 0,
          "power-on message wrong");
    n = gip_build(buf, (int)sizeof buf, gip_init_seq[2].cmd, 2,
                  gip_init_seq[2].payload, gip_init_seq[2].len);
    CHECK(n == 7 && buf[0] == 0x0A && buf[2] == 2 && buf[4] == 0x00 &&
          buf[5] == 0x01 && buf[6] == 0x14, "LED message wrong");
}

static void test_sha256(void)
{
    /* FIPS 180-4 example vectors. */
    static const char *msg2 =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char hex[2 * SHA256_SIZE + 1];
    unsigned char million[1001];
    unsigned char d[SHA256_SIZE];
    sha256_ctx c;
    int i;

    sha256_hex("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "sha256(\"\") = %s", hex);
    sha256_hex("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256(abc) = %s", hex);
    sha256_hex(msg2, strlen(msg2), hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
          "sha256(448-bit message) = %s", hex);

    /* A million 'a's, fed in uneven pieces to exercise the block buffer. */
    memset(million, 'a', sizeof million);
    sha256_init(&c);
    for (i = 0; i < 1000; i++) sha256_update(&c, million, (size_t)(i % 2 ? 999 : 1001));
    sha256_final(&c, d);
    for (i = 0; i < SHA256_SIZE; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
          "sha256(million a) = %s", hex);
}

static void test_fw_parse(void)
{
    unsigned char img[MT76_FW_HEADER_SIZE + 10];
    char why[160];
    mt76_fw fw;

    memset(img, 0, sizeof img);
    img[0] = 6;                    /* ILM 6 bytes */
    img[4] = 4;                    /* DLM 4 bytes */
    img[8] = 0x34; img[9] = 0x12;  /* build version */
    img[10] = 0x01; img[11] = 0x02;
    memcpy(img + 16, "201703101234____", 16);

    CHECK(mt76_fw_parse(img, sizeof img, &fw, why, (int)sizeof why),
          "valid image refused: %s", why);
    CHECK(fw.ilm_len == 6 && fw.dlm_len == 4 && fw.build_version == 0x1234 &&
          fw.fw_version == 0x0201, "header fields wrong");
    CHECK(fw.ilm == img + 32 && fw.dlm == img + 38, "image offsets wrong");
    CHECK(strcmp(fw.build_time, "201703101234____") == 0, "build time wrong");

    img[0] = 7;   /* one byte more than the file holds */
    CHECK(!mt76_fw_parse(img, sizeof img, &fw, why, (int)sizeof why),
          "overlong ILM accepted");
    img[0] = 0xFF; img[1] = 0xFF; img[2] = 0xFF; img[3] = 0xFF;
    CHECK(!mt76_fw_parse(img, sizeof img, &fw, why, (int)sizeof why),
          "wrapping ILM length accepted");
    CHECK(!mt76_fw_parse(img, 10, &fw, why, (int)sizeof why),
          "headerless file accepted");
}

int main(void)
{
    test_translation_unchanged();
    test_decode_rejects();
    test_ack();
    test_power_off();
    test_battery();
    test_init_sequence();
    test_sha256();
    test_fw_parse();

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
