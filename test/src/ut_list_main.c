#include <ut_list.h>

// Simple create tests
test_context_t test_simple_create_prestates[] = {
    TEST_CASE_FULL(
        (),
        (),
        ()
    ),
    TEST_CASE_FULL(
        (1),
        (),
        (1)
    ),
    TEST_CASE_FULL(
        (1, 2),
        (),
        (1, 2)
    ),
    TEST_CASE_FULL(
        (1, 2, 3),
        (),
        (1, 2, 3)
    ),
};

static void test_simple_create(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);
    assert_true(compare_list_arr(head, expected, expected_size));
}

// Push tests
test_context_t test_push_empty_prestates[] = {
    TEST_CASE_FULL(
        (),
        (1),
        (1)
    ),
    TEST_CASE_FULL(
        (),
        (2, 1),
        (2, 1)
    ),
    TEST_CASE_FULL(
        (),
        (3, 2, 1),
        (3, 2, 1)
    ),
};

static void test_push_empty(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    size_t args_size = context->params.args_size;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);
    
    for (int i = 0; i < args_size; i++) {
        test_node_t *node = make_node(args[i]);
        assert_non_null(node);
        list_node_push(head, node, entry);
    }
    
    assert_true(compare_list_arr(head, expected, expected_size));
}

// Push back tests
static void test_push_back_empty_1(void **state) {
    (void)state;
    int input[] = {};
    int args[] = {1};
    int expected[] = {1};
    list_node_t *head = make_list(input, 0);
    assert_non_null(head);
    
    test_node_t *node = make_node(args[0]);
    assert_non_null(node);
    list_node_push_back(head, node, entry);
    
    assert_true(compare_list_arr(head, expected, 1));
    destroy_list(head);
}

static void test_push_back_empty_2(void **state) {
    (void)state;
    int input[] = {};
    int args[] = {2, 1}; // Note: Push back order is different
    int expected[] = {1, 2};
    list_node_t *head = make_list(input, 0);
    assert_non_null(head);
    
    for (int i = 0; i < 2; i++) {
        test_node_t *node = make_node(args[i]);
        assert_non_null(node);
        list_node_push_back(head, node, entry);
    }
    
    assert_true(compare_list_arr(head, expected, 2));
    destroy_list(head);
}

test_context_t test_push_back_empty_prestates[] = {
    TEST_CASE_FULL(
        (),
        (1),
        (1)
    ),
    TEST_CASE_FULL(
        (),
        (2, 1),
        (1, 2)
    ),
    TEST_CASE_FULL(
        (),
        (3, 2, 1),
        (1, 2, 3)
    ),
};

static void test_push_back_empty(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    size_t args_size = context->params.args_size;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);
    
    for (int i = 0; i < expected_size; i++) {
        test_node_t *node = make_node(args[i]);
        assert_non_null(node);
        list_node_push_back(head, node, entry);
    }
    
    assert_true(compare_list_arr(head, expected, expected_size));
}

// Pop tests
static void test_pop_empty(void **state) {
    (void)state;
    int input[] = {};
    int expected[] = {};
    list_node_t *head = make_list(input, 0);
    assert_non_null(head);
    
    // Try to pop from empty list - should return NULL
    test_node_t *node = list_node_pop(head, test_node_t, entry);
    assert_null(node);
    
    assert_true(compare_list_arr(head, expected, 0));
    destroy_list(head);
}

static void test_pop_1(void **state) {
    (void)state;
    int input[] = {1};
    int expected[] = {};
    list_node_t *head = make_list(input, 1);
    assert_non_null(head);
    
    test_node_t *node = list_node_pop(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 1);
    destroy_node(node);
    
    assert_true(compare_list_arr(head, expected, 0));
    destroy_list(head);
}

static void test_pop_2(void **state) {
    (void)state;
    int input[] = {1, 2};
    int expected[] = {1};
    list_node_t *head = make_list(input, 2);
    assert_non_null(head);
    
    test_node_t *node = list_node_pop(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 2);
    destroy_node(node);
    
    assert_true(compare_list_arr(head, expected, 1));
    destroy_list(head);
}

static void test_pop_3(void **state) {
    (void)state;
    int input[] = {1, 2, 3};
    int expected[] = {1, 2};
    list_node_t *head = make_list(input, 3);
    assert_non_null(head);
    
    test_node_t *node = list_node_pop(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 3);
    destroy_node(node);
    
    assert_true(compare_list_arr(head, expected, 2));
    destroy_list(head);
}

