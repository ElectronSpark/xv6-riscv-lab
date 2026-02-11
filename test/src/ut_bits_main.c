#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdio.h>
#include <cmocka.h>

#include "bits.h"

static int naive_ffs8(uint8 value) {
    if (value == 0) {
        return 0;
    }
    int index = 1;
    uint8 tmp = value;
    while ((tmp & 0x1U) == 0U) {
        tmp >>= 1U;
        index++;
    }
    return index;
}

static int naive_ctz8(uint8 value) {
    if (value == 0) {
        return -1;
    }
    int count = 0;
    uint8 tmp = value;
    while ((tmp & 0x1U) == 0U) {
        tmp >>= 1U;
        count++;
    }
    return count;
}

static int naive_clz8(uint8 value) {
    if (value == 0) {
        return -1;
    }
    int count = 0;
    uint8 tmp = value;
    while ((tmp & 0x80U) == 0U) {
        tmp <<= 1U;
        count++;
    }
    return count;
}

static int naive_popcount8(uint8 value) {
    int total = 0;
    uint8 tmp = value;
    while (tmp != 0U) {
        total += (tmp & 0x1U);
        tmp >>= 1U;
    }
    return total;
}

static int naive_ctz_u64(uint64 value) {
    if (value == 0) {
        return -1;
    }
    int count = 0;
    uint64 tmp = value;
    while ((tmp & 0x1ULL) == 0ULL) {
        tmp >>= 1ULL;
        count++;
    }
    return count;
}

static int naive_clz_width(uint64 value, unsigned width) {
    if (width == 0U) {
        return -1;
    }
    if (width > 64U) {
        width = 64U;
    }
    if (width < 64U) {
        uint64 mask = (1ULL << width) - 1ULL;
        value &= mask;
    }
    if (value == 0ULL) {
        return -1;
    }
    uint64 msb_mask = (width == 64U) ? (1ULL << 63) : (1ULL << (width - 1U));
    int count = 0;
    while ((value & msb_mask) == 0ULL) {
        count++;
        msb_mask >>= 1U;
    }
    return count;
}

static int naive_popcount_u64(uint64 value) {
    int total = 0;
    uint64 tmp = value;
    while (tmp != 0ULL) {
        total += (int)(tmp & 0x1ULL);
        tmp >>= 1ULL;
    }
    return total;
}

static int naive_ffs_u64(uint64 value) {
    if (value == 0ULL) {
        return 0;
    }
    return naive_ctz_u64(value) + 1;
}

static void test_bits_ffs8_matches_naive(void **state) {
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_ffs8(x), naive_ffs8(x));
    }
}

static void test_bits_ctz8_matches_naive(void **state) {
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_ctz8(x), naive_ctz8(x));
    }
}

static void test_bits_clz8_matches_naive(void **state) {
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_clz8(x), naive_clz8(x));
    }
}

static void test_bits_popcount8_matches_naive(void **state) {
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_popcount8(x), naive_popcount8(x));
    }
}

static void test_bits_ctzg_multiwidth(void **state) {
    (void)state;

    const uint16 samples16[] = {0x0001U, 0x0002U, 0x0004U,
                                0x0040U, 0x0400U, 0x8000U};
    for (size_t i = 0; i < sizeof(samples16) / sizeof(samples16[0]); i++) {
        uint16 value = samples16[i];
        assert_int_equal(bits_ctzg(value), naive_ctz_u64(value));
    }

    const uint32 samples32[] = {0x00000001U, 0x00000002U, 0x00000010U,
                                0x00008000U, 0x01000000U, 0x40000000U,
                                0x80000000U};
    for (size_t i = 0; i < sizeof(samples32) / sizeof(samples32[0]); i++) {
        uint32 value = samples32[i];
        assert_int_equal(bits_ctzg(value), naive_ctz_u64(value));
    }

    const uint64 samples64[] = {0x0000000000000001ULL, 0x0000000000000002ULL,
                                0x0000000000010000ULL, 0x0000000100000000ULL,
                                0x0001000000000000ULL, 0x0102030400000000ULL,
                                0x8000000000000000ULL};
    for (size_t i = 0; i < sizeof(samples64) / sizeof(samples64[0]); i++) {
        uint64 value = samples64[i];
        assert_int_equal(bits_ctzg(value), naive_ctz_u64(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_ctzg(shifted_left),
                             naive_ctz_u64(shifted_left));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_ctzg(shifted_right),
                             naive_ctz_u64(shifted_right));
        }
    }
}

