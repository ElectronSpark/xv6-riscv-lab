#ifndef KERNEL_INC_BITS_H
#define KERNEL_INC_BITS_H

#include "types.h"

/***
 * Bit manipulation utilities
 * 
 * The following definitions are from GCC built-in functions:
 * 
 * ffs: Returns one plus the index of the least significant 1-bit of x, or if x is zero, returns zero.
 * clz: Returns the number of leading 0-bits in x, starting at the most significant bit position. If x is 0, the result is undefined.
 * ctz: Returns the number of trailing 0-bits in x, starting at the least significant bit position. If x is 0, the result is undefined.
 * popcount: Returns the number of 1-bits in x.
 */

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__) || !(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
// I only plan to support little-endian architectures for now
    #error "This code only supports little-endian architectures."
#endif

#if defined(WITH_GNU_BUILTIN_BITS_OPS) && !defined(USE_SOFTWARE_FFS)
// Use compiler builtins
    #define bits_ffs8(x)  __builtin_ffs((uint8)(x))
    #define bits_clz8(x)  ((x) ? (__builtin_clz((uint8)(x))) - 24 : -1)
    #define bits_ctz8(x)  ((x) ? (__builtin_ctz((uint8)(x))) : -1)
    #define bits_popcount8(x)  __builtin_popcount((uint8)(x))
    #define bits_ffs16(x)  __builtin_ffs((uint16)(x))
    #define bits_clz16(x)  ((x) ? (__builtin_clz((uint16)(x))) - 16 : -1)
    #define bits_ctz16(x)  ((x) ? (__builtin_ctz((uint16)(x))) : -1)
    #define bits_popcount16(x)  __builtin_popcount((uint16)(x))
    #define bits_ffs32(x)  __builtin_ffs((uint32)(x))
    #define bits_clz32(x)  ((x) ? (__builtin_clz((uint32)(x))) : -1)
    #define bits_ctz32(x)  ((x) ? (__builtin_ctz((uint32)(x))) : -1)
    #define bits_popcount32(x)  __builtin_popcount((uint32)(x))
    #define bits_ffs64(x)  __builtin_ffsll((uint64)(x))
    #define bits_clz64(x)  ((x) ? (__builtin_clzll((uint64)(x))) : -1)
    #define bits_ctz64(x)  ((x) ? (__builtin_ctzll((uint64)(x))) : -1)
    #define bits_popcount64(x)  __builtin_popcountll((uint64)(x))
    // @TODO: Generic versions
#elif USE_SOFTWARE_FFS
// Use predefined tables
    extern const int8 __uint8_bits_count[256];
    extern const int8 __uint8_trailing_zeros[256];
    extern const int8 __uint8_leading_zeros[256];
    static inline int __bits_ffs8(uint8 x) {
        return __uint8_trailing_zeros[x] + 1;
    }
    static inline int __bits_clz8(uint8 x) {
        return __uint8_leading_zeros[x];
    }
    static inline int __bits_ctz8(uint8 x) {
        return __uint8_trailing_zeros[x];
    }
    static inline int __bits_popcount8(uint8 x) {
        return __uint8_bits_count[x];
    }
    #define bits_ffs8(x)  __bits_ffs8(x)
    #define bits_clz8(x)  __bits_clz8(x)
    #define bits_ctz8(x)  __bits_ctz8(x)
    #define bits_popcount8(x)  __bits_popcount8(x)

    #define bits_ctz_x(x, width)    ({              \
        int res = -1;                               \
        int rem = width;                            \
        if (x) {                                    \
            typeof(x) __tmp = (x);                  \
            res = 0;                                \
            while (!(__tmp & 0xFF) && rem > 0) {    \
                __tmp >>= 8;                        \
                res += 8;                           \
                rem -= 8;                           \
            }                                       \
            res += bits_ctz8((__tmp & 0xFF));       \
        }                                           \
        res;                                        \
    })

    #define bits_clz_x(x, width)    ({              \
        int res = -1;                               \
        if (x) {                                    \
            int shift = width * 8 - 8;              \
            while(shift >= 0) {                     \
                uint8 shifted = ((x) >> shift)      \
                shifted &= 0xFF;                    \
                if (shifted) {                      \
                    ret = bits_clz8(shifted);       \
                    break;                          \
                }                                   \
                shift -= 8;                         \
            }                                       \
            ret += width - 8 - shift;               \
        }                                           \
        res;                                        \
    })

    #define bits_ffs_x(x, width)    (bits_ctz_x((x), (width)) + 1)

    #define bits_popcount_x(x, width)    ({         \
        int total = 0;                              \
        typeof(x) __tmp = (x);                      \
        int rem = width;                            \
        while (__tmp && rem > 0) {                  \
            uint8 byte = __tmp & 0xFF;              \
            total += bits_popcount8(byte);          \
            __tmp >>= 8;                            \
            rem -= 8;                               \
        }                                           \
        total;                                      \
    })

    #define bits_ffsg(x)      bits_ffs_x((uint64)(x), 8 * sizeof(x))
    #define bits_clzg(x)      bits_clz_x((uint64)(x), 8 * sizeof(x))
    #define bits_ctzg(x)      bits_ctz_x((uint64)(x), 8 * sizeof(x))
    #define bits_popcountg(x) bits_popcount_x((uint64)(x), 8 * sizeof(x))
    #define bits_ffs16(x)     bits_ffs_x((uint16)(x), 16)
    #define bits_clz16(x)     bits_clz_x((uint16)(x), 16)
    #define bits_ctz16(x)     bits_ctz_x((uint16)(x), 16)
    #define bits_popcount16(x) bits_popcount_x((uint16)(x), 16)
    #define bits_ffs32(x)     bits_ffs_x((uint32)(x), 32)
    #define bits_clz32(x)     bits_clz_x((uint32)(x), 32)
    #define bits_ctz32(x)     bits_ctz_x((uint32)(x), 32)
    #define bits_popcount32(x) bits_popcount_x((uint32)(x), 32)
    #define bits_ffs64(x)     bits_ffs_x((uint64)(x), 64)
    #define bits_clz64(x)     bits_clz_x((uint64)(x), 64)
    #define bits_ctz64(x)     bits_ctz_x((uint64)(x), 64)
    #define bits_popcount64(x) bits_popcount_x((uint64)(x), 64)