// Pop back tests
static void test_pop_back_empty(void **state) {
    (void)state;
    int input[] = {};
    int expected[] = {};
    list_node_t *head = make_list(input, 0);
    assert_non_null(head);
    
    // Try to pop_back from empty list - should return NULL
    test_node_t *node = list_node_pop_back(head, test_node_t, entry);
    assert_null(node);
    
    assert_true(compare_list_arr(head, expected, 0));
    destroy_list(head);
}

static void test_pop_back_1(void **state) {
    (void)state;
    int input[] = {1};
    int expected[] = {};
    list_node_t *head = make_list(input, 1);
    assert_non_null(head);
    
    test_node_t *node = list_node_pop_back(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 1);
    destroy_node(node);
    
    assert_true(compare_list_arr(head, expected, 0));
    destroy_list(head);
}

static void test_pop_back_2(void **state) {
    (void)state;
    int input[] = {1, 2};
    int expected[] = {2};
    list_node_t *head = make_list(input, 2);
    assert_non_null(head);
    
    test_node_t *node = list_node_pop_back(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 1);
    destroy_node(node);
    
    assert_true(compare_list_arr(head, expected, 1));
    destroy_list(head);
}

static void test_pop_back_3(void **state) {
    (void)state;
    int input[] = {1, 2, 3};
    int expected[] = {2, 3};
    list_node_t *head = make_list(input, 3);
    assert_non_null(head);
    
    test_node_t *node = list_node_pop_back(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 1);
    destroy_node(node);
    
    assert_true(compare_list_arr(head, expected, 2));
    destroy_list(head);
}

// Find first and detach tests
test_context_t test_find_first_detach_prestates[] = {
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1),
        (2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (8),
        (1, 2, 3, 4, 5, 6, 7)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 2),
        (3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 5),
        (2, 3, 4, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (7, 8),
        (1, 2, 3, 4, 5, 6)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 8),
        (1, 2, 3, 5, 6, 7)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (5, 10),
        (1, 2, 3, 4, 6, 7, 8)
    ),
};

static void test_find_first_detach(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);
    
    test_node_t *node = NULL;
    for (int i = 0; i < 2; i++) {
        node = list_find_first(head, test_node_t, entry, node, node->val == args[i]);
        if (node != NULL) {
            list_node_detach(node, entry);
            destroy_node(node);
            node = NULL;
        }
    }
    
    assert_true(compare_list_arr(head, expected, expected_size));
}

// Find last and detach tests
test_context_t test_find_last_detach_prestates[] = {
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1),
        (2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (8),
        (1, 2, 3, 4, 5, 6, 7)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 2),
        (3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 5),
        (2, 3, 4, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (7, 8),
        (1, 2, 3, 4, 5, 6)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 8),
        (1, 2, 3, 5, 6, 7)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (5, 10),
        (1, 2, 3, 4, 6, 7, 8)
    )
};

static void test_find_last_detach(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);
    
    test_node_t *node = NULL;
    for (int i = 0; i < 2; i++) {
        node = list_find_last(head, test_node_t, entry, node, node->val == args[i]);
        if (node != NULL) {
            list_node_detach(node, entry);
            destroy_node(node);
            node = NULL;
        }
    }
    
    assert_true(compare_list_arr(head, expected, expected_size));
}

// Find next and detach tests
test_context_t test_find_next_detach_prestates[] = {
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 1),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 2),
        (1, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 4),
        (1, 2, 3, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 8),
        (1, 2, 3, 4, 5, 6, 7)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 10),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (0, 4),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 4),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 5),
        (1, 2, 3, 4, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 7),
        (1, 2, 3, 4, 5, 6, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 8),
        (1, 2, 3, 4, 5, 6, 7)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 10),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (8, 8),
        (1, 2, 3, 4, 5, 6, 7, 8)
    )
};

static void test_find_next_detach(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);
    
    test_node_t *node = NULL;
    node = list_find_first(head, test_node_t, entry, node, node->val == args[0]);
    if (node != NULL) {
        node = list_find_next(head, node, entry, node, node->val == args[1]);
        if (node != NULL) {
            list_node_detach(node, entry);
            destroy_node(node);
        }
    }
    
    assert_true(compare_list_arr(head, expected, expected_size));
}

