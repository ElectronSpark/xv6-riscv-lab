#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
#include <cmocka.h>
#include "../kernel/hlist.h"

// Helper function to create a hash list with dynamic memory allocation
static inline hlist_t *mock_hlist_create(uint64 bucket_cnt) {
    size_t size = sizeof(hlist_t) + bucket_cnt * sizeof(hlist_bucket_t);
    hlist_t *hlist = (hlist_t *)malloc(size);
    if (hlist) {
        memset(hlist, 0, size);
    }
    return hlist;
}

// Define a test node structure to use in our hash list
typedef struct test_node {
    hlist_entry_t entry;  // Include a hash list entry
    int key;              // Key for hashing/lookup
    char value[32];       // Value associated with the key
} test_node_t;

// Hash function for test_node
static ht_hash_t test_node_hash(void *node) {
    test_node_t *n = (test_node_t *)node;
    return hlist_hash_uint64(n->key);
}

// Get entry function for test_node
static hlist_entry_t *test_node_get_entry(void *node) {
    test_node_t *n = (test_node_t *)node;
    return &n->entry;
}

// Get node function for test_node
static void *test_node_get_node(hlist_entry_t *entry) {
    hlist_entry_t *e = (hlist_entry_t *)entry;
    return container_of(e, test_node_t, entry);
}

// Compare nodes function for test_node
static int test_node_cmp(hlist_t *hlist, void *node1, void *node2) {
    test_node_t *n1 = (test_node_t *)node1;
    test_node_t *n2 = (test_node_t *)node2;
    return n1->key - n2->key;
}

// Initialize a hash list functions structure
static hlist_func_t test_hlist_func = {
    .hash = test_node_hash,
    .get_entry = test_node_get_entry,
    .get_node = test_node_get_node,
    .cmp_node = test_node_cmp
};

// Create a test node with given key and value
static test_node_t *create_test_node(int key, const char *value) {
    test_node_t *node = (test_node_t *)malloc(sizeof(test_node_t));
    if (!node) return NULL;
    
    node->key = key;
    strncpy(node->value, value, sizeof(node->value) - 1);
    node->value[sizeof(node->value) - 1] = '\0';
    hlist_entry_init(&node->entry);
    
    return node;
}

// Free a test node
static void free_test_node(test_node_t *node) {
    if (node) free(node);
}

// Test fixture data structure
typedef struct test_fixture {
    hlist_t *hlist;
    test_node_t *nodes[5]; // Pre-created nodes for testing
} test_fixture_t;

// Setup function for tests
static int setup(void **state) {
    test_fixture_t *fixture = malloc(sizeof(test_fixture_t));
    if (!fixture) return -1;
    
    // Initialize fixture with NULL values
    memset(fixture, 0, sizeof(test_fixture_t));
    
    // Create a hash list
    fixture->hlist = mock_hlist_create(10);
    if (!fixture->hlist) {
        free(fixture);
        return -1;
    }
    
    // Initialize hash list
    if (hlist_init(fixture->hlist, 10, &test_hlist_func) != 0) {
        free(fixture->hlist);
        free(fixture);
        return -1;
    }
    
    *state = fixture;
    return 0;
}

// Teardown function for tests
static int teardown(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    if (fixture) {
        // Free any allocated nodes
        for (int i = 0; i < 5; i++) {
            if (fixture->nodes[i]) {
                free_test_node(fixture->nodes[i]);
                fixture->nodes[i] = NULL;
            }
        }
        
        // Free the hash list
        if (fixture->hlist) {
            free(fixture->hlist);
        }
        
        free(fixture);
    }
    
    return 0;
}

/* Test Cases */

// Test hlist initialization with NULL hlist
static void test_hlist_init_null_hlist(void **state) {
    (void)state; // Unused
    
    int result = hlist_init(NULL, 10, &test_hlist_func);
    assert_int_equal(result, -1);
}

// Test hlist initialization with NULL functions
static void test_hlist_init_null_functions(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    int result = hlist_init(fixture->hlist, 10, NULL);
    assert_int_equal(result, -1);
}

