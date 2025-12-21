#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdio.h>
#include <cmocka.h>

#include "bits.h"

static int
naive_ffs8(uint8 value)
{
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

static int
naive_ctz8(uint8 value)
{
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

static int
naive_clz8(uint8 value)
{
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

static int
naive_popcount8(uint8 value)
{
    int total = 0;
    uint8 tmp = value;
    while (tmp != 0U) {
        total += (tmp & 0x1U);
        tmp >>= 1U;
    }
    return total;
}

static int
naive_ctz_u64(uint64 value)
{
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

static int
naive_clz_width(uint64 value, unsigned width)
{
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

static int
naive_popcount_u64(uint64 value)
{
    int total = 0;
    uint64 tmp = value;
    while (tmp != 0ULL) {
        total += (int)(tmp & 0x1ULL);
        tmp >>= 1ULL;
    }
    return total;
}

static int
naive_ffs_u64(uint64 value)
{
    if (value == 0ULL) {
        return 0;
    }
    return naive_ctz_u64(value) + 1;
}

static void
test_bits_ffs8_matches_naive(void **state)
{
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_ffs8(x), naive_ffs8(x));
    }
}

static void
test_bits_ctz8_matches_naive(void **state)
{
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_ctz8(x), naive_ctz8(x));
    }
}

static void
test_bits_clz8_matches_naive(void **state)
{
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_clz8(x), naive_clz8(x));
    }
}

static void
test_bits_popcount8_matches_naive(void **state)
{
    (void)state;

    for (int value = 0; value < 256; value++) {
        uint8 x = (uint8)value;
        assert_int_equal(bits_popcount8(x), naive_popcount8(x));
    }
}

static void
test_bits_ctzg_multiwidth(void **state)
{
    (void)state;

    const uint16 samples16[] = {0x0001U, 0x0002U, 0x0004U, 0x0040U, 0x0400U, 0x8000U};
    for (size_t i = 0; i < sizeof(samples16) / sizeof(samples16[0]); i++) {
        uint16 value = samples16[i];
        assert_int_equal(bits_ctzg(value), naive_ctz_u64(value));
    }

    const uint32 samples32[] = {
        0x00000001U,
        0x00000002U,
        0x00000010U,
        0x00008000U,
        0x01000000U,
        0x40000000U,
        0x80000000U
    };
    for (size_t i = 0; i < sizeof(samples32) / sizeof(samples32[0]); i++) {
        uint32 value = samples32[i];
        assert_int_equal(bits_ctzg(value), naive_ctz_u64(value));
    }

    const uint64 samples64[] = {
        0x0000000000000001ULL,
        0x0000000000000002ULL,
        0x0000000000010000ULL,
        0x0000000100000000ULL,
        0x0001000000000000ULL,
        0x0102030400000000ULL,
        0x8000000000000000ULL
    };
    for (size_t i = 0; i < sizeof(samples64) / sizeof(samples64[0]); i++) {
        uint64 value = samples64[i];
        assert_int_equal(bits_ctzg(value), naive_ctz_u64(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_ctzg(shifted_left), naive_ctz_u64(shifted_left));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_ctzg(shifted_right), naive_ctz_u64(shifted_right));
        }
    }
}

static void
test_bits_clzg_multiwidth(void **state)
{
    (void)state;

    const uint16 samples16[] = {
        0x0001U,
        0x0002U,
        0x0010U,
        0x0100U,
        0x0F00U,
        0x7FFFU,
        0x8000U
    };
    for (size_t i = 0; i < sizeof(samples16) / sizeof(samples16[0]); i++) {
        uint16 value = samples16[i];
        assert_int_equal(bits_clzg(value), naive_clz_width(value, 16));
        for (unsigned shift = 1; shift < 8; shift++) {
            uint16 shifted_left = (uint16)(value << shift);
            assert_int_equal(bits_clzg(shifted_left), naive_clz_width(shifted_left, 16));
            uint16 shifted_right = (uint16)(value >> shift);
            assert_int_equal(bits_clzg(shifted_right), naive_clz_width(shifted_right, 16));
        }
    }

    const uint32 samples32[] = {
        0x00000001U,
        0x00000010U,
        0x00000F00U,
        0x000F0000U,
        0x00F00000U,
        0x7FFFFFFFU,
        0x80000000U
    };
    const size_t expected[] = {31, 27, 20, 12, 8, 1, 0};
    for (size_t i = 0; i < sizeof(samples32) / sizeof(samples32[0]); i++) {
        uint32 value = samples32[i];
        assert_int_equal(bits_clzg(value), expected[i]);
        for (unsigned shift = 0; shift < 16; shift++) {
            uint32 shifted_left = (uint32)(value << shift);
            assert_int_equal(bits_clzg(shifted_left), naive_clz_width(shifted_left, 32));
            uint32 shifted_right = (uint32)(value >> shift);
            assert_int_equal(bits_clzg(shifted_right), naive_clz_width(shifted_right, 32));
        }
    }

    const uint64 samples64[] = {
        0x0000000000000001ULL,
        0x0000000000000010ULL,
        0x0000000000100000ULL,
        0x0000000010000000ULL,
        0x0000000F00000000ULL,
        0x0F00000000000000ULL,
        0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL
    };
    for (size_t i = 0; i < sizeof(samples64) / sizeof(samples64[0]); i++) {
        uint64 value = samples64[i];
        assert_int_equal(bits_clzg(value), naive_clz_width(value, 64));
        printf("Testing value: 0x%016llx, expected: %d, got: %d\n", (unsigned long long)value, naive_clz_width(value, 64), bits_clzg(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_clzg(shifted_left), naive_clz_width(shifted_left, 64));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_clzg(shifted_right), naive_clz_width(shifted_right, 64));
        }
    }

    assert_int_equal(bits_clzg((uint16)0), -1);
    assert_int_equal(bits_clzg((uint32)0), -1);
    assert_int_equal(bits_clzg((uint64)0), -1);
}

