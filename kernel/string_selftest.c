#include "types.h"
#include "printf.h"
#include "string.h"
#include "cmdline.h"

static void expect_ptr(const char *name, const char *base, const char *got,
                       int expected_offset)
{
    if (expected_offset < 0) {
        assert(got == NULL, "string_selftest: %s expected NULL got %p",
               name, got);
        return;
    }

    assert(got == base + expected_offset,
           "string_selftest: %s expected offset %d got %ld",
           name, expected_offset, (long)(got - base));
}

static void expect_mem_ptr(const char *name, const uchar *base, const void *got,
                           int expected_offset)
{
    if (expected_offset < 0) {
        assert(got == NULL, "string_selftest: %s expected NULL got %p",
               name, got);
        return;
    }

    assert(got == base + expected_offset,
           "string_selftest: %s expected offset %d got %ld",
           name, expected_offset, (long)((const uchar *)got - base));
}

static void test_strstr_cases(void)
{
    const char *s = "hello kernel world";

    expect_ptr("strstr-empty-needle", s, strstr(s, ""), 0);
    expect_ptr("strstr-empty-haystack", "", strstr("", "x"), -1);
    expect_ptr("strstr-exact", "abc", strstr("abc", "abc"), 0);
    expect_ptr("strstr-prefix", s, strstr(s, "hello"), 0);
    expect_ptr("strstr-middle", s, strstr(s, "kernel"), 6);
    expect_ptr("strstr-suffix", s, strstr(s, "world"), 13);
    expect_ptr("strstr-not-found", s, strstr(s, "userspace"), -1);
    expect_ptr("strstr-needle-longer", "abc", strstr("abc", "abcd"), -1);
    expect_ptr("strstr-single-char", s, strstr(s, "k"), 6);
    expect_ptr("strstr-first-match", "abcabcabc", strstr("abcabcabc", "abc"),
               0);
}

static void test_repeated_prefix_cases(void)
{
    const char *a = "aaaaaaaaaaaaaaaaab";
    const char *b = "ababababababababca";
    const char *c = "abcxabcdabxabcdabcdabcy";

    expect_ptr("kmp-repeated-a", a, strstr(a, "aaaaab"), 12);
    expect_ptr("kmp-repeated-ab", b, strstr(b, "ababca"), 12);
    expect_ptr("kmp-classic", c, strstr(c, "abcdabcy"), 15);
    expect_ptr("kmp-overlap-first", "aaaabaaaab",
               strstr("aaaabaaaab", "aaab"), 1);
    expect_ptr("kmp-overlap-miss", "ababababababx",
               strstr("ababababababx", "abababz"), -1);
}

static void test_strnstr_cases(void)
{
    const char *s = "0123456789abcdef";

    expect_ptr("strnstr-empty-needle", s, strnstr(s, "", 0), 0);
    expect_ptr("strnstr-within-bound", s, strnstr(s, "345", 7), 3);
    expect_ptr("strnstr-at-boundary", s, strnstr(s, "789", 10), 7);
    expect_ptr("strnstr-past-boundary", s, strnstr(s, "789a", 10), -1);
    expect_ptr("strnstr-limit-zero", s, strnstr(s, "0", 0), -1);
    expect_ptr("strnstr-stops-at-nul", "abc", strnstr("abc", "c", 8), 2);
}

static void test_memmem_cases(void)
{
    const uchar binary[] = {
        0x00, 0x10, 0x20, 0x00, 0x10, 0x20, 0x30, 0x00,
    };
    const uchar n1[] = {0x00, 0x10, 0x20};
    const uchar n2[] = {0x10, 0x20, 0x30, 0x00};
    const uchar n3[] = {0x20, 0x30, 0x40};
    const char *text = "zzabczzabcabcd";

    expect_mem_ptr("memmem-empty-needle", binary,
                   memmem(binary, sizeof(binary), n1, 0), 0);
    expect_mem_ptr("memmem-binary-first", binary,
                   memmem(binary, sizeof(binary), n1, sizeof(n1)), 0);
    expect_mem_ptr("memmem-binary-middle", binary,
                   memmem(binary, sizeof(binary), n2, sizeof(n2)), 4);
    expect_mem_ptr("memmem-binary-miss", binary,
                   memmem(binary, sizeof(binary), n3, sizeof(n3)), -1);
    expect_mem_ptr("memmem-zero-haystack", binary,
                   memmem(binary, 0, n1, sizeof(n1)), -1);
    expect_mem_ptr("memmem-text-repeat", (const uchar *)text,
                   memmem(text, strlen(text), "abcabcd", 7), 7);
}

static void test_large_needle_fallback(void)
{
    char haystack[640];
    char needle[520];

    memset(haystack, 'a', sizeof(haystack));
    memset(needle, 'a', sizeof(needle));
    haystack[sizeof(haystack) - 1] = '\0';
    needle[sizeof(needle) - 1] = '\0';
    needle[sizeof(needle) - 2] = 'b';
    haystack[600] = 'b';

    expect_ptr("large-needle-linear-fallback", haystack,
               strstr(haystack, needle), 82);
}

void string_selftest_run_if_enabled(void)
{
    char value[16];

    if (cmdline_get_param("string_selftest", value, sizeof(value)) != 0 ||
        !cmdline_value_is_true(value))
        return;

    printf("string_selftest: start\n");
    test_strstr_cases();
    test_repeated_prefix_cases();
    test_strnstr_cases();
    test_memmem_cases();
    test_large_needle_fallback();
    printf("string_selftest: PASS\n");
}
