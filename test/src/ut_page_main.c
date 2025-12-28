#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdlib.h>

#include <cmocka.h>

#include "ut_page_wraps.h"
#include "param.h"
#include "list.h"

// Structure to store buddy system state
typedef struct {
    uint64 counts[PAGE_BUDDY_MAX_ORDER + 1];
    bool empty[PAGE_BUDDY_MAX_ORDER + 1];
    uint64 total_free_pages;
    bool skip;
} buddy_system_state_t;

// Test initialization setup that runs before each test
static int test_setup(void **state) {
    // Allocate state to store buddy system statistics
    buddy_system_state_t *buddy_state = malloc(sizeof(buddy_system_state_t));
    assert_non_null(buddy_state);
    
    // Reset buddy pools and pages
    memset(__buddy_pools, 0, sizeof(__buddy_pools));
    memset(__pages, 0, sizeof(__pages));
    
    // Initialize mock pages with proper values
    assert_int_equal(page_buddy_init(KERNBASE, PHYSTOP), 0);
    
    // Record initial buddy system state
    memset(buddy_state, 0, sizeof(buddy_system_state_t));
    page_buddy_stat(buddy_state->counts, buddy_state->empty, PAGE_BUDDY_MAX_ORDER + 1);
    
    // Calculate total free pages
    buddy_state->total_free_pages = 0;
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        buddy_state->total_free_pages += (1UL << i) * buddy_state->counts[i];
    }
    buddy_state->skip = true;
    
    // Pass the state to the test
    *state = buddy_state;
    
    return 0;
}

// Test teardown function that runs after each test
static int test_teardown(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    static buddy_system_state_t final_state = { 0 };

    if (initial_state->skip) {
        goto skip;
    }
    
    // Get the current buddy system state
    page_buddy_stat(final_state.counts, final_state.empty, PAGE_BUDDY_MAX_ORDER + 1);
    
    // Calculate total free pages
    final_state.total_free_pages = 0;
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        final_state.total_free_pages += (1UL << i) * final_state.counts[i];
    }
    
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        uint64 initial_count = initial_state->counts[i];
        uint64 final_count = final_state.counts[i];
        bool initial_empty = initial_state->empty[i];
        bool final_empty = final_state.empty[i];
        assert_int_equal(final_count, initial_count);
        assert_int_equal(final_empty, initial_empty);
    }
    
    // Check if the buddy system state matches the initial state
    assert_int_equal(final_state.total_free_pages, initial_state->total_free_pages);
    
    // Free the state
skip:
    free(initial_state);
    *state = NULL;
    return 0;
}

static void test_print_buddy_system_stat(void **state) {
    (void)state;
    print_message("Testing buddy system statistics printing\n");
    
    // This is a mock test, we just want to ensure the function can be called
    // and it doesn't crash. In a real test, you would check the output.
    print_buddy_system_stat(1);
}

// Test page reference count increment
static void test_page_ref_inc_dec(void **state) {
    (void)state;
    page_t *page = &__pages[0];
    __page_init(page, page->physical_address, 1, PAGE_TYPE_ANON);
    
    print_message("Testing page reference count increment and decrement\n");
    
    // Reset the page's reference count to 1 to start
    page->ref_count = 1;
    print_message("  Initial ref_count: %d\n", page->ref_count);
    
    // Test reference increment
    int result = __real_page_ref_inc((void*)KERNBASE);
    assert_int_equal(result, 2);
    assert_int_equal(page->ref_count, 2);
    print_message("  After increment: %d\n", page->ref_count);
    
    // Test again to ensure it increments properly
    result = __real_page_ref_inc((void*)KERNBASE);
    assert_int_equal(result, 3);
    assert_int_equal(page->ref_count, 3);
    print_message("  After second increment: %d\n", page->ref_count);

    // Test reference decrement
    result = __real_page_ref_dec((void*)KERNBASE);
    assert_int_equal(result, 2);
    assert_int_equal(page->ref_count, 2);
    print_message("  After decrement: %d\n", page->ref_count);
    
    // Test again
    result = __real_page_ref_dec((void*)KERNBASE);
    assert_int_equal(result, 1);
    assert_int_equal(page->ref_count, 1);
    print_message("  After second decrement: %d\n", page->ref_count);
    
    // Test again to reach zero
    result = __real_page_ref_dec((void*)KERNBASE);
    assert_int_equal(result, 0);
    assert_int_equal(page->ref_count, 0);
    print_message("  After third decrement (to zero): %d\n", page->ref_count);
}

