#include "dongle/mt76_fw.h"
#include "sha256.h"

#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW_DIR "/data/fgg-xsense/firmware"

/* No adapter's image comes anywhere near this; a larger file is not one. */
#define FW_MAX_SIZE (1024 * 1024)

/* The checksums dlundqvist/xone's install/firmware.sh pins for each adapter. */
static const struct {
    unsigned    pid;
    const char *sha256;
} g_known[] = {
    { 0x02E6, "080ce4091e53a4ef3e5fe29939f51fd91f46d6a88be6d67eb6e99a5723b3a223" },
    { 0x02FE, "48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66" },
    { 0x02F9, "0023a7bae02974834500c665a281e25b1ba52c9226c84989f9084fa5ce591d9b" },
    { 0x091E, "e2710daf81e7b36d35985348f68a81d18bc537a2b0c508ffdfde6ac3eae1bad7" },
};

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

void mt76_fw_path(unsigned pid, char *path, int pathlen)
{
    snprintf(path, (size_t)pathlen, FW_DIR "/dongle_%04x.bin", pid);
}

int mt76_fw_parse(const unsigned char *data, size_t size, mt76_fw *fw,
                  char *why, int whylen)
{
    int i;

    memset(fw, 0, sizeof *fw);

    if (size < MT76_FW_HEADER_SIZE) {
        snprintf(why, (size_t)whylen, "%zu bytes is too short for a header", size);
        return 0;
    }

    fw->ilm_len = le32(data + 0);
    fw->dlm_len = le32(data + 4);
    fw->build_version = le16(data + 8);
    fw->fw_version = le16(data + 10);
    memcpy(fw->build_time, data + 16, 16);
    fw->build_time[16] = '\0';
    for (i = 0; i < 16; i++)
        if (fw->build_time[i] && (fw->build_time[i] < 0x20 || fw->build_time[i] > 0x7E))
            fw->build_time[i] = '?';

    /* Compared without adding, so huge lengths cannot wrap round. */
    if (fw->ilm_len > size - MT76_FW_HEADER_SIZE ||
        fw->dlm_len > size - MT76_FW_HEADER_SIZE - fw->ilm_len) {
        snprintf(why, (size_t)whylen,
                 "header claims ILM %u + DLM %u bytes but the file has %zu",
                 fw->ilm_len, fw->dlm_len, size - MT76_FW_HEADER_SIZE);
        return 0;
    }
    if (fw->ilm_len == 0 || fw->dlm_len == 0) {
        snprintf(why, (size_t)whylen, "empty ILM or DLM image");
        return 0;
    }

    fw->ilm = data + MT76_FW_HEADER_SIZE;
    fw->dlm = fw->ilm + fw->ilm_len;
    fw->size = size;
    return 1;
}

int mt76_fw_load(unsigned pid, mt76_fw *fw, char *why, int whylen)
{
    char path[96], digest[2 * SHA256_SIZE + 1];
    const char *want = NULL;
    unsigned char *data;
    struct stat st;
    size_t i, got;
    FILE *f;

    memset(fw, 0, sizeof *fw);

    for (i = 0; i < sizeof g_known / sizeof g_known[0]; i++)
        if (g_known[i].pid == pid) want = g_known[i].sha256;
    if (!want) {
        snprintf(why, (size_t)whylen, "no known firmware for adapter %04X", pid);
        return 0;
    }

    mt76_fw_path(pid, path, (int)sizeof path);
    if (stat(path, &st) != 0) {
        snprintf(why, (size_t)whylen, "%s: %s", path, strerror(errno));
        return 0;
    }
    if (st.st_size <= 0 || st.st_size > FW_MAX_SIZE) {
        snprintf(why, (size_t)whylen, "%s: implausible size %lld", path,
                 (long long)st.st_size);
        return 0;
    }

    data = malloc((size_t)st.st_size);
    f = fopen(path, "rb");
    if (!data || !f) {
        snprintf(why, (size_t)whylen, "%s: %s", path, strerror(errno));
        free(data);
        if (f) fclose(f);
        return 0;
    }
    got = fread(data, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) {
        snprintf(why, (size_t)whylen, "%s: short read", path);
        free(data);
        return 0;
    }

    sha256_hex(data, got, digest);
    if (strcmp(digest, want) != 0) {
        snprintf(why, (size_t)whylen, "%s: checksum %.16s... is not the expected %.16s...",
                 path, digest, want);
        free(data);
        return 0;
    }

    if (!mt76_fw_parse(data, got, fw, why, whylen)) {
        free(data);
        return 0;
    }
    fw->data = data;
    return 1;
}

void mt76_fw_free(mt76_fw *fw)
{
    free(fw->data);
    memset(fw, 0, sizeof *fw);
}
