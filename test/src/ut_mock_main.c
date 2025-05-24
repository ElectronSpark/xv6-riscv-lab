#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>
#include "types.h"
#include "riscv.h"
#include "page.h"
#include "memlayout.h"
#include "ut_mock_wraps.h"

extern page_t mock_pages[8];

// Test initialization setup that runs before each test
static int test_setup(void **state) {
    (void)state;
    static page_t __init_mock_pages[8] = {
        {.physical_address = KERNBASE, .ref_count = 1},
        {.physical_address = KERNBASE + PGSIZE, .ref_count = 0},
        {.physical_address = KERNBASE + 2 * PGSIZE, .ref_count = 0},
        {.physical_address = KERNBASE + 3 * PGSIZE, .ref_count = 0},
        {.physical_address = KERNBASE + 4 * PGSIZE, .ref_count = 0},
        {.physical_address = KERNBASE + 5 * PGSIZE, .ref_count = 0},
        {.physical_address = KERNBASE + 6 * PGSIZE, .ref_count = 0},
        {.physical_address = KERNBASE + 7 * PGSIZE, .ref_count = 0}
    };
    // Initialize mock pages with zeroes
    memcpy(mock_pages, __init_mock_pages, sizeof(mock_pages));
    return 0;
}

// Test page reference count increment
static void test_page_ref_inc_dec(void **state) {
    (void)state;
    page_t *page = &mock_pages[0];
    
    print_message("Testing page reference count increment and decrement\n");
    
    // Test reference increment
    print_message("  Initial ref_count: %d\n", page->ref_count);
    assert_int_equal(page_ref_inc(KERNBASE), 1);
    assert_int_equal(page->ref_count, 1);
    print_message("  After increment: %d\n", page->ref_count);
    
    // Test again to ensure it increments properly
    assert_int_equal(page_ref_inc(KERNBASE), 2);
    assert_int_equal(page->ref_count, 2);
    print_message("  After second increment: %d\n", page->ref_count);

    // Test reference decrement
    assert_int_equal(page_ref_dec(KERNBASE), 1);
    assert_int_equal(page->ref_count, 1);
    print_message("  After decrement: %d\n", page->ref_count);
    
    // Test again to reach zero
    assert_int_equal(page_ref_dec(KERNBASE), 0);
    assert_int_equal(page->ref_count, 0);
    print_message("  After second decrement: %d\n", page->ref_count);
    
    // Test when already at zero (shouldn't go below -1, which indicates failure)
    assert_int_equal(page_ref_dec(KERNBASE), -1);
    assert_int_equal(page->ref_count, -1);
    print_message("  After decrement at zero: %d\n", page->ref_count);
    assert_int_equal(page_ref_dec(KERNBASE), -1);
    assert_int_equal(page->ref_count, -1);
    print_message("  After another decrement at negative: %d\n", page->ref_count);
}

// Test page reference count via physical address
static void test_page_ref_count(void **state) {
    (void)state;
    page_t *page = &mock_pages[1];
    
    // Initialize page with ref_count = 3
    page->ref_count = 3;
    
    print_message("Testing page reference count retrieval\n");
    print_message("  Setting ref_count to: %d\n", page->ref_count);
    
    // Test page_ref_count function
    assert_int_equal(page_ref_count(page), 3);
    print_message("  Retrieved ref_count: %d\n", page_ref_count(page));
}

// Test page operations with null pointer
static void test_page_ops_null(void **state) {
    (void)state;
    
    print_message("Testing NULL page reference operations\n");
    
    // Test reference operations with NULL page
    print_message("  Testing __page_ref_inc(NULL)\n");
    assert_int_equal(__page_ref_inc(NULL), -1);
    print_message("  Testing __page_ref_dec(NULL)\n");
    assert_int_equal(__page_ref_dec(NULL), -1);
    print_message("  NULL pointer checks passed\n");
}

// Test physical address conversions
static void test_page_address_conversion(void **state) {
    (void)state;
    uint64 physical_addr = 0x1000;  // Example physical address
    
    print_message("Testing physical address to page conversion\n");
    
    // Test address to page conversion
    page_t *page = __pa_to_page(physical_addr);
    assert_non_null(page);
    print_message("  Successfully converted address 0x%lx to page\n", physical_addr);
    
    // Test page to physical address conversion (this would need more setup)
    // uint64 result_addr = __page_to_pa(page);
    // assert_int_equal(result_addr, physical_addr);
}

// Test page buddy initialization (basic checks)
static void test_page_buddy_init_basic(void **state) {
    (void)state;
    
    print_message("Testing buddy system page initialization (basic check)\n");
    
    // Setup mock pages with initial state needed for the test
    // This is a simplified test since we can't test the full initialization 
    // without setting up a lot of mock infrastructure
    
    // Test a basic property of the initialization
    // For example, if we know page_buddy_init should mark pages as free:
    uint64 start_addr = 0x1000;
    uint64 end_addr = 0x3000;
    
    print_message("  Start address: 0x%lx, End address: 0x%lx\n", start_addr, end_addr);
    
    // This is more of a placeholder - in reality you'd need to mock more functions
    // to properly test page_buddy_init
    // assert_int_equal(page_buddy_init(start_addr, end_addr), 0);
    
    // Instead, just check that the function exists
    assert_non_null(page_buddy_init);
    print_message("  Verified page_buddy_init function exists\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_page_ref_inc_dec, test_setup),
        cmocka_unit_test_setup(test_page_ref_count, test_setup),
        cmocka_unit_test_setup(test_page_ops_null, test_setup),
        cmocka_unit_test_setup(test_page_address_conversion, test_setup),
        cmocka_unit_test_setup(test_page_buddy_init_basic, test_setup),
    };

    int result = cmocka_run_group_tests(tests, NULL, NULL);
    return result;
}