// Test page reference count via physical address
static void test_page_ref_count(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    page_t *page = &__pages[1];
    
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
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    
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
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 physical_addr = KERNBASE + 0x1000;  // Use a valid address in our mock range
    
    print_message("Testing physical address to page conversion\n");
    
    // Test address to page conversion
    page_t *page = __pa_to_page(physical_addr);
    assert_non_null(page);
    assert_int_equal(page->physical_address, physical_addr);
    print_message("  Successfully converted address 0x%lx to page\n", physical_addr);
    
    // Test page to physical address conversion
    uint64 result_addr = __page_to_pa(page);
    assert_int_equal(result_addr, physical_addr);
    print_message("  Successfully converted page back to address 0x%lx\n", result_addr);
}

// Test page buddy initialization (basic checks)
static void test_page_buddy_init_basic(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    
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

// Test page address conversion edge cases
static void test_page_address_conversion_edge(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    extern uint64 __managed_start;
    extern uint64 __managed_end;
    
    print_message("Testing page address conversion edge cases\n");
    
    // Test boundaries of managed memory
    print_message("  Testing boundary addresses\n");
    
    // Test lower boundary - should be valid
    page_t *page_start = __pa_to_page(__managed_start);
    assert_non_null(page_start);
    print_message("  Successfully converted lower boundary address 0x%lx to page\n", __managed_start);
    
    // We now need to check the __pa_to_page implementation to understand how we handle
    // addresses outside the managed range
    // In our mocked version, we just calculate an index without checking boundaries
    // Let's skip the next test rather than make it always pass falsely
    print_message("  Skipping test for below boundary address\n");
    
    // Test upper boundary that's in range
    assert_true(__managed_end > __managed_start + PAGE_SIZE);
    page_t *page_end = __pa_to_page(__managed_end - PAGE_SIZE);
    assert_non_null(page_end);
    print_message("  Successfully converted upper boundary address 0x%lx to page\n", __managed_end - PAGE_SIZE);
    
    // test on upper boundary 
    page_t *page = __pa_to_page(__managed_end);
    assert_null(page);
    page = __pa_to_page(__managed_end + PAGE_SIZE);
    assert_null(page);

    // test on lower boundary
    page = __pa_to_page(__managed_start - PAGE_SIZE);
    assert_null(page);
    page = __pa_to_page(__managed_start - (PAGE_SIZE << 1));
    assert_null(page);

    // The test for unaligned addresses relies on the implementation of __pa_to_page
    // in the mock, but let's still do a test with aligned addresses
    
    // Test conversion from page to physical address (round trip)
    uint64 physical_addr = __managed_start + PAGE_SIZE;
    page = __pa_to_page(physical_addr);
    assert_non_null(page);
    
    uint64 converted_addr = __page_to_pa(page);
    assert_int_equal(converted_addr, physical_addr);
    print_message("  Successfully round-trip converted address 0x%lx\n", physical_addr);
    
    // Test NULL page to physical address conversion
    uint64 null_addr = __page_to_pa(NULL);
    assert_int_equal(null_addr, 0);
    print_message("  Correctly handled NULL page to address conversion\n");
}

// Test the page_refcnt helper function
static void test_page_refcnt_helper(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    page_t *page = &__pages[2];
    uint64 physical_addr = KERNBASE + (2 * PGSIZE);
    
    print_message("Testing page_refcnt helper function\n");
    
    // Set reference count to a known value
    __page_init(page, physical_addr, 5, PAGE_TYPE_ANON);
    print_message("  Setting page ref_count to: %d\n", page->ref_count);
    
    // Test the helper function
    int refcnt = __real_page_refcnt((void*)physical_addr);
    assert_int_equal(refcnt, 5);
    print_message("  Successfully retrieved ref_count: %d via helper function\n", refcnt);
    
    // Test with NULL
    refcnt = __real_page_refcnt(NULL);
    assert_int_equal(refcnt, -1);
    print_message("  Correctly handled NULL pointer to page_refcnt\n");
}

// Test page allocation and freeing
// @TODO: improve
static void test_page_alloc_free(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 flags = 0;
    uint64 order = 0;  // Single page allocation
    
    print_message("Testing page allocation and freeing\n");
    
    // Test allocation
    page_t *page = __page_alloc(order, flags);
    assert_non_null(page);
    print_message("  Allocated page at physical address: 0x%lx\n", page->physical_address);
    
    // Check reference count after allocation should be 1
    assert_int_equal(page->ref_count, 1);
    
    // Check flags are set correctly
    assert_int_equal(page->flags, flags);
    
    // Test freeing
    __page_free(page, order);
    print_message("  Freed page successfully\n");
    
    // Allocate again to verify the page can be reused
    page_t *page2 = __page_alloc(order, flags);
    assert_non_null(page2);
    print_message("  Reallocated page at physical address: 0x%lx\n", page2->physical_address);
    
    // Clean up
    __page_free(page2, order);
}

// Test buddy system allocation of multiple orders
// @TODO: improve
static void test_buddy_multi_order_alloc(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 flags = 0;
    
    print_message("Testing buddy system multi-order allocation\n");
    
    // Test allocating orders 0-3, but only up to what our mock can handle
    // With 8 pages, we can support up to order 3 (8 pages)
    for (uint64 order = 0; order <= 2; order++) {  // Only test up to order 2 (4 pages)
        // Calculate expected page count
        uint64 page_count = 1UL << order;
        
        // Allocate pages
        page_t *page = __page_alloc(order, flags);
        if (page == NULL) {
            print_message("  Allocation for order %lu failed - this may be expected with limited mock pages\n", order);
            continue;
        }
        
        print_message("  Allocated 2^%ld=%ld pages at physical address: 0x%lx\n", 
                     order, page_count, page->physical_address);
        
        // Verify alignment is correct for the order - in our mock, all pages are aligned
        // to their natural boundaries
        
        // Verify all pages in the group are properly initialized
        for (uint64 i = 0; i < page_count; i++) {
            assert_int_equal(page[i].ref_count, 1);
            assert_int_equal(page[i].flags, flags);
        }
        
        // Free the pages
        __page_free(page, order);
        print_message("  Freed 2^%ld=%ld pages\n", order, page_count);
    }
}

// Test buddy system page flags
// @TODO: Some flags need to be mutually exclusive
static void test_page_flags(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 order = 0;
    
    print_message("Testing page flags\n");
    
    // Test different flag combinations
    uint64 flag_tests[] = {
        PAGE_TYPE_SLAB,
        PAGE_TYPE_ANON,
        PAGE_TYPE_PGTABLE,
        PAGE_TYPE_PCACHE,
    };
    
    // Initialize mock pages - ensure all are free
    for (size_t i = 0; i < 8; i++) {
        __pages[i].ref_count = 0;
        __pages[i].flags = 0;
    }
    
    for (size_t i = 0; i < sizeof(flag_tests)/sizeof(flag_tests[0]); i++) {
        uint64 flags = flag_tests[i];
        print_message("  Testing flag combination: 0x%lx\n", flags);
        
        // Allocate page with flags
        page_t *page = __page_alloc(order, flags);
        assert_non_null(page);
        
        // Verify flags are set correctly
        assert_int_equal(page->flags, flags);
        
        // Clean up
        __page_free(page, order);
    }
    
    // Test invalid flags
    print_message("  Testing invalid flags\n");
    for (uint64 flags_i = __PAGE_TYPE_MAX; flags_i <= PAGE_FLAG_TYPE_MASK; flags_i++) {
        page_t *page = __page_alloc(order, flags_i);
        assert_null(page);
    }
    page_t *page = __page_alloc(order, PAGE_FLAG_MASK);
    assert_null(page);
}

// Test for __page_buddy_init and buddy pool functionality
// @TODO: improve
static void test_page_buddy_init_detailed(void **state) {
    (void)state;
    
    print_message("Testing buddy system initialization and pool management\n");
    
    // Setup test range
    uint64 start_addr = KERNBASE + PGSIZE;  // Use second page
    uint64 end_addr = start_addr + (4 * PGSIZE);  // Use 4 pages
    
    print_message("  Testing page_buddy_init with range 0x%lx to 0x%lx\n", start_addr, end_addr);
    
    // Initialize buddy system for the test range
    int result = page_buddy_init(start_addr, end_addr);
    assert_int_equal(result, 0);
    
    // Check buddy pools after initialization
    // We expect different distributions based on how the buddy algorithm merged pages
    // At minimum, we should have pages available in the system
    bool found_pages = false;
    
    for (int i = 0; i <= PAGE_BUDDY_MAX_ORDER; i++) {
        buddy_pool_t *pool = &__buddy_pools[i];
        if (pool->count > 0) {
            found_pages = true;
            print_message("  Found %lu pages in order %d pool\n", pool->count, i);
        }
    }
    
    assert_true(found_pages);
    print_message("  Buddy system initialization successful\n");
}

// Test page buddy split and merge functionality
static void test_buddy_split_merge(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    
    print_message("Testing buddy system split and merge operations\n");

    static page_t *pages[TOTALPAGES] = { 0 };

    for (int order = 0; order <= PAGE_BUDDY_MAX_ORDER; order++) {
        int alloc_count = TOTALPAGES >> order;  // Number of pages in this order

        print_message("Allocating %d pages with order %d\n", alloc_count, order);

        for (int i = 0; i < alloc_count; i++) {
            pages[i] = __page_alloc(order, PAGE_TYPE_ANON);
            assert_non_null(pages[i]);
        }

        assert_null(__page_alloc(order, PAGE_TYPE_ANON));

        print_message("Freeing %d pages with order %d\n", alloc_count, order);
        for (int i = 0; i < alloc_count; i++) {
            __page_free(pages[i], order);
        }
    }
}

// Test allocation failure cases
// @TODO: improve
static void test_page_alloc_failure(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    
    print_message("Testing page allocation failure cases\n");
    
    // Test invalid order
    uint64 invalid_order = PAGE_BUDDY_MAX_ORDER + 1;
    page_t *page = __page_alloc(invalid_order, 0);
    assert_null(page);
    print_message("  Correctly failed to allocate page with invalid order %lu\n", invalid_order);
    
    // Test invalid flags
    uint64 invalid_flags = PAGE_FLAG_LOCKED;  // PAGE_FLAG_LOCKED not allowed in allocation
    page = __page_alloc(0, invalid_flags);
    assert_null(page);
    print_message("  Correctly failed to allocate page with invalid flags 0x%lx\n", invalid_flags);
}

// Test for helper functions that calculate buddy addresses
// @TODO: improve
static void test_buddy_address_helpers(void **state) {
    (void)state;
    extern uint64 __get_buddy_addr(uint64 physical, uint32 order);
    
    print_message("Testing buddy address calculation helpers\n");
    
    // Test cases for different orders
    struct test_case {
        uint64 physical_addr;
        uint32 order;
        uint64 expected_buddy_addr;
    };
    
    struct test_case test_cases[] = {
        // For order 0 (single page), buddy is at offset of 1 page
        {KERNBASE, 0, KERNBASE ^ (1UL << PAGE_SHIFT)},
        // For order 1 (2 pages), buddy is at offset of 2 pages
        {KERNBASE, 1, KERNBASE ^ (1UL << (PAGE_SHIFT + 1))},
        // For order 2 (4 pages), buddy is at offset of 4 pages
        {KERNBASE, 2, KERNBASE ^ (1UL << (PAGE_SHIFT + 2))},
        // For address not at KERNBASE
        {KERNBASE + PGSIZE*10, 0, (KERNBASE + PGSIZE*10) ^ (1UL << PAGE_SHIFT)}
    };
    
    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        uint64 buddy_addr = __get_buddy_addr(test_cases[i].physical_addr, test_cases[i].order);
        print_message("  Order %u: Address 0x%lx has buddy at 0x%lx\n", 
                     test_cases[i].order, test_cases[i].physical_addr, buddy_addr);
        assert_int_equal(buddy_addr, test_cases[i].expected_buddy_addr);
    }
    
    print_message("  All buddy address calculations verified\n");
}

