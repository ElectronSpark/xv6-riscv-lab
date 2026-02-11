#include <ut_list.h>

// Simple create tests
test_context_t test_simple_create_prestates[] = {
    TEST_CASE_FULL((), (), ()),
    TEST_CASE_FULL((1), (), (1)),
    TEST_CASE_FULL((1, 2), (), (1, 2)),
    TEST_CASE_FULL((1, 2, 3), (), (1, 2, 3)),
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
    TEST_CASE_FULL((), (1), (1)),
    TEST_CASE_FULL((), (2, 1), (2, 1)),
    TEST_CASE_FULL((), (3, 2, 1), (3, 2, 1)),
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

test_context_t test_push_back_empty_prestates[] = {
    TEST_CASE_FULL((), (1), (1)),
    TEST_CASE_FULL((), (2, 1), (1, 2)),
    TEST_CASE_FULL((), (3, 2, 1), (1, 2, 3)),
};

static void test_push_back_empty(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    size_t args_size = context->params.args_size; // Added back for clarity
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);

    // Safety check to prevent potential overread
    size_t loop_count = (args_size < expected_size) ? args_size : expected_size;

    for (int i = 0; i < loop_count; i++) {
        test_node_t *node = make_node(args[i]);
        assert_non_null(node);
        list_node_push_back(head, node, entry);
    }

    assert_true(compare_list_arr(head, expected, expected_size));
}

// Pop tests
static void test_pop_empty(void **state) {
    (void)state;
    // Use NULL instead of empty array to avoid stringop-overread
    list_node_t *head = make_list(NULL, 0);
    assert_non_null(head);

    // Try to pop from empty list - should return NULL
    test_node_t *node = list_node_pop(head, test_node_t, entry);
    assert_null(node);

    assert_true(LIST_IS_EMPTY(head));
    destroy_list(head);
}

static void test_pop_1(void **state) {
    (void)state;
    int input[] = {1};
    // Use NULL instead of empty array for expected
    list_node_t *head = make_list(input, 1);
    assert_non_null(head);

    test_node_t *node = list_node_pop(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 1);
    destroy_node(node);

    assert_true(LIST_IS_EMPTY(head));
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
    // Use NULL instead of empty array to avoid stringop-overread
    list_node_t *head = make_list(NULL, 0);
    assert_non_null(head);

    // Try to pop_back from empty list - should return NULL
    test_node_t *node = list_node_pop_back(head, test_node_t, entry);
    assert_null(node);

    assert_true(LIST_IS_EMPTY(head));
    destroy_list(head);
}

static void test_pop_back_1(void **state) {
    (void)state;
    int input[] = {1};
    // Use NULL instead of empty array for expected
    list_node_t *head = make_list(input, 1);
    assert_non_null(head);

    test_node_t *node = list_node_pop_back(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 1);
    destroy_node(node);

    assert_true(LIST_IS_EMPTY(head));
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
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1), (2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (8), (1, 2, 3, 4, 5, 6, 7)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 2), (3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 5), (2, 3, 4, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (7, 8), (1, 2, 3, 4, 5, 6)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 8), (1, 2, 3, 5, 6, 7)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (5, 10), (1, 2, 3, 4, 6, 7, 8)),
};

static void test_find_first_detach(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    size_t args_size = context->params.args_size;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);

    test_node_t *node = NULL;
    for (size_t i = 0; i < args_size; i++) {
        node = list_find_first(head, test_node_t, entry, node,
                               node->val == args[i]);
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
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1), (2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (8), (1, 2, 3, 4, 5, 6, 7)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 2), (3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 5), (2, 3, 4, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (7, 8), (1, 2, 3, 4, 5, 6)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 8), (1, 2, 3, 5, 6, 7)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (5, 10), (1, 2, 3, 4, 6, 7, 8))};

static void test_find_last_detach(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    size_t args_size = context->params.args_size;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);

    test_node_t *node = NULL;
    for (size_t i = 0; i < args_size; i++) {
        node = list_find_last(head, test_node_t, entry, node,
                              node->val == args[i]);
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
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 1), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 2), (1, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 4), (1, 2, 3, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 8), (1, 2, 3, 4, 5, 6, 7)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 10), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (0, 4), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 4), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 5), (1, 2, 3, 4, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 7), (1, 2, 3, 4, 5, 6, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 8), (1, 2, 3, 4, 5, 6, 7)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 10), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (8, 8), (1, 2, 3, 4, 5, 6, 7, 8))};