// Find prev and detach tests
test_context_t test_find_prev_detach_prestates[] = {
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (1, 1),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (8, 8),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (2, 1),
        (2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 1),
        (2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (8, 1),
        (2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (10, 1),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 0),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (4, 4),
        (1, 2, 3, 4, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (5, 4),
        (1, 2, 3, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (7, 4),
        (1, 2, 3, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (8, 4),
        (1, 2, 3, 5, 6, 7, 8)
    ),
    TEST_CASE_FULL(
        (1, 2, 3, 4, 5, 6, 7, 8),
        (10, 4),
        (1, 2, 3, 4, 5, 6, 7, 8)
    )
};

static void test_find_prev_detach(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);
    
    test_node_t *node = NULL;
    node = list_find_last(head, test_node_t, entry, node, node->val == args[0]);
    if (node != NULL) {
        node = list_find_prev(head, node, entry, node, node->val == args[1]);
        if (node != NULL) {
            list_node_detach(node, entry);
            destroy_node(node);
        }
    }
    
    assert_true(compare_list_arr(head, expected, expected_size));
}

static int list_test_setup(void **state) {
    test_context_t *context = *(test_context_t **)state;
    assert_non_null(context);
    const int *input = context->params.input;
    size_t input_size = context->params.input_size;
    
    context->head = make_list(input, input_size);
    assert_non_null(context->head);

    return 0;
}

static int list_test_teardown(void **state) {
    test_context_t *context = *(test_context_t **)state;
    assert_non_null(context);
    
    if (context->head != NULL) {
        destroy_list(context->head);
        context->head = NULL;
    }

    return 0;
}

int main(void) {
    const struct CMUnitTest tests[] = {
        // Simple create tests
        cmocka_unit_test_prestate_setup_teardown(test_simple_create, list_test_setup, list_test_teardown, &test_simple_create_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(test_simple_create, list_test_setup, list_test_teardown, &test_simple_create_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(test_simple_create, list_test_setup, list_test_teardown, &test_simple_create_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(test_simple_create, list_test_setup, list_test_teardown, &test_simple_create_prestates[3]),
        
        // Push tests
        cmocka_unit_test_prestate_setup_teardown(test_push_empty, list_test_setup, list_test_teardown, &test_push_empty_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(test_push_empty, list_test_setup, list_test_teardown, &test_push_empty_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(test_push_empty, list_test_setup, list_test_teardown, &test_push_empty_prestates[2]),
        
        // Push back tests
        cmocka_unit_test_prestate_setup_teardown(test_push_back_empty, list_test_setup, list_test_teardown, &test_push_back_empty_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(test_push_back_empty, list_test_setup, list_test_teardown, &test_push_back_empty_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(test_push_back_empty, list_test_setup, list_test_teardown, &test_push_back_empty_prestates[2]),
        
        // Pop tests
        cmocka_unit_test(test_pop_empty),
        cmocka_unit_test(test_pop_1),
        cmocka_unit_test(test_pop_2),
        cmocka_unit_test(test_pop_3),
        
        // Pop back tests
        cmocka_unit_test(test_pop_back_empty),
        cmocka_unit_test(test_pop_back_1),
        cmocka_unit_test(test_pop_back_2),
        cmocka_unit_test(test_pop_back_3),
        
        // Find first and detach tests
        cmocka_unit_test_prestate_setup_teardown(test_find_first_detach, list_test_setup, list_test_teardown, &test_find_first_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(test_find_first_detach, list_test_setup, list_test_teardown, &test_find_first_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(test_find_first_detach, list_test_setup, list_test_teardown, &test_find_first_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(test_find_first_detach, list_test_setup, list_test_teardown, &test_find_first_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(test_find_first_detach, list_test_setup, list_test_teardown, &test_find_first_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(test_find_first_detach, list_test_setup, list_test_teardown, &test_find_first_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(test_find_first_detach, list_test_setup, list_test_teardown, &test_find_first_detach_prestates[6]),
        
        // Find last and detach tests
        cmocka_unit_test_prestate_setup_teardown(test_find_last_detach, list_test_setup, list_test_teardown, &test_find_last_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(test_find_last_detach, list_test_setup, list_test_teardown, &test_find_last_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(test_find_last_detach, list_test_setup, list_test_teardown, &test_find_last_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(test_find_last_detach, list_test_setup, list_test_teardown, &test_find_last_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(test_find_last_detach, list_test_setup, list_test_teardown, &test_find_last_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(test_find_last_detach, list_test_setup, list_test_teardown, &test_find_last_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(test_find_last_detach, list_test_setup, list_test_teardown, &test_find_last_detach_prestates[6]),
        
        // Find next and detach tests
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[6]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[7]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[8]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[9]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[10]),
        cmocka_unit_test_prestate_setup_teardown(test_find_next_detach, list_test_setup, list_test_teardown, &test_find_next_detach_prestates[11]),
        
        // Find prev and detach tests
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[6]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[7]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[8]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[9]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[10]),
        cmocka_unit_test_prestate_setup_teardown(test_find_prev_detach, list_test_setup, list_test_teardown, &test_find_prev_detach_prestates[11]),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}
