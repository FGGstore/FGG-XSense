/* The Xbox Wireless Adapter's firmware image.
 *
 * The adapter is a MediaTek MT76 radio that boots firmware the host uploads
 * on every power-up. The image is Microsoft's and cannot be shipped, so the
 * user fetches it (tools/fetch_firmware.sh) to a per-adapter path on the
 * console. Before a single byte of it reaches the adapter it must match the
 * checksum the Linux drivers pin for that adapter and parse cleanly: an image
 * that fails either is refused outright, never partially uploaded.
 *
 * Layout (xow dongle/mt76.h, FwHeader): a 32-byte header, then the
 * instruction memory (ILM) image, then the data memory (DLM) image.
 */
#ifndef FGG_MT76_FW_H
#define FGG_MT76_FW_H

#include <stddef.h>
#include <stdint.h>

#define MT76_FW_HEADER_SIZE 32

typedef struct {
    unsigned char       *data;   /* the whole file */
    size_t               size;
    uint32_t             ilm_len, dlm_len;
    uint16_t             build_version, fw_version;
    char                 build_time[17];
    const unsigned char *ilm, *dlm;
} mt76_fw;

/* Where the firmware for adapter `pid` is expected. */
void mt76_fw_path(unsigned pid, char *path, int pathlen);

/* Parses an image already in memory. Returns 0 with a reason in `why` if the
 * lengths do not add up. Does not take ownership of `data`. */
int mt76_fw_parse(const unsigned char *data, size_t size, mt76_fw *fw,
                  char *why, int whylen);

/* Reads, checks and parses the firmware for adapter `pid`. Returns 0 with a
 * reason in `why` on any problem, in which case nothing needs freeing. */
int mt76_fw_load(unsigned pid, mt76_fw *fw, char *why, int whylen);

void mt76_fw_free(mt76_fw *fw);

#endif