static void test_bits_clzg_multiwidth(void **state) {
    (void)state;

    const uint16 samples16[] = {0x0001U, 0x0002U, 0x0010U, 0x0100U,
                                0x0F00U, 0x7FFFU, 0x8000U};
    for (size_t i = 0; i < sizeof(samples16) / sizeof(samples16[0]); i++) {
        uint16 value = samples16[i];
        assert_int_equal(bits_clzg(value), naive_clz_width(value, 16));
        for (unsigned shift = 1; shift < 8; shift++) {
            uint16 shifted_left = (uint16)(value << shift);
            assert_int_equal(bits_clzg(shifted_left),
                             naive_clz_width(shifted_left, 16));
            uint16 shifted_right = (uint16)(value >> shift);
            assert_int_equal(bits_clzg(shifted_right),
                             naive_clz_width(shifted_right, 16));
        }
    }

    const uint32 samples32[] = {0x00000001U, 0x00000010U, 0x00000F00U,
                                0x000F0000U, 0x00F00000U, 0x7FFFFFFFU,
                                0x80000000U};
    const size_t expected[] = {31, 27, 20, 12, 8, 1, 0};
    for (size_t i = 0; i < sizeof(samples32) / sizeof(samples32[0]); i++) {
        uint32 value = samples32[i];
        assert_int_equal(bits_clzg(value), expected[i]);
        for (unsigned shift = 0; shift < 16; shift++) {
            uint32 shifted_left = (uint32)(value << shift);
            assert_int_equal(bits_clzg(shifted_left),
                             naive_clz_width(shifted_left, 32));
            uint32 shifted_right = (uint32)(value >> shift);
            assert_int_equal(bits_clzg(shifted_right),
                             naive_clz_width(shifted_right, 32));
        }
    }

    const uint64 samples64[] = {0x0000000000000001ULL, 0x0000000000000010ULL,
                                0x0000000000100000ULL, 0x0000000010000000ULL,
                                0x0000000F00000000ULL, 0x0F00000000000000ULL,
                                0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL};
    for (size_t i = 0; i < sizeof(samples64) / sizeof(samples64[0]); i++) {
        uint64 value = samples64[i];
        assert_int_equal(bits_clzg(value), naive_clz_width(value, 64));
        printf("Testing value: 0x%016llx, expected: %d, got: %d\n",
               (unsigned long long)value, naive_clz_width(value, 64),
               bits_clzg(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_clzg(shifted_left),
                             naive_clz_width(shifted_left, 64));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_clzg(shifted_right),
                             naive_clz_width(shifted_right, 64));
        }
    }

    assert_int_equal(bits_clzg((uint16)0), -1);
    assert_int_equal(bits_clzg((uint32)0), -1);
    assert_int_equal(bits_clzg((uint64)0), -1);
}

static void test_bits_popcountg_multiwidth(void **state) {
    (void)state;

    const uint16 samples16[] = {0x0000U, 0x0001U, 0x00FFU,
                                0x0F0FU, 0xF00FU, 0xFFFFU};
    for (size_t i = 0; i < sizeof(samples16) / sizeof(samples16[0]); i++) {
        uint16 value = samples16[i];
        assert_int_equal(bits_popcountg(value), naive_popcount_u64(value));
    }

    const uint32 samples32[] = {0x00000000U, 0x00000001U, 0x0000FFFFU,
                                0x00FF00FFU, 0x0F0F0F0FU, 0xF0F0F0F0U,
                                0xFFFFFFFFU};
    for (size_t i = 0; i < sizeof(samples32) / sizeof(samples32[0]); i++) {
        uint32 value = samples32[i];
        assert_int_equal(bits_popcountg(value), naive_popcount_u64(value));
    }

    const uint64 samples64[] = {0x0000000000000000ULL, 0x0000000000000001ULL,
                                0x00000000FFFFFFFFULL, 0x0000FFFF0000FFFFULL,
                                0x0123456789ABCDEFULL, 0xAAAAAAAA55555555ULL,
                                0xFFFFFFFFFFFFFFFFULL};
    for (size_t i = 0; i < sizeof(samples64) / sizeof(samples64[0]); i++) {
        uint64 value = samples64[i];
        assert_int_equal(bits_popcountg(value), naive_popcount_u64(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_popcountg(shifted_left),
                             naive_popcount_u64(shifted_left));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_popcountg(shifted_right),
                             naive_popcount_u64(shifted_right));
        }
    }
}