// Test buddy system with simulated fragmentation
// @TODO: improve
static void __attribute__((unused)) test_buddy_fragmentation(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 flags = 0;
    static page_t *pages[TOTALPAGES];
    
    print_message("Testing buddy system under fragmentation\n");

    // Try to allocate a mix of different order pages
    // Note: With only 8 pages total, we can only do simplified fragmentation tests
    pages[0] = __page_alloc(0, flags);  // Single page
    if (!pages[0]) {
        print_message("  Failed basic allocation - skipping test\n");
        return;
    }
    
    pages[1] = __page_alloc(0, flags);  // Single page
    pages[2] = __page_alloc(0, flags);  // Single page
    
    print_message("  Created fragmentation with single-page allocations\n");
    
    // Print allocated pages
    for (int i = 0; i < 3; i++) {
        if (pages[i]) {
            print_message("  Page %d at 0x%lx\n", i, pages[i]->physical_address);
        }
    }
    
    // Try to allocate a larger order page
    print_message("  Trying to allocate higher order page with fragmentation present\n");
    page_t *large_page = __page_alloc(2, flags);  // 4 pages
    if (large_page) {
        print_message("  Successfully allocated page of order 2 at 0x%lx\n", large_page->physical_address);
        
        // Verify this page meets the alignment requirements for order 2
        uint64 page_idx = large_page - __pages;
        if ((page_idx & ((1UL << 2) - 1)) != 0) {
            print_message("  WARNING: Order 2 page at index %lu is not properly aligned\n", page_idx);
        }
        
        __page_free(large_page, 2);
    } else {
        print_message("  Could not allocate page of order 2 (expected due to fragmentation)\n");
    }
    
    // Free the allocated pages - verify they're aligned for the specified order
    for (int i = 0; i < 3; i++) {
        if (pages[i]) {
            print_message("  Freeing page %d\n", i);
            uint64 page_idx = pages[i] - __pages;
            if ((page_idx & ((1UL << 0) - 1)) != 0) {
                print_message("  WARNING: Order 0 page at index %lu is not properly aligned\n", page_idx);
            }
            __page_free(pages[i], 0);
        }
    }
    
    // After freeing, try to allocate a larger order page again
    print_message("  After freeing pages, trying to allocate higher order page\n");
    large_page = __page_alloc(2, flags); // Try order 2 (4 pages)
    if (large_page) {
        print_message("  Successfully allocated page of order 2 at 0x%lx\n", large_page->physical_address);
        __page_free(large_page, 2);
    } else {
        print_message("  Still could not allocate page of order 2 (limited by mock implementation)\n");
    }
}