// Test hlist initialization with zero bucket count
static void test_hlist_init_zero_bucket_count(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    int result = hlist_init(fixture->hlist, 0, &test_hlist_func);
    assert_int_equal(result, -1);
}

// Test hlist initialization with valid parameters
static void test_hlist_init_valid(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    int result = hlist_init(fixture->hlist, 10, &test_hlist_func);
    assert_int_equal(result, 0);
}

// Test hlist_put and hlist_get with new nodes
static void test_hlist_put_and_get(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    // Create test nodes
    fixture->nodes[0] = create_test_node(1, "Node 1");
    fixture->nodes[1] = create_test_node(2, "Node 2");
    fixture->nodes[2] = create_test_node(3, "Node 3");
    
    // Insert nodes into hash list
    void *result1 = hlist_put(fixture->hlist, fixture->nodes[0]);
    void *result2 = hlist_put(fixture->hlist, fixture->nodes[1]);
    void *result3 = hlist_put(fixture->hlist, fixture->nodes[2]);
    
    // Check that new nodes were inserted correctly (returns NULL)
    assert_null(result1);
    assert_null(result2);
    assert_null(result3);
    
    // Create dummy nodes for lookup
    test_node_t dummy1 = { .key = 1 };
    test_node_t dummy2 = { .key = 2 };
    test_node_t dummy3 = { .key = 3 };
    test_node_t dummy4 = { .key = 4 }; // Not in the list
    
    // Get nodes and verify
    test_node_t *get1 = hlist_get(fixture->hlist, &dummy1);
    test_node_t *get2 = hlist_get(fixture->hlist, &dummy2);
    test_node_t *get3 = hlist_get(fixture->hlist, &dummy3);
    test_node_t *get4 = hlist_get(fixture->hlist, &dummy4);
    
    assert_ptr_equal(get1, fixture->nodes[0]);
    assert_string_equal(get1->value, "Node 1");
    
    assert_ptr_equal(get2, fixture->nodes[1]);
    assert_string_equal(get2->value, "Node 2");
    
    assert_ptr_equal(get3, fixture->nodes[2]);
    assert_string_equal(get3->value, "Node 3");
    
    // Check that non-existent node returns NULL
    assert_null(get4);
}

// Test replacing an existing node with hlist_put
static void test_hlist_put_replace(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    // Create and insert an initial node
    fixture->nodes[0] = create_test_node(1, "Node 1");
    hlist_put(fixture->hlist, fixture->nodes[0]);
    
    // Create a replacement node with the same key
    test_node_t *replacement = create_test_node(1, "Node 1 New");
    
    // Replace the existing node
    void *old_node = hlist_put(fixture->hlist, replacement);
    
    // Verify the old node was returned and the replacement is in the list
    assert_ptr_equal(old_node, fixture->nodes[0]);
    
    // Create dummy node for lookup
    test_node_t dummy = { .key = 1 };
    test_node_t *get_node = hlist_get(fixture->hlist, &dummy);
    
    assert_ptr_equal(get_node, replacement);
    assert_string_equal(get_node->value, "Node 1 New");
    
    // Free the old node
    free_test_node(old_node);
    
    // Set the replacement in the fixture for later cleanup
    fixture->nodes[0] = replacement;
}

// Test hlist_pop with NULL key (pop from empty list)
static void test_hlist_pop_empty(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    // Try to pop from empty list
    void *node = hlist_pop(fixture->hlist, NULL);
    assert_null(node);
}

