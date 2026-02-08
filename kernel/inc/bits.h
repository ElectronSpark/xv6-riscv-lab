#ifndef KERNEL_INC_BITS_H
#define KERNEL_INC_BITS_H

#include "types.h"

/***
 * Bit manipulation utilities
 *
 * The following definitions are from GCC built-in functions:
 *
 * ffs: Returns one plus the index of the least significant 1-bit of x, or if x
 * is zero, returns zero. clz: Returns the number of leading 0-bits in x,
 * starting at the most significant bit position. If x is 0, the result is
 * undefined. ctz: Returns the number of trailing 0-bits in x, starting at the
 * least significant bit position. If x is 0, the result is undefined. popcount:
 * Returns the number of 1-bits in x.
 */

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__) ||           \
    !(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
// I only plan to support little-endian architectures for now
#error "This code only supports little-endian architectures."
#endif

#if defined(WITH_GNU_BUILTIN_BITS_OPS) && !defined(USE_SOFTWARE_FFS)
// Use compiler builtins
#define bits_ffs8(x) __builtin_ffs((uint8)(x))
#define bits_clz8(x) ((x) ? (__builtin_clz((uint8)(x))) - 24 : -1)
#define bits_ctz8(x) ((x) ? (__builtin_ctz((uint8)(x))) : -1)
#define bits_popcount8(x) __builtin_popcount((uint8)(x))
#define bits_ffs16(x) __builtin_ffs((uint16)(x))
#define bits_clz16(x) ((x) ? (__builtin_clz((uint16)(x))) - 16 : -1)
#define bits_ctz16(x) ((x) ? (__builtin_ctz((uint16)(x))) : -1)
#define bits_popcount16(x) __builtin_popcount((uint16)(x))
#define bits_ffs32(x) __builtin_ffs((uint32)(x))
#define bits_clz32(x) ((x) ? (__builtin_clz((uint32)(x))) : -1)
#define bits_ctz32(x) ((x) ? (__builtin_ctz((uint32)(x))) : -1)
#define bits_popcount32(x) __builtin_popcount((uint32)(x))
#define bits_ffs64(x) __builtin_ffsll((uint64)(x))
#define bits_clz64(x) ((x) ? (__builtin_clzll((uint64)(x))) : -1)
#define bits_ctz64(x) ((x) ? (__builtin_ctzll((uint64)(x))) : -1)
#define bits_popcount64(x) __builtin_popcountll((uint64)(x))
// @TODO: Generic versions
#elif USE_SOFTWARE_FFS
// Use predefined tables
extern const int8 __uint8_bits_count[256];
extern const int8 __uint8_trailing_zeros[256];
extern const int8 __uint8_leading_zeros[256];
static inline int __bits_ffs8(uint8 x) { return __uint8_trailing_zeros[x] + 1; }
static inline int __bits_clz8(uint8 x) { return __uint8_leading_zeros[x]; }
static inline int __bits_ctz8(uint8 x) { return __uint8_trailing_zeros[x]; }
static inline int __bits_popcount8(uint8 x) { return __uint8_bits_count[x]; }
#define bits_ffs8(x) __bits_ffs8(x)
#define bits_clz8(x) __bits_clz8(x)
#define bits_ctz8(x) __bits_ctz8(x)
#define bits_popcount8(x) __bits_popcount8(x)

#define bits_ctz_x(x, width)                                                   \
    ({                                                                         \
        int res = -1;                                                          \
        int rem = width;                                                       \
        if (x) {                                                               \
            typeof(x) __tmp = (x);                                             \
            res = 0;                                                           \
            while (!(__tmp & 0xFF) && rem > 0) {                               \
                __tmp >>= 8;                                                   \
                res += 8;                                                      \
                rem -= 8;                                                      \
            }                                                                  \
            res += bits_ctz8((__tmp & 0xFF));                                  \
        }                                                                      \
        res;                                                                   \
    })

#define bits_clz_x(x, width)                                                   \
    ({                                                                         \
        int __res = -1;                                                        \
        int __width = (width);                                                 \
        if (__width > 0) {                                                     \
            typeof(x) __orig = (x);                                            \
            uint64 __val = (uint64)__orig;                                     \
            if (__width < 64) {                                                \
                uint64 __mask = (1ULL << __width) - 1ULL;                      \
                __val &= __mask;                                               \
            }                                                                  \
            if (__val) {                                                       \
                int __highest = -1;                                            \
                int __bit_index = 0;                                           \
                while (__val && __bit_index < __width) {                       \
                    uint8 __byte = (uint8)(__val & 0xFFU);                     \
                    if (__byte) {                                              \
                        int __byte_msb = 7 - bits_clz8(__byte);                \
                        __highest = __bit_index + __byte_msb;                  \
                    }                                                          \
                    __val >>= 8;                                               \
                    __bit_index += 8;                                          \
                }                                                              \
                if (__highest >= __width) {                                    \
                    __highest = __width - 1;                                   \
                }                                                              \
                __res = (__width - 1) - __highest;                             \
            }                                                                  \
        }                                                                      \
        __res;                                                                 \
    })

#define bits_ffs_x(x, width) (bits_ctz_x((x), (width)) + 1)

#define bits_popcount_x(x, width)                                              \
    ({                                                                         \
        int total = 0;                                                         \
        typeof(x) __tmp = (x);                                                 \
        int rem = width;                                                       \
        while (__tmp && rem > 0) {                                             \
            uint8 byte = __tmp & 0xFF;                                         \
            total += bits_popcount8(byte);                                     \
            __tmp >>= 8;                                                       \
            rem -= 8;                                                          \
        }                                                                      \
        total;                                                                 \
    })

#define bits_ffsg(x) bits_ffs_x((uint64)(x), 8 * sizeof(x))
#define bits_clzg(x) bits_clz_x((uint64)(x), 8 * sizeof(x))
#define bits_ctzg(x) bits_ctz_x((uint64)(x), 8 * sizeof(x))
#define bits_popcountg(x) bits_popcount_x((uint64)(x), 8 * sizeof(x))
#define bits_ffs16(x) bits_ffs_x((uint16)(x), 16)
#define bits_clz16(x) bits_clz_x((uint16)(x), 16)
#define bits_ctz16(x) bits_ctz_x((uint16)(x), 16)
#define bits_popcount16(x) bits_popcount_x((uint16)(x), 16)
#define bits_ffs32(x) bits_ffs_x((uint32)(x), 32)
#define bits_clz32(x) bits_clz_x((uint32)(x), 32)
#define bits_ctz32(x) bits_ctz_x((uint32)(x), 32)
#define bits_popcount32(x) bits_popcount_x((uint32)(x), 32)
#define bits_ffs64(x) bits_ffs_x((uint64)(x), 64)
#define bits_clz64(x) bits_clz_x((uint64)(x), 64)
#define bits_ctz64(x) bits_ctz_x((uint64)(x), 64)
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
    for (size_t i = 0;
         i < sizeof(__bits_binary_steps) / sizeof(__bits_binary_steps[0]);
         i++) {
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
    for (size_t i = 0;
         i < sizeof(__bits_binary_steps) / sizeof(__bits_binary_steps[0]);
         i++) {
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

#define bits_ffs8(x) bits_ffs_x((uint8)(x), 8)
#define bits_clz8(x) bits_clz_x((uint8)(x), 8)
#define bits_ctz8(x) bits_ctz_x((uint8)(x), 8)
#define bits_popcount8(x) bits_popcount_x((uint8)(x), 8)
#define bits_ffs16(x) bits_ffs_x((uint16)(x), 16)
#define bits_clz16(x) bits_clz_x((uint16)(x), 16)
#define bits_ctz16(x) bits_ctz_x((uint16)(x), 16)
#define bits_popcount16(x) bits_popcount_x((uint16)(x), 16)
#define bits_ffs32(x) bits_ffs_x((uint32)(x), 32)
#define bits_clz32(x) bits_clz_x((uint32)(x), 32)
#define bits_ctz32(x) bits_ctz_x((uint32)(x), 32)
#define bits_popcount32(x) bits_popcount_x((uint32)(x), 32)
#define bits_ffs64(x) bits_ffs_x((uint64)(x), 64)
#define bits_clz64(x) bits_clz_x((uint64)(x), 64)
#define bits_ctz64(x) bits_ctz_x((uint64)(x), 64)
#define bits_popcount64(x) bits_popcount_x((uint64)(x), 64)
#define bits_ffsg(x) bits_ffs_x((uint64)(x), 8 * sizeof(x))
#define bits_clzg(x) bits_clz_x((uint64)(x), 8 * sizeof(x))
#define bits_ctzg(x) bits_ctz_x((uint64)(x), 8 * sizeof(x))
#define bits_popcountg(x) bits_popcount_x((uint64)(x), 8 * sizeof(x))
#endif

/**
 * __bits_ctz_ptr - find first set (or clear) bit in a memory region
 * @ptr:   pointer to the start of the memory region
 * @limit: maximum number of bits to scan (need not be byte-aligned)
 * @inv:   when true, search for the first *clear* bit instead
 *
 * Scans the memory region starting at @ptr in little-endian bit order and
 * returns the absolute bit index of the first set bit (or first clear bit
 * when @inv is true).  The search is bounded to the first @limit bits;
 * any matching bit whose index >= @limit is ignored.
 *
 * Internally the scan proceeds in three phases for performance:
 *   1. Byte-by-byte prefix until @ptr is 8-byte aligned.
 *   2. 8-byte aligned chunk reads for the bulk of the buffer.
 *   3. Byte-by-byte remainder for the trailing partial chunk.
 *
 * Return: bit index (0-based) on success, or -1 if @ptr is NULL or no
 *         matching bit exists within the first @limit bits.
 */
static inline int64 __bits_ctz_ptr(const void *ptr, size_t limit, bool inv) {
    if (ptr == NULL) {
        return -1;
    }
    size_t byte_limit = (limit + 7) >> 3; // number of bytes to cover the bit limit
    const uint8 *byte_ptr = (const uint8 *)ptr;
    const uint8 *aligned_start =
        (const uint8 *)(((size_t)byte_ptr + 7) & ~0x7UL);
    // First process trailing bytes to align to 8-byte boundary
    size_t trailing_bytes = aligned_start - byte_ptr;
    if (trailing_bytes > byte_limit) {
        trailing_bytes = byte_limit; // Clamp to buffer size
    }
    for (size_t i = 0; i < trailing_bytes; i++) {
        uint8 byte = byte_ptr[i];
        if (inv) {
            byte = ~byte;
        }
        if (byte) {
            int64 bit_pos = (i << 3) | bits_ctz8(byte);
            if (bit_pos >= (int64)limit) {
                return -1; // Found bit is beyond the limit
            }
            return bit_pos;
        }
    }

    // Process 8-byte aligned chunks (only complete chunks within byte_limit)
    int64 index;
    for (index = trailing_bytes; (size_t)index + 8 <= byte_limit; index += 8) {
        uint64 chunk = *((const uint64 *)(byte_ptr + index));
        if (inv) {
            chunk = ~chunk;
        }
        if (chunk) {
            int64 bit_pos = (index << 3) + bits_ctz64(chunk);
            if (bit_pos >= (int64)limit) {
                return -1; // Found bit is beyond the limit
            }
            return bit_pos;
        }
    }

    // Process remaining bytes
    for (; (size_t)index < byte_limit; index++) {
        uint8 byte = byte_ptr[index];
        if (inv) {
            byte = ~byte;
        }
        if (byte) {
            int64 bit_pos = (index << 3) | bits_ctz8(byte);
            if (bit_pos >= (int64)limit) {
                return -1; // Found bit is beyond the limit
            }
            return bit_pos;
        }
    }
    return -1;
}

/**
 * __bits_ctz_ptr_from - find first set (or clear) bit starting from an offset
 * @ptr:   pointer to the start of the memory region
 * @from:  bit index at which to begin the search (inclusive)
 * @limit: maximum bit index (exclusive) — bits at or beyond @limit are ignored
 * @inv:   when true, search for the first *clear* bit instead
 *
 * Behaves like __bits_ctz_ptr() but skips all bits before @from.  The
 * partial first byte (bits [@from, next byte boundary)) is handled
 * inline; any remaining whole bytes are delegated to __bits_ctz_ptr().
 *
 * Return: absolute bit index (0-based, relative to @ptr) on success,
 *         or -1 if @ptr is NULL, @from >= @limit, or no matching bit
 *         exists in [@from, @limit).
 */
static inline int64 __bits_ctz_ptr_from(const void *ptr, size_t from, size_t limit, bool inv) {
    if (ptr == NULL || from >= limit) {
        return -1;
    }
    
    // Scan partial first byte (bits [from, next_byte_boundary))
    size_t start_byte_index = from >> 3;
    uint8 first_byte = ((const uint8 *)ptr)[start_byte_index];
    if (inv) {
        first_byte = ~first_byte;
    }
    first_byte >>= (from & 0x7);
    int64 ret = bits_ctz8(first_byte);
    if (ret >= 0) {
        int64 bit_pos = (int64)from + ret;
        return (bit_pos < (int64)limit) ? bit_pos : -1;
    }
    start_byte_index++;

    size_t byte_limit = (limit + 7) >> 3;
    if (start_byte_index >= byte_limit) {
        return -1;
    }

    // Delegate remaining whole bytes to __bits_ctz_ptr
    const uint8 *byte_ptr = (const uint8 *)ptr + start_byte_index;
    size_t remaining_bits = limit - (start_byte_index << 3);
    int64 sub_ret = __bits_ctz_ptr(byte_ptr, remaining_bits, inv);
    if (sub_ret < 0) {
        return -1;
    }
    return (int64)(start_byte_index << 3) + sub_ret;
}

/**
 * @brief Find the next set bit after a given position in a 64-bit bitmap.
 * @param bits The 64-bit bitmap to search.
 * @param last The bit position to start searching from (exclusive).
 *             Use -1 to find the first set bit (position 0+).
 * @return The index of the next set bit after position 'last', or -1 if none
 * found.
 *
 * Example usage:
 *   uint64 bitmap = 0b101010;  // bits set at positions 1, 3, 5
 *   int pos = -1;
 *   while ((pos = bits_next_bit_set(bitmap, pos)) >= 0) {
 *       // pos will be 1, then 3, then 5
 *   }
 */
static inline int bits_next_bit_set(uint64 bits, int last) {
    int start = last + 1;
    if (start >= 64) {
        return -1;  // No more bits to search
    }
    int delta = bits_ctz64(bits >> start);
    if (delta < 0) {
        return -1;
    }
    return start + delta;
}

/**
 * @brief Iterate over all set bits in a 64-bit bitmap.
 * @param __bits The 64-bit bitmap to iterate over.
 * @param __pos  Loop variable (int) that receives each set bit's index.
 *
 * This macro provides a convenient for-loop to iterate through all set bits
 * in a bitmap, from least significant to most significant.
 *
 * Example usage:
 *   uint64 bitmap = 0b101010;  // bits set at positions 1, 3, 5
 *   int pos;
 *   bits_foreach_set_bit(bitmap, pos) {
 *       printf("bit %d is set\n", pos);  // prints 1, 3, 5
 *   }
 *
 * @note Do not modify __bits during iteration.
 * @note __pos must be declared before the loop (not inside the for).
 */
#define bits_foreach_set_bit(__bits, __pos)                                    \
    for ((__pos) = bits_next_bit_set((__bits), -1); (__pos) >= 0;              \
         (__pos) = bits_next_bit_set((__bits), (__pos)))

#define bits_ctz_ptr(ptr, limit) __bits_ctz_ptr((ptr), (limit), !!0)
#define bits_ctz_ptr_inv(ptr, limit) __bits_ctz_ptr((ptr), (limit), !!1)
#define bits_ctz_ptr_from(ptr, from, limit) __bits_ctz_ptr_from((ptr), (from), (limit), !!0)
#define bits_ctz_ptr_from_inv(ptr, from, limit) __bits_ctz_ptr_from((ptr), (from), (limit), !!1)

#define bswap16(x) (typeof(x))((((x) & 0x00FF) << 8) | (((x) & 0xFF00) >> 8))
#define bswap32(x)                                                             \
    (typeof(x))((((x) & 0x000000FF) << 24) | (((x) & 0x0000FF00) << 8) |       \
                (((x) & 0x00FF0000) >> 8) | (((x) & 0xFF000000) >> 24))
#define bswap64(x)                                                             \
    (typeof(x))((((x) & 0x00000000000000FFULL) << 56) |                        \
                    (((x) & 0x000000000000FF00ULL) << 40) |                    \
                    (((x) & 0x0000000000FF0000ULL) << 24) |                    \
                    (((x) & 0x00000000FF000000ULL) << 8) |                     \
                    (((x) & 0x000000FF00000000ULL) >> 8) ||                    \
                (((x) & 0x0000FF0000000000ULL) >> 24) |                        \
                    (((x) & 0x00FF0000000000000ULL) >> 40) |                   \
                    (((x) & 0xFF00000000000000ULL) >> 56))

/***
 * Bitmap manipulation macros
 *
 * These macros generate inline functions for atomic-style bit operations on
 * bitmaps. The bitmap is treated as an array of unsigned integers of the
 * specified width.
 *
 * Generated functions (where N is 8, 16, 32, or 64):
 *   - bits_test_and_set_bitN(bitmap, bit_index)   : Set bit and return previous
 * value
 *   - bits_test_and_clear_bitN(bitmap, bit_index) : Clear bit and return
 * previous value
 *   - bits_test_bitN(bitmap, bit_index)           : Test if bit is set
 *
 * Macro expansion:
 *   __bits_bitmap_action generates a function that computes the element index
 * and bit mask, then delegates to __ACTION for the actual bit operation. The
 * __ACTION parameter is a macro that receives a pointer to the storage element
 * and the mask:
 *     - __bit_bitmap_setter  : Sets the bit, returns true if it was already set
 *     - __bit_bitmap_clearer : Clears the bit, returns true if it was set
 *     - __bit_bitmap_tester  : Returns true if the bit is set (no modification)
 *
 * Parameters:
 *   @bitmap    : Pointer to the bitmap (void* for flexibility)
 *   @bit_index : Zero-based index of the bit to operate on
 *
 * Return value:
 *   - test_and_set / test_and_clear: true if the bit WAS set before the
 * operation
 *   - test: true if the bit IS currently set
 *
 * Implementation notes:
 *   - __NAME  : Suffix for generated function names (e.g., bit8, bit64)
 *   - __BITS  : Width of storage unit (8, 16, 32, 64) - must be literal for
 * token pasting
 *   - __SHIFT : log2(__BITS) for index calculation (3, 4, 5, 6) - must be
 * literal
 *   - The bit mask uses (uint##__BITS)1 to ensure proper width before shifting,
 *     avoiding undefined behavior when bit positions exceed 32 for 64-bit
 * operations.
 */

#define __bits_bitmap_action(__NAME, __ACTION_NAME, __BITS, __SHIFT, __ACTION) \
    static inline bool bits_##__ACTION_NAME##_##__NAME(void *bitmap,           \
                                                       size_t bit_index) {     \
        size_t byte_index = bit_index >> __SHIFT;                              \
        uint##__BITS bit_mask = (uint##__BITS)1                                \
                                << (bit_index & ((1UL << __SHIFT) - 1UL));     \
        return __ACTION(&((uint##__BITS *)bitmap)[byte_index], bit_mask);      \
    }

/* Set bit at __ptr using __mask, return true if bit was previously set */
#define __bit_bitmap_setter(__ptr, __mask)                                     \
    ({                                                                         \
        bool was_set = (*(__ptr) & (__mask)) != 0;                             \
        *(__ptr) |= (__mask);                                                  \
        was_set;                                                               \
    })

/* Clear bit at __ptr using __mask, return true if bit was previously set */
#define __bit_bitmap_clearer(__ptr, __mask)                                    \
    ({                                                                         \
        bool was_set = (*(__ptr) & (__mask)) != 0;                             \
        *(__ptr) &= ~(__mask);                                                 \
        was_set;                                                               \
    })

/* Test bit at __ptr using __mask, return true if bit is set */
#define __bit_bitmap_tester(__ptr, __mask)                                     \
    ({                                                                         \
        bool is_set = (*(__ptr) & (__mask)) != 0;                              \
        is_set;                                                                \
    })

/*
 * Generate test_and_set, test_and_clear, and test functions for a given bit
 * width.
 * __SETTER, __CLEARER, __TESTER are interface macros that must conform to:
 *   __INTERFACE(__ptr, __mask) -> bool
 * where __ptr is a pointer to the storage element and __mask is the bit mask.
 * Each interface should return the previous or current state of the bit.
 */
#define __bits_bitmap_helpers(__NAME, __BITS, __SHIFT, __SETTER, __CLEARER,    \
                              __TESTER)                                        \
    __bits_bitmap_action(__NAME, test_and_set, __BITS, __SHIFT, __SETTER)      \
        __bits_bitmap_action(__NAME, test_and_clear, __BITS, __SHIFT,          \
                             __CLEARER)                                        \
            __bits_bitmap_action(__NAME, test, __BITS, __SHIFT, __TESTER)

/* Instantiate bitmap helpers for 8, 16, 32, and 64-bit storage units */
__bits_bitmap_helpers(bit8, 8, 3, __bit_bitmap_setter, __bit_bitmap_clearer,
                      __bit_bitmap_tester)
    __bits_bitmap_helpers(bit16, 16, 4, __bit_bitmap_setter,
                          __bit_bitmap_clearer, __bit_bitmap_tester)
        __bits_bitmap_helpers(bit32, 32, 5, __bit_bitmap_setter,
                              __bit_bitmap_clearer, __bit_bitmap_tester)
            __bits_bitmap_helpers(bit64, 64, 6, __bit_bitmap_setter,
                                  __bit_bitmap_clearer, __bit_bitmap_tester)

#endif // KERNEL_INC_BITS_H
