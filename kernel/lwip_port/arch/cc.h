/**
 * @file cc.h
 * @brief lwIP compiler/platform abstraction for xv6 kernel
 *
 * This file is included by lwip/arch.h and provides platform-specific
 * type definitions, diagnostics, assertions, and struct packing macros.
 *
 * Since we are in kernel space, NO standard C library headers (newlib)
 * are used. All types and helpers are provided by the xv6 kernel.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

/* xv6 kernel headers for types and printf */
#include "types.h"
#include "printf.h"
#include "string.h"

/* C99 fixed-width types — some lwIP apps (mdns.h) use uint32_t directly */
#include "stdint.h"

/* --------------------------------------------------------------------------
 * Block ALL standard library header includes — we are in kernel space.
 * The kernel provides its own types, string functions, etc.
 * -------------------------------------------------------------------------- */
#define LWIP_NO_STDDEF_H   1
#define LWIP_NO_STDINT_H   1
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_LIMITS_H   1
#define LWIP_NO_UNISTD_H   1
#define LWIP_NO_CTYPE_H    1

/* --------------------------------------------------------------------------
 * Provide the types that lwIP needs (normally from stdint.h / stddef.h).
 * These map directly to xv6's types.h definitions.
 * -------------------------------------------------------------------------- */
typedef uint8   u8_t;
typedef int8    s8_t;
typedef uint16  u16_t;
typedef int16   s16_t;
typedef uint32  u32_t;
typedef int32   s32_t;
typedef uint64  u64_t;
typedef int64   s64_t;
typedef uint64  mem_ptr_t;

/* ptrdiff_t — needed by LWIP_CONST_CAST in arch.h */
#ifndef __PTRDIFF_TYPE__
typedef long    ptrdiff_t;
#else
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#endif

#define LWIP_HAVE_INT64 1

/* Provide format strings (normally from inttypes.h) */
#define X8_F   "02x"
#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define SZT_F  "lu"

/* Limits (normally from limits.h) */
#define INT_MAX   0x7FFFFFFF
#define UINT_MAX  0xFFFFFFFFU
#define SSIZE_MAX 0x7FFFFFFFFFFFFFFFL

/* Byte order: RISC-V is little-endian */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* --------------------------------------------------------------------------
 * Struct packing (GCC) 
 * -------------------------------------------------------------------------- */
#define PACK_STRUCT_FIELD(x)    x
#define PACK_STRUCT_STRUCT      __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_FLD_8(x)    x
#define PACK_STRUCT_FLD_S(x)    x

/* --------------------------------------------------------------------------
 * Platform diagnostics and assertions — use xv6's printf/panic
 * -------------------------------------------------------------------------- */
#define LWIP_PLATFORM_DIAG(x)   do { printf x ; } while(0)

#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("lwIP ASSERT: %s at %s:%d\n", (x), __FILE__, __LINE__); \
    panic("lwIP assertion failed"); \
} while(0)

/* --------------------------------------------------------------------------
 * Random number generator
 * -------------------------------------------------------------------------- */
uint32 lwip_xv6_rand(void);
#define LWIP_RAND() ((u32_t)lwip_xv6_rand())

/* --------------------------------------------------------------------------
 * Alignment
 * -------------------------------------------------------------------------- */
#ifndef LWIP_DECLARE_MEMORY_ALIGNED
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
    u8_t variable_name[size] __attribute__((aligned(4)))
#endif

#endif /* LWIP_ARCH_CC_H */
