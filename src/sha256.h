/* SHA-256, for checking the adapter firmware before it is ever uploaded. */
#ifndef FGG_SHA256_H
#define FGG_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_SIZE 32

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char block[64];
    size_t used;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, unsigned char out[SHA256_SIZE]);

/* One-shot digest, written as 64 lowercase hex characters plus a NUL. */
void sha256_hex(const void *data, size_t n, char out[2 * SHA256_SIZE + 1]);

#endif
