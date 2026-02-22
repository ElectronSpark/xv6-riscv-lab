/*
 * stdint.h — Minimal C99 fixed-width integer types shim for lwIP
 *
 * The xv6 kernel uses its own type names (uint8, uint32, etc.) without
 * the C99 _t suffix.  Some lwIP application headers (notably mdns.h)
 * use uint32_t directly.  This shim bridges the gap.
 */
#ifndef __LWIP_COMPAT_STDINT_H
#define __LWIP_COMPAT_STDINT_H

#include "types.h"

typedef uint8   uint8_t;
typedef int8    int8_t;
typedef uint16  uint16_t;
typedef int16   int16_t;
typedef uint32  uint32_t;
typedef int32   int32_t;
typedef uint64  uint64_t;
typedef int64   int64_t;

typedef uint64  uintptr_t;
typedef int64   intptr_t;

#endif /* __LWIP_COMPAT_STDINT_H */