static void test_bits_ffsg_matches_naive(void **state) {
    (void)state;

    const uint64 samples[] = {0x0000000000000000ULL, 0x0000000000000001ULL,
                              0x0000000000000010ULL, 0x0000000010000000ULL,
                              0x0000000100000000ULL, 0x0000008000000000ULL,
                              0x0000100000000000ULL, 0x1000000000000000ULL};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64 value = samples[i];
        assert_int_equal(bits_ffsg(value), naive_ffs_u64(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_ffsg(shifted_left),
                             naive_ffs_u64(shifted_left));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_ffsg(shifted_right),
                             naive_ffs_u64(shifted_right));
        }
    }
}

static void test___bits_ctz_ptr_null(void **state) {
    (void)state;
    assert_int_equal(bits_ctz_ptr(NULL, 4), -1);
}

static void test___bits_ctz_ptr_no_match(void **state) {
    (void)state;
    // Use 8-byte aligned buffer to avoid read overflow in __bits_ctz_ptr
    uint64 data = 0x0000000000000000ULL;
    assert_int_equal(bits_ctz_ptr(&data, sizeof(data) * 8), -1);
}

static void test___bits_ctz_ptr_basic(void **state) {
    (void)state;
    // Use 8-byte aligned buffer to avoid read overflow in __bits_ctz_ptr
    uint64 data = 0x0000000000000400ULL; // bit 10 set (byte 1, bit 2)
    int64 expected = (1LL << 3) | 2LL;   // byte index 1, bit index 2
    assert_int_equal(bits_ctz_ptr(&data, sizeof(data) * 8), expected);
}

static void test___bits_ctz_ptr_inverted(void **state) {
    (void)state;
    // Use 8-byte aligned buffer to avoid read overflow in __bits_ctz_ptr
    uint64 data = 0x000000000000F0FFULL; // inverted: first 0 at byte 1, bit 4
    int64 expected = (1LL << 3);         // byte index 1, bit index 0
    assert_int_equal(bits_ctz_ptr_inv(&data, sizeof(data) * 8), expected);
}

static void test___bits_ctz_ptr_limit(void **state) {
    (void)state;
    // Use 8-byte aligned buffer to avoid read overflow in __bits_ctz_ptr
    uint64 data = 0x0000000000000800ULL; // bit 11 set (byte 1, bit 3)
    // Only search first 1 byte, so bit 11 should not be found (it's in byte 1)
    assert_int_equal(__bits_ctz_ptr(&data, 1, false), -1);
}

static void test___bits_ctz_ptr_long_buffer(void **state) {
    (void)state;
    uint8 data[32] = {0};
    data[17] = 0x20U;
    int64 expected = (17LL << 3) | 5LL;
    assert_int_equal(bits_ctz_ptr(data, sizeof(data) * 8), expected);

    uint8 inverted[32];
    for (size_t i = 0; i < sizeof(inverted); i++) {
        inverted[i] = 0xFFU;
    }
    inverted[24] = 0x7FU;
    int64 expected_inv = (24LL << 3) | 7LL;
    assert_int_equal(bits_ctz_ptr_inv(inverted, sizeof(inverted) * 8),
                     expected_inv);
}

/*
 * __bits_ctz_ptr — unaligned / small buffer tests (bugs #1, #2)
 */

static void test___bits_ctz_ptr_small_unaligned(void **state) {
    (void)state;
    /*
     * Bug #1: When ptr is unaligned, trailing_bytes could exceed byte_limit,
     * causing an OOB read.  Place a 3-byte buffer inside an 8-byte aligned
     * area so ASAN can catch any over-read.
     */
    uint8 __attribute__((aligned(8))) pad[16] = {0};
    /* Put the 3 bytes at an offset that is NOT 8-byte-aligned. */
    uint8 *buf = pad + 1; /* unaligned */
    buf[0] = 0x00;
    buf[1] = 0x04; /* bit 2 of byte 1 → absolute bit 10 */
    buf[2] = 0x00;

    /* limit = 24 bits (3 bytes). trailing_bytes = 7, clamped to 3. */
    assert_int_equal(__bits_ctz_ptr(buf, 24, false), (1 << 3) | 2);
    /* Same but inverted: first zero bit is bit 0 of byte 0. */
    buf[0] = 0xFF;
    buf[1] = 0xFF;
    buf[2] = 0xFE; /* bit 0 of byte 2 is zero */
    assert_int_equal(__bits_ctz_ptr(buf, 24, true), (2 << 3) | 0);
}