// Test hlist_pop with specific key
static void test_hlist_pop_specific_key(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    // Create and insert test nodes
    fixture->nodes[0] = create_test_node(1, "Node 1");
    fixture->nodes[1] = create_test_node(2, "Node 2");
    fixture->nodes[2] = create_test_node(3, "Node 3");
    
    hlist_put(fixture->hlist, fixture->nodes[0]);
    hlist_put(fixture->hlist, fixture->nodes[1]);
    hlist_put(fixture->hlist, fixture->nodes[2]);
    
    // Pop the node with key 2
    test_node_t dummy = { .key = 2 };
    test_node_t *popped = hlist_pop(fixture->hlist, &dummy);
    
    // Verify the correct node was popped
    assert_ptr_equal(popped, fixture->nodes[1]);
    
    // Try to get the popped node - should return NULL
    test_node_t *get = hlist_get(fixture->hlist, &dummy);
    assert_null(get);
    
    // Free the popped node manually since it's no longer in the fixture
    free_test_node(popped);
    fixture->nodes[1] = NULL;
}

// Test hlist_pop with NULL key (arbitrary node removal)
static void test_hlist_pop_null_key(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    // Create and insert test nodes
    fixture->nodes[0] = create_test_node(1, "Node 1");
    fixture->nodes[1] = create_test_node(2, "Node 2");
    
    hlist_put(fixture->hlist, fixture->nodes[0]);
    hlist_put(fixture->hlist, fixture->nodes[1]);
    
    // Pop with NULL key - should return arbitrary node
    test_node_t *popped = hlist_pop(fixture->hlist, NULL);
    
    // Verify a node was returned (could be either node)
    assert_non_null(popped);
    assert_true(popped == fixture->nodes[0] || popped == fixture->nodes[1]);
    
    // Mark which node was popped in the fixture
    if (popped == fixture->nodes[0]) {
        fixture->nodes[0] = NULL;
    } else {
        fixture->nodes[1] = NULL;
    }
    
    // Free the popped node
    free_test_node(popped);
}

// Test hlist_node_in_list
static void test_hlist_node_in_list(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    // Create test nodes
    fixture->nodes[0] = create_test_node(1, "Node 1");
    fixture->nodes[1] = create_test_node(2, "Node 2");
    
    // Check that nodes are not in list initially
    assert_false(hlist_node_in_list(fixture->hlist, fixture->nodes[0]));
    assert_false(hlist_node_in_list(fixture->hlist, fixture->nodes[1]));
    
    // Insert one node
    hlist_put(fixture->hlist, fixture->nodes[0]);
    
    // Check that inserted node is in list but other is not
    assert_true(hlist_node_in_list(fixture->hlist, fixture->nodes[0]));
    assert_false(hlist_node_in_list(fixture->hlist, fixture->nodes[1]));
}

// Test hlist_get_node_hash
static void test_hlist_get_node_hash(void **state) {
    test_fixture_t *fixture = (test_fixture_t *)*state;
    
    // Create a test node
    test_node_t *node = create_test_node(42, "Hash Test Node");
    fixture->nodes[0] = node;
    
    // Get hash value
    ht_hash_t hash = hlist_get_node_hash(fixture->hlist, node);
    ht_hash_t expected_hash = test_node_hash(node);
    
    // Hash should be the key value (42) as per our hash function
    assert_int_equal(hash, expected_hash);
    
    // Test with NULL node
    hash = hlist_get_node_hash(fixture->hlist, NULL);
    assert_int_equal(hash, 0);
}

/* Main function */
int main(void) {
    const struct CMUnitTest tests[] = {
        // Initialization tests
        cmocka_unit_test(test_hlist_init_null_hlist),
        cmocka_unit_test_setup_teardown(test_hlist_init_null_functions, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hlist_init_zero_bucket_count, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hlist_init_valid, setup, teardown),
        
        // Put and Get tests
        cmocka_unit_test_setup_teardown(test_hlist_put_and_get, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hlist_put_replace, setup, teardown),
        
        // Pop tests
        cmocka_unit_test_setup_teardown(test_hlist_pop_empty, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hlist_pop_specific_key, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hlist_pop_null_key, setup, teardown),
        
        // Other tests
        cmocka_unit_test_setup_teardown(test_hlist_node_in_list, setup, teardown),
        cmocka_unit_test_setup_teardown(test_hlist_get_node_hash, setup, teardown),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}