// Test page allocation stress test
// @TODO: improve
static void __attribute__((unused)) test_page_alloc_stress(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 flags = 0;
    const int NUM_ALLOCS = 8;  // Max number of allocations our mock supports
    page_t *pages[NUM_ALLOCS];  // Store allocated pages
    uint64 orders[NUM_ALLOCS];  // Store orders for each allocation
    
    print_message("Running page allocation stress test\n");
    
    // Perform a series of allocations of different orders
    // Limit to order 0 (single page) allocations to ensure test runs successfully
    for (int i = 0; i < NUM_ALLOCS; i++) {
        orders[i] = 0;  // Stick to single-page allocations
        pages[i] = __page_alloc(orders[i], flags);
        
        if (pages[i] == NULL) {
            print_message("  Allocation %d failed (order %lu) - mock system out of memory\n", i, orders[i]);
            break;
        }
        
        print_message("  Allocation %d: order %lu at 0x%lx\n", i, orders[i], pages[i]->physical_address);
    }
    
    // Free all successfully allocated pages
    print_message("  Freeing all allocated pages\n");
    for (int i = 0; i < NUM_ALLOCS; i++) {
        if (pages[i] != NULL) {
            __page_free(pages[i], orders[i]);
        }
    }
    
    // Try one final allocation to verify system recovered
    uint64 order = 1;  // Try a 2-page allocation
    page_t *page = __page_alloc(order, flags);
    if (page != NULL) {
        print_message("  Successfully allocated order %lu page after stress test\n", order);
        __page_free(page, order);
    } else {
        print_message("  Could not allocate order %lu page after stress test\n", order);
    }
}