static void test___bits_ctz_ptr_1byte_buffer(void **state) {
    (void)state;
    /* Edge case: buffer is exactly 1 byte, ptr unaligned. */
    uint8 __attribute__((aligned(8))) pad[16] = {0};
    uint8 *buf = pad + 3; /* unaligned */
    buf[0] = 0x80;        /* bit 7 set */
    assert_int_equal(__bits_ctz_ptr(buf, 8, false), 7);
    /* Bit 7 set → limit 7 means bit 7 is OUT of range. */
    assert_int_equal(__bits_ctz_ptr(buf, 7, false), -1);
}

static void test___bits_ctz_ptr_non_8byte_tail(void **state) {
    (void)state;
    /*
     * Bug #2: Buffer whose total length is NOT a multiple of 8.
     * The old code could read a full uint64 past the buffer end.
     * Use 11 bytes (aligned start) so the remainder phase is exercised.
     */
    uint8 __attribute__((aligned(8))) data[16] = {0};
    /* Set bit in byte 10 — the tail region (after the aligned chunk). */
    data[10] = 0x01; /* bit 0 of byte 10 → absolute bit 80 */
    assert_int_equal(bits_ctz_ptr(data, 11 * 8), (10 << 3) | 0);

    /* All zero in 11 bytes → -1 */
    data[10] = 0x00;
    assert_int_equal(bits_ctz_ptr(data, 11 * 8), -1);
}

static void test___bits_ctz_ptr_exact_8byte(void **state) {
    (void)state;
    /* Exactly 8 bytes aligned — exercises the chunk loop with no remainder. */
    uint64 __attribute__((aligned(8))) data = 0;
    /* Set the very last bit (bit 63). */
    data = 0x8000000000000000ULL;
    assert_int_equal(bits_ctz_ptr(&data, 64), 63);
    /* Limit to 63 bits → that bit is out of range. */
    assert_int_equal(__bits_ctz_ptr(&data, 63, false), -1);
}

/*
 * __bits_ctz_ptr_from — partial first byte & offset arithmetic (bugs #3–6)
 */

static void test___bits_ctz_ptr_from_basic(void **state) {
    (void)state;
    /* Bit 10 set (byte 1 bit 2).  Search from bit 0 → find bit 10. */
    uint8 __attribute__((aligned(8))) data[8] = {0};
    data[1] = 0x04;
    assert_int_equal(bits_ctz_ptr_from(data, 0, 64), (1 << 3) | 2);
    /* Search from bit 10 → still find 10. */
    assert_int_equal(bits_ctz_ptr_from(data, 10, 64), 10);
    /* Search from bit 11 → miss. */
    assert_int_equal(bits_ctz_ptr_from(data, 11, 64), -1);
}

static void test___bits_ctz_ptr_from_inv(void **state) {
    (void)state;
    /*
     * Bug #3: inv flag was not applied to the first partial byte.
     * All-ones buffer, first zero at bit 10.
     */
    uint8 __attribute__((aligned(8))) data[8];
    for (size_t i = 0; i < 8; i++)
        data[i] = 0xFF;
    data[1] = 0xFB; /* bit 2 of byte 1 is 0 → inv zero at 10 */

    /* inv search from bit 0 → first zero at bit 10. */
    assert_int_equal(bits_ctz_ptr_from_inv(data, 0, 64), 10);
    /* inv search from bit 5 → still find bit 10 (different first byte). */
    assert_int_equal(bits_ctz_ptr_from_inv(data, 5, 64), 10);
    /* inv search from bit 10 → find bit 10 exactly. */
    assert_int_equal(bits_ctz_ptr_from_inv(data, 10, 64), 10);
    /* inv search from bit 11 → no more zeros. */
    assert_int_equal(bits_ctz_ptr_from_inv(data, 11, 64), -1);
}