static void
test_bits_popcountg_multiwidth(void **state)
{
    (void)state;

    const uint16 samples16[] = {0x0000U, 0x0001U, 0x00FFU, 0x0F0FU, 0xF00FU, 0xFFFFU};
    for (size_t i = 0; i < sizeof(samples16) / sizeof(samples16[0]); i++) {
        uint16 value = samples16[i];
        assert_int_equal(bits_popcountg(value), naive_popcount_u64(value));
    }

    const uint32 samples32[] = {
        0x00000000U,
        0x00000001U,
        0x0000FFFFU,
        0x00FF00FFU,
        0x0F0F0F0FU,
        0xF0F0F0F0U,
        0xFFFFFFFFU
    };
    for (size_t i = 0; i < sizeof(samples32) / sizeof(samples32[0]); i++) {
        uint32 value = samples32[i];
        assert_int_equal(bits_popcountg(value), naive_popcount_u64(value));
    }

    const uint64 samples64[] = {
        0x0000000000000000ULL,
        0x0000000000000001ULL,
        0x00000000FFFFFFFFULL,
        0x0000FFFF0000FFFFULL,
        0x0123456789ABCDEFULL,
        0xAAAAAAAA55555555ULL,
        0xFFFFFFFFFFFFFFFFULL
    };
    for (size_t i = 0; i < sizeof(samples64) / sizeof(samples64[0]); i++) {
        uint64 value = samples64[i];
        assert_int_equal(bits_popcountg(value), naive_popcount_u64(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_popcountg(shifted_left), naive_popcount_u64(shifted_left));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_popcountg(shifted_right), naive_popcount_u64(shifted_right));
        }
    }
}

static void
test_bits_ffsg_matches_naive(void **state)
{
    (void)state;

    const uint64 samples[] = {
        0x0000000000000000ULL,
        0x0000000000000001ULL,
        0x0000000000000010ULL,
        0x0000000010000000ULL,
        0x0000000100000000ULL,
        0x0000008000000000ULL,
        0x0000100000000000ULL,
        0x1000000000000000ULL
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64 value = samples[i];
        assert_int_equal(bits_ffsg(value), naive_ffs_u64(value));
        for (unsigned shift = 1; shift < 16; shift++) {
            uint64 shifted_left = value << shift;
            assert_int_equal(bits_ffsg(shifted_left), naive_ffs_u64(shifted_left));
            uint64 shifted_right = value >> shift;
            assert_int_equal(bits_ffsg(shifted_right), naive_ffs_u64(shifted_right));
        }
    }
}

static void
test___bits_ctz_ptr_null(void **state)
{
    (void)state;
    assert_int_equal(bits_ctz_ptr(NULL, 4), -1);
}

static void
test___bits_ctz_ptr_no_match(void **state)
{
    (void)state;
    uint8 data[3] = {0x00U, 0x00U, 0x00U};
    assert_int_equal(bits_ctz_ptr(data, sizeof(data)), -1);
}

static void
test___bits_ctz_ptr_basic(void **state)
{
    (void)state;
    uint8 data[3] = {0x00U, 0x04U, 0x00U};
    int64 expected = (1LL << 3) | 2LL;
    assert_int_equal(bits_ctz_ptr(data, sizeof(data)), expected);
}

static void
test___bits_ctz_ptr_inverted(void **state)
{
    (void)state;
    uint8 data[2] = {0xFFU, 0xF0U};
    int64 expected = (1LL << 3);
    assert_int_equal(bits_ctz_ptr_inv(data, sizeof(data)), expected);
}

static void
test___bits_ctz_ptr_limit(void **state)
{
    (void)state;
    uint8 data[2] = {0x00U, 0x08U};
    assert_int_equal(__bits_ctz_ptr(data, 1, false), 11);
}

static void
test___bits_ctz_ptr_long_buffer(void **state)
{
    (void)state;
    uint8 data[32] = {0};
    data[17] = 0x20U;
    int64 expected = (17LL << 3) | 5LL;
    assert_int_equal(bits_ctz_ptr(data, sizeof(data)), expected);

    uint8 inverted[32];
    for (size_t i = 0; i < sizeof(inverted); i++) {
        inverted[i] = 0xFFU;
    }
    inverted[24] = 0x7FU;
    int64 expected_inv = (24LL << 3) | 7LL;
    assert_int_equal(bits_ctz_ptr_inv(inverted, sizeof(inverted)), expected_inv);
}

int
main(void)
{
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
    };

    return cmocka_run_group_tests_name("bits", tests, NULL, NULL);
}
