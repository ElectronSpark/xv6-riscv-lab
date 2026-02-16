/**
 * @file inttypes.h
 * @brief Minimal inttypes.h shim for lwext4 in xv6 kernel space.
 *
 * Provides printf format macros for fixed-width integer types.
 * On RISC-V LP64: int=32b, long=64b, long long=64b.
 */
#ifndef _EXT4_COMPAT_INTTYPES_H
#define _EXT4_COMPAT_INTTYPES_H

#include <stdint.h>

/* 8-bit */
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"
#define PRId8   "d"

/* 16-bit */
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"
#define PRId16  "d"

/* 32-bit */
#define PRIu32  "u"
#define PRIx32  "x"
#define PRIX32  "X"
#define PRId32  "d"

/* 64-bit (unsigned long on LP64/RISC-V) */
#define PRIu64  "lu"
#define PRIx64  "lx"
#define PRIX64  "lX"
#define PRId64  "ld"

#endif /* _EXT4_COMPAT_INTTYPES_H */