static void test___bits_ctz_ptr_from_limit_clips_first_byte(void **state) {
    (void)state;
    /*
     * Bug #4: Result in the first byte at/beyond limit was not rejected.
     * Bit 6 set.  Searching from bit 0 with limit=6 → bit 6 is out of range.
     */
    uint8 __attribute__((aligned(8))) data[8] = {0};
    data[0] = 0x40; /* bit 6 */
    assert_int_equal(bits_ctz_ptr_from(data, 0, 6), -1);
    /* limit=7 → bit 6 is in range. */
    assert_int_equal(bits_ctz_ptr_from(data, 0, 7), 6);
    /* From bit 3 with limit=6 → still out of range. */
    assert_int_equal(bits_ctz_ptr_from(data, 3, 6), -1);
    /* From bit 3 with limit=7 → in range. */
    assert_int_equal(bits_ctz_ptr_from(data, 3, 7), 6);
}

static void test___bits_ctz_ptr_from_offset_multi_byte(void **state) {
    (void)state;
    /*
     * Bugs #5 & #6: remaining_limit and return-value offset were wrong.
     * Set bit 42 (byte 5, bit 2).  Search from bit 10.
     * Delegation should start at byte_ptr + 2 with remaining_bits = limit - 16.
     */
    uint8 __attribute__((aligned(8))) data[8] = {0};
    data[5] = 0x04; /* bit 2 of byte 5 → absolute bit 42 */

    assert_int_equal(bits_ctz_ptr_from(data, 10, 64), 42);
    /* From bit 40 (byte-aligned) → still find 42. */
    assert_int_equal(bits_ctz_ptr_from(data, 40, 64), 42);
    /* From bit 43 → miss. */
    assert_int_equal(bits_ctz_ptr_from(data, 43, 64), -1);
}

static void test___bits_ctz_ptr_from_cross_chunk(void **state) {
    (void)state;
    /*
     * Search across an 8-byte chunk boundary.
     * 16 bytes, set bit in second chunk (byte 12, bit 0 → absolute bit 96).
     */
    uint8 __attribute__((aligned(8))) data[16] = {0};
    data[12] = 0x01;

    assert_int_equal(bits_ctz_ptr_from(data, 5, 128), 96);
    /* Confirm the limit still clips properly. */
    assert_int_equal(bits_ctz_ptr_from(data, 5, 96), -1);
    assert_int_equal(bits_ctz_ptr_from(data, 5, 97), 96);
}

static void test___bits_ctz_ptr_from_at_limit_boundary(void **state) {
    (void)state;
    /* from == limit → always -1. */
    uint8 __attribute__((aligned(8))) data[8] = {0xFF};
    assert_int_equal(bits_ctz_ptr_from(data, 5, 5), -1);
    /* from > limit → -1. */
    assert_int_equal(bits_ctz_ptr_from(data, 10, 5), -1);
}

/*
 * bits_foreach_set_bit tests
 */

static void test_bits_foreach_set_bit_zero(void **state) {
    (void)state;
    // Empty bitmap - should not iterate at all
    uint64 bitmap = 0;
    int pos;
    int count = 0;
    bits_foreach_set_bit(bitmap, pos) { count++; }
    assert_int_equal(count, 0);
}

static void test_bits_foreach_set_bit_all_ones(void **state) {
    (void)state;
    // All 64 bits set - should iterate through all positions 0-63
    uint64 bitmap = 0xFFFFFFFFFFFFFFFFULL;
    int pos;
    int count = 0;
    int expected_positions[64];
    int actual_positions[64];

    for (int i = 0; i < 64; i++) {
        expected_positions[i] = i;
    }

    bits_foreach_set_bit(bitmap, pos) {
        assert_true(count < 64);
        actual_positions[count] = pos;
        count++;
    }

    assert_int_equal(count, 64);
    for (int i = 0; i < 64; i++) {
        assert_int_equal(actual_positions[i], expected_positions[i]);
    }
}

static void test_bits_foreach_set_bit_single_lsb(void **state) {
    (void)state;
    // Only LSB (bit 0) set
    uint64 bitmap = 0x1ULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, 0);
        count++;
    }
    assert_int_equal(count, 1);
}

