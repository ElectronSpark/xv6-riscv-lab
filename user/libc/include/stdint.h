#ifndef _XV6_STDINT_H
#define _XV6_STDINT_H

#include "kernel/inc/types.h"
#include "stddef.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int8   int8_t;
typedef int16  int16_t;
typedef int32  int32_t;
typedef int64  int64_t;
typedef uint8  uint8_t;
typedef uint16 uint16_t;
typedef uint32 uint32_t;
typedef uint64 uint64_t;

typedef int64  intptr_t;
typedef uint64 uintptr_t;
typedef int64  ssize_t;

#define INT8_MIN   (-128)
#define INT8_MAX   (127)
#define UINT8_MAX  (255U)
#define INT16_MIN  (-32768)
#define INT16_MAX  (32767)
#define UINT16_MAX (65535U)
#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  (2147483647)
#define UINT32_MAX (4294967295U)
#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  (9223372036854775807LL)
#define UINT64_MAX (18446744073709551615ULL)

#ifdef __cplusplus
}
#endif

#endif /* _XV6_STDINT_H */