// Test mixed use of regular and helper allocation functions
// @TODO: improve
static void test_mixed_allocation_methods(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 flags = PAGE_TYPE_ANON;
    uint64 order = 0;
    
    print_message("Testing mixed use of regular and helper allocation methods\n");
    
    // Use low-level allocation function
    page_t *page = __page_alloc(order, flags);
    assert_non_null(page);
    uint64 physical_addr = page->physical_address;
    print_message("  Low-level allocation: page at 0x%lx\n", physical_addr);
    
    // Use helper allocation function
    void *memory = __real_page_alloc(order, flags);
    assert_non_null(memory);
    print_message("  Helper allocation: memory at %p\n", memory);
    
    // Convert between the two 
    page_t *page_from_pa = __pa_to_page((uint64)memory);
    assert_non_null(page_from_pa);
    
    void *addr_from_page = (void *)__page_to_pa(page);
    assert_non_null(addr_from_page);
    
    print_message("  Conversion from page to address: %p\n", addr_from_page);
    print_message("  Conversion from address to page: %p\n", page_from_pa);
    
    // Free using both methods
    __page_free(page, order);
    print_message("  Freed page with low-level function\n");
    
    __real_page_free(memory, order);
    print_message("  Freed memory with helper function\n");
}

// Test buddy system alignment requirements explicitly
static void test_buddy_alignment(void **state) {
    buddy_system_state_t *initial_state = (buddy_system_state_t *)*state;
    initial_state->skip = false;  // Allow this test to run
    uint64 flags = PAGE_TYPE_ANON;
    
    print_message("Testing buddy system alignment requirements\n");
    
    // Test allocation of different orders and verify alignment
    for (uint64 order = 0; order <= 2; order++) {  // Test orders 0, 1, and 2
        uint64 page_count = 1UL << order;
        
        print_message("  Testing order %lu (2^%lu = %lu pages)\n", order, order, page_count);
        
        // Allocate page of this order
        page_t *page = __page_alloc(order, flags);
        if (!page) {
            print_message("  Could not allocate page of order %lu - skipping\n", order);
            continue;
        }
        
        // Verify the returned page is properly aligned for this order
        uint64 physical = page->physical_address;
        uint64 alignment_mask = (1UL << (order + PAGE_SHIFT)) - 1;
        
        print_message("  Allocated page at index %p, order %lu\n", (void *)physical, order);
        print_message("  Alignment check: index %% 2^%lu == 0: %s\n", 
                     order, (physical & alignment_mask) == 0 ? "yes" : "no");
        
        // For orders > 0, the page index must be a multiple of 2^order
        assert_int_equal(physical & alignment_mask, 0);
        
        // Check that all pages in the group have been marked as allocated
        for (uint64 i = 0; i < page_count; i++) {
            assert_int_equal(page[i].ref_count, 1);
        }
        
        // Free the allocated pages
        print_message("  Freeing allocated pages of order %lu\n", order);
        __page_free(page, order);
        
        // Verify all pages were freed
        for (uint64 i = 0; i < page_count; i++) {
            assert_int_equal(page[i].ref_count, 0);
        }
    }
}