static void test_bits_foreach_set_bit_single_msb(void **state) {
    (void)state;
    // Only MSB (bit 63) set
    uint64 bitmap = 0x8000000000000000ULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, 63);
        count++;
    }
    assert_int_equal(count, 1);
}

static void test_bits_foreach_set_bit_single_middle(void **state) {
    (void)state;
    // Single bit at position 31 (boundary between low and high 32-bits)
    uint64 bitmap = 0x80000000ULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, 31);
        count++;
    }
    assert_int_equal(count, 1);

    // Single bit at position 32
    bitmap = 0x100000000ULL;
    count = 0;
    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, 32);
        count++;
    }
    assert_int_equal(count, 1);
}

static void test_bits_foreach_set_bit_alternating_01(void **state) {
    (void)state;
    // Alternating pattern: 0101... (even positions set)
    uint64 bitmap = 0x5555555555555555ULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        // Should be even positions: 0, 2, 4, ..., 62
        assert_int_equal(pos, count * 2);
        count++;
    }
    assert_int_equal(count, 32);
}

static void test_bits_foreach_set_bit_alternating_10(void **state) {
    (void)state;
    // Alternating pattern: 1010... (odd positions set)
    uint64 bitmap = 0xAAAAAAAAAAAAAAAAULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        // Should be odd positions: 1, 3, 5, ..., 63
        assert_int_equal(pos, count * 2 + 1);
        count++;
    }
    assert_int_equal(count, 32);
}

static void test_bits_foreach_set_bit_sparse(void **state) {
    (void)state;
    // Sparse: bits at positions 0, 7, 15, 31, 32, 47, 63
    uint64 bitmap = (1ULL << 0) | (1ULL << 7) | (1ULL << 15) | (1ULL << 31) |
                    (1ULL << 32) | (1ULL << 47) | (1ULL << 63);
    int expected[] = {0, 7, 15, 31, 32, 47, 63};
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_true(count < 7);
        assert_int_equal(pos, expected[count]);
        count++;
    }
    assert_int_equal(count, 7);
}

static void test_bits_foreach_set_bit_dense_low_byte(void **state) {
    (void)state;
    // Only first byte has bits set (0-7)
    uint64 bitmap = 0xFFULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, count);
        count++;
    }
    assert_int_equal(count, 8);
}

static void test_bits_foreach_set_bit_dense_high_byte(void **state) {
    (void)state;
    // Only last byte has bits set (56-63)
    uint64 bitmap = 0xFF00000000000000ULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, 56 + count);
        count++;
    }
    assert_int_equal(count, 8);
}

static void test_bits_foreach_set_bit_boundary_32bit(void **state) {
    (void)state;
    // Bits around the 32-bit boundary: 30, 31, 32, 33
    uint64 bitmap = (1ULL << 30) | (1ULL << 31) | (1ULL << 32) | (1ULL << 33);
    int expected[] = {30, 31, 32, 33};
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, expected[count]);
        count++;
    }
    assert_int_equal(count, 4);
}

static void test_bits_foreach_set_bit_powers_of_two(void **state) {
    (void)state;
    // Powers of two positions: 0, 1, 2, 4, 8, 16, 32
    uint64 bitmap = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 4) |
                    (1ULL << 8) | (1ULL << 16) | (1ULL << 32);
    int expected[] = {0, 1, 2, 4, 8, 16, 32};
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, expected[count]);
        count++;
    }
    assert_int_equal(count, 7);
}

static void test_bits_foreach_set_bit_two_extremes(void **state) {
    (void)state;
    // Only bits 0 and 63 set
    uint64 bitmap = 0x8000000000000001ULL;
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        if (count == 0) {
            assert_int_equal(pos, 0);
        } else {
            assert_int_equal(pos, 63);
        }
        count++;
    }
    assert_int_equal(count, 2);
}

static void test_bits_foreach_set_bit_consecutive_run(void **state) {
    (void)state;
    // Consecutive run of bits: 20-29
    uint64 bitmap = 0x3FF00000ULL; // bits 20-29
    int pos;
    int count = 0;

    bits_foreach_set_bit(bitmap, pos) {
        assert_int_equal(pos, 20 + count);
        count++;
    }
    assert_int_equal(count, 10);
}

