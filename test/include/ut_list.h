#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "common.h"
#include "list.h"
#include <stdlib.h>

/* --- MACROS FOR AUTOMATIC ARRAY LENGTH CALCULATION --- */


// Macro that creates a compound literal array and calculates its length
#define MAKE_ARRAY(...) (const int[]){ __VA_ARGS__ }
// Basic macro to get the size of an array
#define ARRAY_SIZE(...) (sizeof(MAKE_ARRAY __VA_ARGS__) / sizeof((MAKE_ARRAY __VA_ARGS__)[0]))

#define TEST_CASE_PARAMS(input_elements, args_elements, expected_elements) \
    { \
        .input = MAKE_ARRAY input_elements, \
        .input_size = ARRAY_SIZE(input_elements), \
        .args = MAKE_ARRAY args_elements, \
        .args_size = ARRAY_SIZE(args_elements), \
        .expected = MAKE_ARRAY expected_elements, \
        .expected_size = ARRAY_SIZE(expected_elements) \
    }

/**
 * This flexible macro allows you to specify the three arrays using separate parameter lists.
 * 
 * Usage example:
 * TEST_CASE_FULL(
 *     (1, 2, 3, 4),   // Input array elements
 *     (5, 6),         // Args array elements
 *     (7, 8, 9)       // Expected array elements
 * )
 */
#define TEST_CASE_FULL(input_elements, args_elements, expected_elements) \
    { \
        .head = NULL, \
        .params = TEST_CASE_PARAMS(input_elements, args_elements, expected_elements) \
    }

/**
 * Test parameters structure for parameterized tests
 */
typedef struct {
    const int *input;
    int input_size;
    const int *args;
    int args_size;
    const int *expected;
    int expected_size;
} test_params_t;

/**
 * Test context structure to be passed between test functions via state parameter
 */
typedef struct {
    list_node_t *head;       // List head
    test_params_t params; // Test parameters
} test_context_t;

// Helper node and list functions (copied from original)
typedef struct test_node {
    list_node_t entry;
    int val;
} test_node_t;

static inline test_node_t *make_node(int val) {
    test_node_t *ret = malloc(sizeof(test_node_t));
    if (val == -9999) {
        return NULL; // Simulate allocation failure
    }
    if (ret != NULL) {
        ret->val = val;
    }
    list_entry_init(&ret->entry);
    return ret;
}

static inline void destroy_node(test_node_t *node) {
    if (node != NULL) {
        free(node);
    }
}

static inline void destroy_list(list_node_t *head) {
    test_node_t *pos, *tmp;
    list_foreach_node_safe(head, pos, tmp, entry) {
        destroy_node(pos);
    }
    free(head);
}

static inline list_node_t *make_list(const int arr[], int arr_length) {
    if (arr_length < 0) {
        return NULL;
    }
    
    list_node_t *head = malloc(sizeof(list_node_t));
    if (head == NULL) {
        return NULL;
    }
    
    list_entry_init(head);
    
    // Only iterate if arr_length > 0 and arr is not NULL
    if (arr_length > 0 && arr != NULL) {
        for (int i = 0; i < arr_length; i++) {
            test_node_t *node = make_node(arr[i]);
            if (node == NULL) {
                destroy_list(head);
                return NULL;
            }
            list_node_push(head, node, entry);
        }
    }
    
    return head;
}

static inline bool compare_list_arr(list_node_t *head, const int arr[], int arr_length) {
    if (head == NULL) {
        return false;
    }
    
    // Handle special cases
    if (arr_length == 0 || arr == NULL) {
        // Make sure the list is empty too
        if (LIST_IS_EMPTY(head)) {
            return true;
        }
        return false;
    }
    
    // Normal case, compare elements
    int idx = 0;
    test_node_t *pos, *tmp;
    list_foreach_node_safe(head, pos, tmp, entry) {
        if (idx >= arr_length) {
            return false;
        }
        if (pos->val != arr[idx]) {
            return false;
        }
        idx++;
    }
    return idx == arr_length;
}

// Print functions for debug purposes
static inline void print_list(list_node_t *head) {
    test_node_t *pos, *tmp;
    int cnt = 0;
    printf("[");
    list_foreach_node_safe(head, pos, tmp, entry) {
        printf(" %d,", pos->val);
        cnt++;
    }
    if (cnt > 0) printf("\b ");
    printf("]\n");
}

static inline void print_array(const int arr[], int arr_length) {
    int cnt = 0;
    printf("[");
    for (int i = 0; i < arr_length; i++) {
        printf(" %d,", arr[i]);
        cnt++;
    }
    if (cnt > 0) printf("\b ");
    printf("]\n");
}