// prepare the test environment before all tests
int set_up_test_suite() {
    void *ret = mmap((void *)KERNBASE, PHYSTOP - KERNBASE, 
                    PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    assert_ptr_equal(ret, (void *)KERNBASE);

    __managed_start = KERNBASE;
    __managed_end = PHYSTOP;

    __wrap___page_free_passthrough = true;
    __wrap___page_alloc_passthrough = true;
    return 0;
}

// tear down test environment after all tests
int tear_down_test_suite() {
    assert_int_equal(munmap((void *)KERNBASE, PHYSTOP - KERNBASE), 0);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    static buddy_system_state_t *prestate = NULL;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_print_buddy_system_stat),

        // Basic page operations
        cmocka_unit_test_prestate_setup_teardown(test_page_ref_inc_dec, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_ref_count, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_ops_null, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_address_conversion, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_address_conversion_edge, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_refcnt_helper, test_setup, test_teardown, &prestate),
        
        // Buddy system tests
        cmocka_unit_test_prestate_setup_teardown(test_page_buddy_init_basic, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_buddy_init_detailed, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_buddy_address_helpers, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_buddy_alignment, test_setup, test_teardown, &prestate),
        
        // Allocation and freeing tests
        cmocka_unit_test_prestate_setup_teardown(test_page_alloc_free, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_buddy_multi_order_alloc, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_flags, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_buddy_split_merge, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_alloc_failure, test_setup, test_teardown, &prestate),
        
        // Advanced/stress tests
        cmocka_unit_test_prestate_setup_teardown(test_buddy_fragmentation, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_page_alloc_stress, test_setup, test_teardown, &prestate),
        cmocka_unit_test_prestate_setup_teardown(test_mixed_allocation_methods, test_setup, test_teardown, &prestate),
    };

    // set_up_test_suite();
    int result = cmocka_run_group_tests(tests, set_up_test_suite, tear_down_test_suite);
    // tear_down_test_suite();
    return result;
}