#else
// fallback implementations using iterative binary search
// @TODO: untested
    static const int __bits_binary_steps[] = {32, 16, 8, 4, 2, 1};

    static inline int bits_ctz_x(uint64 x, int width) {
        if (width <= 0) {
            return -1;
        }
        int bits = (width < 64) ? width : 64;
        if (bits < 64) {
            uint64 mask = (1ULL << bits) - 1ULL;
            x &= mask;
        }
        if (x == 0) {
            return -1;
        }
        int result = 0;
        int remaining = bits;
        for (size_t i = 0; i < sizeof(__bits_binary_steps) / sizeof(__bits_binary_steps[0]); i++) {
            int shift = __bits_binary_steps[i];
            if (shift > remaining) {
                continue;
            }
            uint64 mask = (shift == 64) ? ~0ULL : ((1ULL << shift) - 1ULL);
            if ((x & mask) == 0ULL) {
                x >>= shift;
                result += shift;
                remaining -= shift;
                if (x == 0) {
                    break;
                }
            }
        }
        return result;
    }

    static inline int bits_ffs_x(uint64 x, int width) {
        int idx = bits_ctz_x(x, width);
        return (idx < 0) ? 0 : (idx + 1);
    }

    static inline int bits_clz_x(uint64 x, int width) {
        if (width <= 0) {
            return -1;
        }
        int bits = (width < 64) ? width : 64;
        if (bits < 64) {
            uint64 mask = (1ULL << bits) - 1ULL;
            x &= mask;
        }
        if (x == 0) {
            return -1;
        }
        int padding = (bits < 64) ? (64 - bits) : 0;
        if (padding > 0) {
            x <<= padding;
        }
        int result = 0;
        int remaining = bits;
        for (size_t i = 0; i < sizeof(__bits_binary_steps) / sizeof(__bits_binary_steps[0]); i++) {
            int shift = __bits_binary_steps[i];
            if (shift > remaining - 1) {
                continue;
            }
            uint64 mask = ~0ULL << (64 - shift);
            if ((x & mask) == 0ULL) {
                result += shift;
                x <<= shift;
                remaining -= shift;
            }
        }
        return result;
    }

    static inline int bits_popcount_x(uint64 x, int width) {
        int count = 0;
        while (x && width) {
            count += x & 1;
            x >>= 1;
            width--;
        }
        return count;
    }

    #define bits_ffs8(x)      bits_ffs_x((uint8)(x), 8)
    #define bits_clz8(x)      bits_clz_x((uint8)(x), 8)
    #define bits_ctz8(x)      bits_ctz_x((uint8)(x), 8)
    #define bits_popcount8(x) bits_popcount_x((uint8)(x), 8)
    #define bits_ffs16(x)      bits_ffs_x((uint16)(x), 16)
    #define bits_clz16(x)      bits_clz_x((uint16)(x), 16)
    #define bits_ctz16(x)      bits_ctz_x((uint16)(x), 16)
    #define bits_popcount16(x) bits_popcount_x((uint16)(x), 16)
    #define bits_ffs32(x)      bits_ffs_x((uint32)(x), 32)
    #define bits_clz32(x)      bits_clz_x((uint32)(x), 32)
    #define bits_ctz32(x)      bits_ctz_x((uint32)(x), 32)
    #define bits_popcount32(x) bits_popcount_x((uint32)(x), 32)
    #define bits_ffs64(x)      bits_ffs_x((uint64)(x), 64)
    #define bits_clz64(x)      bits_clz_x((uint64)(x), 64)
    #define bits_ctz64(x)      bits_ctz_x((uint64)(x), 64)
    #define bits_popcount64(x) bits_popcount_x((uint64)(x), 64)
    #define bits_ffsg(x)      bits_ffs_x((uint64)(x), 8 * sizeof(x))
    #define bits_clzg(x)      bits_clz_x((uint64)(x), 8 * sizeof(x))
    #define bits_ctzg(x)      bits_ctz_x((uint64)(x), 8 * sizeof(x))
    #define bits_popcountg(x) bits_popcount_x((uint64)(x), 8 * sizeof(x))
#endif

static inline int64 bits_ctz_ptr(const void *ptr, size_t limit, bool inv) {
    // if (limit >= (1ULL << (sizeof(size_t)*8 - 16))) {
    //     return -1;
    // }
    if (ptr == NULL) {
        return -1;
    }
    const uint8 *byte_ptr = (const uint8 *)ptr;
    for (int64 index = 0; index < (int64)limit; index++) {
        uint8 byte = byte_ptr[index];
        if (inv) {
            byte = ~byte;
        }
        if (byte) {
            return (index << 3) | bits_ctz8(byte);
        }
    }
}

#endif // KERNEL_INC_BITS_H
