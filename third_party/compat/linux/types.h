/* The few Linux kernel types and macros that the vendored xone headers use,
 * for building them outside the kernel. Every mask they use fits 32 bits. */
#ifndef FGG_COMPAT_LINUX_TYPES_H
#define FGG_COMPAT_LINUX_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef uint16_t __le16;
typedef uint32_t __le32;

#define BIT(n)        (1u << (n))
#define GENMASK(h, l) ((~0u >> (31 - (h))) & (~0u << (l)))
#ifndef __packed
#define __packed      __attribute__((packed))
#endif

#define FIELD_PREP(mask, val) \
    ((((uint32_t)(val)) << __builtin_ctz(mask)) & (mask))
#define FIELD_GET(mask, reg) \
    ((((uint32_t)(reg)) & (mask)) >> __builtin_ctz(mask))

#endif