static void test_find_next_detach(void **state) {
    test_context_t *context = *(test_context_t **)state;
    const int *args = context->params.args;
    const int *expected = context->params.expected;
    size_t expected_size = context->params.expected_size;
    list_node_t *head = context->head;
    assert_non_null(head);

    test_node_t *node = NULL;
    node =
        list_find_first(head, test_node_t, entry, node, node->val == args[0]);
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
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (1, 1), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (8, 8), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (2, 1), (2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 1), (2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (8, 1), (2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (10, 1), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 0), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (4, 4), (1, 2, 3, 4, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (5, 4), (1, 2, 3, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (7, 4), (1, 2, 3, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (8, 4), (1, 2, 3, 5, 6, 7, 8)),
    TEST_CASE_FULL((1, 2, 3, 4, 5, 6, 7, 8), (10, 4),
                   (1, 2, 3, 4, 5, 6, 7, 8))};

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

/**
 * Test creating an empty list
 */
static void test_create_empty_list(void **state) {
    (void)state;
    list_node_t *head = make_list(NULL, 0);
    assert_non_null(head);
    assert_true(LIST_IS_EMPTY(head)); // Use the macro from list.h
    destroy_list(head);
}

/**
 * Test creating a list with one element
 */
static void test_create_one_element_list(void **state) {
    (void)state;
    int input[] = {42};
    list_node_t *head = make_list(input, 1);
    assert_non_null(head);
    assert_false(LIST_IS_EMPTY(head)); // Use the macro from list.h

    // Verify the element using the macro from list.h
    test_node_t *node = LIST_FIRST_NODE(head, test_node_t, entry);
    assert_non_null(node);
    assert_int_equal(node->val, 42);

    destroy_list(head);
}

/**
 * Test pushing elements to an empty list
 */
static void test_push_elements(void **state) {
    (void)state;
    list_node_t *head = make_list(NULL, 0);
    assert_non_null(head);

    // Push several elements
    for (int i = 5; i >= 1; i--) {
        test_node_t *node = make_node(i);
        assert_non_null(node);
        list_node_push(head, node, entry);
    }
    // Verify the list contains the elements in reverse order
    int expected[] = {5, 4, 3, 2, 1};
    assert_true(compare_list_arr(head, expected, 5));

    destroy_list(head);
}

/**
 * Test pushing elements to the back of an empty list
 */
static void test_push_back_elements(void **state) {
    (void)state;
    list_node_t *head = make_list(NULL, 0);
    assert_non_null(head);

    // Push several elements to the back
    for (int i = 5; i >= 1; i--) {
        test_node_t *node = make_node(i);
        assert_non_null(node);
        list_node_push_back(head, node, entry);
    }

    // Verify the list contains the elements in original order
    int expected[] = {1, 2, 3, 4, 5};
    assert_true(compare_list_arr(head, expected, 5));

    destroy_list(head);
}

/**
 * Test popping elements from a list
 */
static void test_pop_elements(void **state) {
    (void)state;
    int input[] = {5, 4, 3, 2, 1};
    list_node_t *head = make_list(input, 5);
    assert_non_null(head);

    // Pop the elements one by one and verify
    for (int i = 1; i <= 5; i++) {
        test_node_t *node = list_node_pop(head, test_node_t, entry);
        assert_non_null(node);
        assert_int_equal(node->val, i);
        destroy_node(node);
    }

    // Verify the list is now empty
    assert_true(LIST_IS_EMPTY(head));

    // Try to pop from empty list
    test_node_t *node = list_node_pop(head, test_node_t, entry);
    assert_null(node);

    destroy_list(head);
}

/**
 * Test popping elements from the back of a list
 */
static void test_pop_back_elements(void **state) {
    (void)state;
    int input[] = {5, 4, 3, 2, 1};
    list_node_t *head = make_list(input, 5);
    assert_non_null(head);

    // Pop the elements from back one by one and verify
    for (int i = 5; i >= 1; i--) {
        test_node_t *node = list_node_pop_back(head, test_node_t, entry);
        assert_non_null(node);
        assert_int_equal(node->val, i);
        destroy_node(node);
    }

    // Verify the list is now empty
    assert_true(LIST_IS_EMPTY(head));

    // Try to pop_back from empty list
    test_node_t *node = list_node_pop_back(head, test_node_t, entry);
    assert_null(node);

    destroy_list(head);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        // Basic list creation tests
        cmocka_unit_test(test_create_empty_list),
        cmocka_unit_test(test_create_one_element_list),

        // Simple create tests
        cmocka_unit_test_prestate_setup_teardown(
            test_simple_create, list_test_setup, list_test_teardown,
            &test_simple_create_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(
            test_simple_create, list_test_setup, list_test_teardown,
            &test_simple_create_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(
            test_simple_create, list_test_setup, list_test_teardown,
            &test_simple_create_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(
            test_simple_create, list_test_setup, list_test_teardown,
            &test_simple_create_prestates[3]),

        // Push tests
        cmocka_unit_test(test_push_elements),
        cmocka_unit_test_prestate_setup_teardown(
            test_push_empty, list_test_setup, list_test_teardown,
            &test_push_empty_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(
            test_push_empty, list_test_setup, list_test_teardown,
            &test_push_empty_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(
            test_push_empty, list_test_setup, list_test_teardown,
            &test_push_empty_prestates[2]),

        // Push back tests
        cmocka_unit_test(test_push_back_elements),
        cmocka_unit_test_prestate_setup_teardown(
            test_push_back_empty, list_test_setup, list_test_teardown,
            &test_push_back_empty_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(
            test_push_back_empty, list_test_setup, list_test_teardown,
            &test_push_back_empty_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(
            test_push_back_empty, list_test_setup, list_test_teardown,
            &test_push_back_empty_prestates[2]),

        // Pop tests
        cmocka_unit_test(test_pop_elements),
        cmocka_unit_test(test_pop_empty),
        cmocka_unit_test(test_pop_1),
        cmocka_unit_test(test_pop_2),
        cmocka_unit_test(test_pop_3),

        // Pop back tests
        cmocka_unit_test(test_pop_back_elements),
        cmocka_unit_test(test_pop_back_empty),
        cmocka_unit_test(test_pop_back_1),
        cmocka_unit_test(test_pop_back_2),
        cmocka_unit_test(test_pop_back_3),

        // Find first and detach tests
        cmocka_unit_test_prestate_setup_teardown(
            test_find_first_detach, list_test_setup, list_test_teardown,
            &test_find_first_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_first_detach, list_test_setup, list_test_teardown,
            &test_find_first_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_first_detach, list_test_setup, list_test_teardown,
            &test_find_first_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_first_detach, list_test_setup, list_test_teardown,
            &test_find_first_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_first_detach, list_test_setup, list_test_teardown,
            &test_find_first_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_first_detach, list_test_setup, list_test_teardown,
            &test_find_first_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_first_detach, list_test_setup, list_test_teardown,
            &test_find_first_detach_prestates[6]),

        // Find last and detach tests
        cmocka_unit_test_prestate_setup_teardown(
            test_find_last_detach, list_test_setup, list_test_teardown,
            &test_find_last_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_last_detach, list_test_setup, list_test_teardown,
            &test_find_last_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_last_detach, list_test_setup, list_test_teardown,
            &test_find_last_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_last_detach, list_test_setup, list_test_teardown,
            &test_find_last_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_last_detach, list_test_setup, list_test_teardown,
            &test_find_last_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_last_detach, list_test_setup, list_test_teardown,
            &test_find_last_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_last_detach, list_test_setup, list_test_teardown,
            &test_find_last_detach_prestates[6]),

        // Find next and detach tests
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[6]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[7]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[8]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[9]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[10]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_next_detach, list_test_setup, list_test_teardown,
            &test_find_next_detach_prestates[11]),

        // Find prev and detach tests
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[0]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[1]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[2]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[3]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[4]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[5]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[6]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[7]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[8]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[9]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[10]),
        cmocka_unit_test_prestate_setup_teardown(
            test_find_prev_detach, list_test_setup, list_test_teardown,
            &test_find_prev_detach_prestates[11]),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