static void test_bits_next_bit_set_no_more(void **state) {
    (void)state;
    // Test bits_next_bit_set when searching past all set bits
    uint64 bitmap = 0x7ULL; // bits 0, 1, 2

    assert_int_equal(bits_next_bit_set(bitmap, -1), 0);
    assert_int_equal(bits_next_bit_set(bitmap, 0), 1);
    assert_int_equal(bits_next_bit_set(bitmap, 1), 2);
    assert_int_equal(bits_next_bit_set(bitmap, 2), -1);  // No more bits
    assert_int_equal(bits_next_bit_set(bitmap, 10), -1); // Way past
    assert_int_equal(bits_next_bit_set(bitmap, 63), -1); // At the end
}

static void test_bits_next_bit_set_zero(void **state) {
    (void)state;
    // Empty bitmap
    uint64 bitmap = 0;
    assert_int_equal(bits_next_bit_set(bitmap, -1), -1);
    assert_int_equal(bits_next_bit_set(bitmap, 0), -1);
    assert_int_equal(bits_next_bit_set(bitmap, 31), -1);
    assert_int_equal(bits_next_bit_set(bitmap, 63), -1);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bits_ffs8_matches_naive),
        cmocka_unit_test(test_bits_ctz8_matches_naive),
        cmocka_unit_test(test_bits_clz8_matches_naive),
        cmocka_unit_test(test_bits_popcount8_matches_naive),
        cmocka_unit_test(test_bits_ctzg_multiwidth),
        cmocka_unit_test(test_bits_clzg_multiwidth),
        cmocka_unit_test(test_bits_popcountg_multiwidth),
        cmocka_unit_test(test_bits_ffsg_matches_naive),
        cmocka_unit_test(test___bits_ctz_ptr_null),
        cmocka_unit_test(test___bits_ctz_ptr_no_match),
        cmocka_unit_test(test___bits_ctz_ptr_basic),
        cmocka_unit_test(test___bits_ctz_ptr_inverted),
        cmocka_unit_test(test___bits_ctz_ptr_limit),
        cmocka_unit_test(test___bits_ctz_ptr_long_buffer),
        cmocka_unit_test(test___bits_ctz_ptr_small_unaligned),
        cmocka_unit_test(test___bits_ctz_ptr_1byte_buffer),
        cmocka_unit_test(test___bits_ctz_ptr_non_8byte_tail),
        cmocka_unit_test(test___bits_ctz_ptr_exact_8byte),
        cmocka_unit_test(test___bits_ctz_ptr_from_basic),
        cmocka_unit_test(test___bits_ctz_ptr_from_inv),
        cmocka_unit_test(test___bits_ctz_ptr_from_limit_clips_first_byte),
        cmocka_unit_test(test___bits_ctz_ptr_from_offset_multi_byte),
        cmocka_unit_test(test___bits_ctz_ptr_from_cross_chunk),
        cmocka_unit_test(test___bits_ctz_ptr_from_at_limit_boundary),
        cmocka_unit_test(test_bits_foreach_set_bit_zero),
        cmocka_unit_test(test_bits_foreach_set_bit_all_ones),
        cmocka_unit_test(test_bits_foreach_set_bit_single_lsb),
        cmocka_unit_test(test_bits_foreach_set_bit_single_msb),
        cmocka_unit_test(test_bits_foreach_set_bit_single_middle),
        cmocka_unit_test(test_bits_foreach_set_bit_alternating_01),
        cmocka_unit_test(test_bits_foreach_set_bit_alternating_10),
        cmocka_unit_test(test_bits_foreach_set_bit_sparse),
        cmocka_unit_test(test_bits_foreach_set_bit_dense_low_byte),
        cmocka_unit_test(test_bits_foreach_set_bit_dense_high_byte),
        cmocka_unit_test(test_bits_foreach_set_bit_boundary_32bit),
        cmocka_unit_test(test_bits_foreach_set_bit_powers_of_two),
        cmocka_unit_test(test_bits_foreach_set_bit_two_extremes),
        cmocka_unit_test(test_bits_foreach_set_bit_consecutive_run),
        cmocka_unit_test(test_bits_next_bit_set_no_more),
        cmocka_unit_test(test_bits_next_bit_set_zero),
    };

    return cmocka_run_group_tests_name("bits", tests, NULL, NULL);
}
