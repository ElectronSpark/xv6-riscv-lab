#include "common.h"
#include "list.h"
#include <stdlib.h>

typedef struct test_node {
    list_node_t     entry;
    int             val;
} test_node_t; 

#define P99_PROTECT(...) __VA_ARGS__ 
typedef void (*action_fun_t)(list_node_t *, int, int[]);

typedef struct test_case_struct {
    const char * case_name;
    const char * func_name;
    int test_length;
    int * test_case;
    int arguments_length;
    int * arguments;
    int expected_result_length;
    int * expected_result;
    action_fun_t actions;
} test_case_t;

#define __TEST_INPUT_NAME(name) __test_case_##name##_input
#define __TEST_EXPECTED_NAME(name) __test_case_##name##_expected
#define __TEST_ARUMENTS_NAME(name) __test_case_##name##_test_args

#define CASE_T_INIT(name, test_function)      \
{                                                                                   \
    .func_name = #test_function,                                                    \
    .case_name = #name,                                                             \
    .actions = test_function,                                                       \
    .test_case = __TEST_INPUT_NAME(name),                                           \
    .test_length = sizeof(typeof(__TEST_INPUT_NAME(name))) / sizeof(int),      \
    .expected_result = __TEST_EXPECTED_NAME(name),                                        \
    .expected_result_length = sizeof(typeof(__TEST_EXPECTED_NAME(name))) / sizeof(int),   \
    .arguments = __TEST_ARUMENTS_NAME(name),                                        \
    .arguments_length = sizeof(typeof(__TEST_ARUMENTS_NAME(name))) / sizeof(int)         \
}

#define ADD_CASE_INPUT(name, ...)                                                   \
    int __TEST_INPUT_NAME(name)[] = __VA_ARGS__

#define ADD_CASE_EXPECTED(name, ...)                                            \
    int __TEST_EXPECTED_NAME(name)[] = __VA_ARGS__

#define ADD_CASE_ARUMENTS(name, ...)                                            \
    int __TEST_ARUMENTS_NAME(name)[] = __VA_ARGS__

#define CASE_DATA(name, test_case_input, expected, test_args)                    \
ADD_CASE_INPUT(name, test_case_input);                                          \
ADD_CASE_EXPECTED(name, expected);                                              \
ADD_CASE_ARUMENTS(name, test_args)

test_node_t *make_node(int val) {
    test_node_t *ret = malloc(sizeof(test_node_t));
    if (ret != NULL) {
        ret->val = val;
    }
    list_entry_init(&ret->entry);
    return ret;
}

void destroy_node(test_node_t *node) {
    if (node != NULL) {
        free(node);
    }
}

void destroy_list(list_node_t *head) {
    test_node_t *pos, *tmp;
    list_foreach_node_safe(head, pos, tmp, entry) {
        destroy_node(pos);
    }
    free(head);
}

list_node_t *make_list(const int arr[], int arr_length) {
    list_node_t *head = malloc(sizeof(list_node_t));
    list_entry_init(head);
    for (int i = 0; i < arr_length; i++) {
        test_node_t *node = make_node(arr[i]);
        if (node == NULL) {
            destroy_list(head);
            return NULL;
        }
        list_node_push(head, node, entry);
    }
    return head;
}

// compare a list and an array
// if identical, return true. otherwise return false.
bool compare_list_arr(list_node_t *head, const int arr[], int arr_length) {
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

    return true;
}

void print_list(list_node_t *head) {
    test_node_t *pos, *tmp;
    int cnt = 0;
    printf("[");
    list_foreach_node_safe(head, pos, tmp, entry) {
        printf(" %d,", pos->val);
        cnt++;
    }
    if (cnt > 0)    printf("\b ");
    printf("]\n");
}

void print_array(const int arr[], int arr_length) {
    int cnt = 0;
    printf("[");
    for (int i = 0; i < arr_length; i++) {
        printf(" %d,", arr[i]);
        cnt++;
    }
    if (cnt > 0)    printf("\b ");
    printf("]\n");
}

ADD_CASE_INPUT(simple_create_1, {});
ADD_CASE_EXPECTED(simple_create_1, {});
ADD_CASE_ARUMENTS(simple_create_1, {});

ADD_CASE_INPUT(simple_create_2, {1});
ADD_CASE_EXPECTED(simple_create_2, {1});
ADD_CASE_ARUMENTS(simple_create_2, {});

ADD_CASE_INPUT(simple_create_3, {1, 2});
ADD_CASE_EXPECTED(simple_create_3, {1, 2});
ADD_CASE_ARUMENTS(simple_create_3, {});

ADD_CASE_INPUT(simple_create_4, {1, 2, 3});
ADD_CASE_EXPECTED(simple_create_4, {1, 2, 3});
ADD_CASE_ARUMENTS(simple_create_4, {});

void test_push(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;

    for (int i = 0; i < argc; i++) {
        node = make_node(argv[i]);
        if (node == NULL) {
            FAILURE();
            printf("failed to create node\n");
            return;
        }
        list_node_push(head, node, entry);
    }
}
ADD_CASE_INPUT(test_push_empty_1, {});
ADD_CASE_EXPECTED(test_push_empty_1, {1});
ADD_CASE_ARUMENTS(test_push_empty_1, {1});

ADD_CASE_INPUT(test_push_empty_2, {});
ADD_CASE_EXPECTED(test_push_empty_2, {1, 2});
ADD_CASE_ARUMENTS(test_push_empty_2, {1, 2});

ADD_CASE_INPUT(test_push_empty_3, {});
ADD_CASE_EXPECTED(test_push_empty_3, {1, 2, 3});
ADD_CASE_ARUMENTS(test_push_empty_3, {1, 2, 3});

void test_push_back(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;

    for (int i = 0; i < argc; i++) {
        node = make_node(argv[i]);
        if (node == NULL) {
            FAILURE();
            printf("failed to create node\n");
            return;
        }
        list_node_push_back(head, node, entry);
    }
}
ADD_CASE_INPUT(test_push_back_empty_1, {});
ADD_CASE_EXPECTED(test_push_back_empty_1, {1});
ADD_CASE_ARUMENTS(test_push_back_empty_1, {1});

ADD_CASE_INPUT(test_push_back_empty_2, {});
ADD_CASE_EXPECTED(test_push_back_empty_2, {1, 2});
ADD_CASE_ARUMENTS(test_push_back_empty_2, {2, 1});

ADD_CASE_INPUT(test_push_back_empty_3, {});
ADD_CASE_EXPECTED(test_push_back_empty_3, {1, 2, 3});
ADD_CASE_ARUMENTS(test_push_back_empty_3, {3, 2, 1});

void test_pop(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;

    if (argc != 2) {
        FAILURE();
        return;
    }
    int pop_nodes = argv[0];
    bool expect_failure = argv[1];

    for (int i = 0; i < pop_nodes; i++) {
        node = list_node_pop(head, test_node_t, entry);
        if ((node == NULL && !expect_failure) || (node != NULL && expect_failure)) {
            FAILURE();
            printf("failed to create node\n");
            return;
        } else {
            SUCCESS();
        }
        destroy_node(node);
    }
}
ADD_CASE_INPUT(test_pop_empty, {});
ADD_CASE_EXPECTED(test_pop_empty, {1});
ADD_CASE_ARUMENTS(test_pop_empty, {1, true});

ADD_CASE_INPUT(test_pop_1, {1});
ADD_CASE_EXPECTED(test_pop_1, {});
ADD_CASE_ARUMENTS(test_pop_1, {1, false});

ADD_CASE_INPUT(test_pop_2, {1, 2});
ADD_CASE_EXPECTED(test_pop_2, {1});
ADD_CASE_ARUMENTS(test_pop_2, {1, false});

ADD_CASE_INPUT(test_pop_3, {1, 2, 3});
ADD_CASE_EXPECTED(test_pop_3, {1, 2});
ADD_CASE_ARUMENTS(test_pop_3, {1, false});

void test_pop_back(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;

    if (argc != 2) {
        FAILURE();
        return;
    }
    int pop_nodes = argv[0];
    bool expect_failure = argv[1];

    for (int i = 0; i < pop_nodes; i++) {
        node = list_node_pop_back(head, test_node_t, entry);
        if ((node == NULL && !expect_failure) || (node != NULL && expect_failure)) {
            FAILURE();
            printf("failed to create node\n");
            return;
        } else {
            SUCCESS();
        }
        destroy_node(node);
    }
}
ADD_CASE_INPUT(test_pop_back_empty, {});
ADD_CASE_EXPECTED(test_pop_back_empty, {1});
ADD_CASE_ARUMENTS(test_pop_back_empty, {1, true});

ADD_CASE_INPUT(test_pop_back_1, {1});
ADD_CASE_EXPECTED(test_pop_back_1, {});
ADD_CASE_ARUMENTS(test_pop_back_1, {1, false});

ADD_CASE_INPUT(test_pop_back_2, {1, 2});
ADD_CASE_EXPECTED(test_pop_back_2, {2});
ADD_CASE_ARUMENTS(test_pop_back_2, {1, false});

ADD_CASE_INPUT(test_pop_back_3, {1, 2, 3});
ADD_CASE_EXPECTED(test_pop_back_3, {2, 3});
ADD_CASE_ARUMENTS(test_pop_back_3, {1, false});

void test_find_first_detach(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;
    if (argc == 0) {
        return;
    }
    
    for (int i = 0; i < argc; i++) {
        node = list_find_first(head, test_node_t, entry, node, node->val == argv[i]);
        if (node != NULL) {
            list_node_detach(node, entry);
            destroy_node(node);
        }
    }
}

ADD_CASE_INPUT(test_find_first_detach_1, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_first_detach_1, {2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_first_detach_1, {1});

ADD_CASE_INPUT(test_find_first_detach_2, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_first_detach_2, {1, 2, 3, 4, 5, 6, 7});
ADD_CASE_ARUMENTS(test_find_first_detach_2, {8});

ADD_CASE_INPUT(test_find_first_detach_3, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_first_detach_3, {3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_first_detach_3, {1, 2});

ADD_CASE_INPUT(test_find_first_detach_4, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_first_detach_4, {2, 3, 4, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_first_detach_4, {1, 5});

ADD_CASE_INPUT(test_find_first_detach_5, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_first_detach_5, {1, 2, 3, 4, 5, 6});
ADD_CASE_ARUMENTS(test_find_first_detach_5, {7, 8});

ADD_CASE_INPUT(test_find_first_detach_6, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_first_detach_6, {1, 2, 3, 5, 6, 7});
ADD_CASE_ARUMENTS(test_find_first_detach_6, {4, 8});

ADD_CASE_INPUT(test_find_first_detach_7, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_first_detach_7, {1, 2, 3, 4, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_first_detach_7, {5, 10});

void test_find_last_detach(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;
    if (argc == 0) {
        return;
    }
    
    for (int i = 0; i < argc; i++) {
        node = list_find_last(head, test_node_t, entry, node, node->val == argv[i]);
        if (node != NULL) {
            list_node_detach(node, entry);
            destroy_node(node);
        }
    }
}

ADD_CASE_INPUT(test_find_last_detach_1, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_last_detach_1, {2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_last_detach_1, {1});

ADD_CASE_INPUT(test_find_last_detach_2, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_last_detach_2, {1, 2, 3, 4, 5, 6, 7});
ADD_CASE_ARUMENTS(test_find_last_detach_2, {8});

ADD_CASE_INPUT(test_find_last_detach_3, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_last_detach_3, {3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_last_detach_3, {1, 2});

ADD_CASE_INPUT(test_find_last_detach_4, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_last_detach_4, {2, 3, 4, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_last_detach_4, {1, 5});

ADD_CASE_INPUT(test_find_last_detach_5, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_last_detach_5, {1, 2, 3, 4, 5, 6});
ADD_CASE_ARUMENTS(test_find_last_detach_5, {7, 8});

ADD_CASE_INPUT(test_find_last_detach_6, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_last_detach_6, {1, 2, 3, 5, 6, 7});
ADD_CASE_ARUMENTS(test_find_last_detach_6, {4, 8});

ADD_CASE_INPUT(test_find_last_detach_7, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_last_detach_7, {1, 2, 3, 4, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_last_detach_7, {5, 10});

void test_find_next_detach(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;
    if (argc != 2) {
        return;
    }

    node = list_find_first(head, test_node_t, entry, node, node->val == argv[0]);
    if (node == NULL) {
        return;
    }
    node = list_find_next(head, node, entry, node, node->val == argv[1]);
    if (node != NULL) {
        list_node_detach(node, entry);
        destroy_node(node);
    }
}

ADD_CASE_INPUT(test_find_next_detach_1, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_1, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_1, {1, 1});

ADD_CASE_INPUT(test_find_next_detach_2, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_2, {1, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_2, {1, 2});

ADD_CASE_INPUT(test_find_next_detach_3, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_3, {1, 2, 3, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_3, {1, 4});

ADD_CASE_INPUT(test_find_next_detach_4, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_4, {1, 2, 3, 4, 5, 6, 7});
ADD_CASE_ARUMENTS(test_find_next_detach_4, {1, 8});

ADD_CASE_INPUT(test_find_next_detach_5, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_5, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_5, {1, 10});

ADD_CASE_INPUT(test_find_next_detach_6, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_6, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_6, {0, 4});

ADD_CASE_INPUT(test_find_next_detach_7, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_7, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_7, {4, 4});

ADD_CASE_INPUT(test_find_next_detach_8, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_8, {1, 2, 3, 4, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_8, {4, 5});

ADD_CASE_INPUT(test_find_next_detach_9, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_9, {1, 2, 3, 4, 5, 6, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_9, {4, 7});

ADD_CASE_INPUT(test_find_next_detach_10, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_10, {1, 2, 3, 4, 5, 6, 7});
ADD_CASE_ARUMENTS(test_find_next_detach_10, {4, 8});

ADD_CASE_INPUT(test_find_next_detach_11, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_next_detach_11, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_next_detach_11, {4, 10});

void test_find_prev_detach(list_node_t *head, int argc, int argv[]) {
    test_node_t *node = NULL;
    if (argc != 2) {
        return;
    }

    node = list_find_last(head, test_node_t, entry, node, node->val == argv[0]);
    if (node == NULL) {
        return;
    }
    node = list_find_prev(head, node, entry, node, node->val == argv[1]);
    if (node != NULL) {
        list_node_detach(node, entry);
        destroy_node(node);
    }
}

ADD_CASE_INPUT(test_find_prev_detach_1, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_1, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_1, {1, 1});

ADD_CASE_INPUT(test_find_prev_detach_2, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_2, {2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_2, {2, 1});

ADD_CASE_INPUT(test_find_prev_detach_3, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_3, {2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_3, {4, 1});

ADD_CASE_INPUT(test_find_prev_detach_4, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_4, {2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_4, {8, 1});

ADD_CASE_INPUT(test_find_prev_detach_5, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_5, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_5, {10, 1});

ADD_CASE_INPUT(test_find_prev_detach_6, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_6, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_6, {4, 0});

ADD_CASE_INPUT(test_find_prev_detach_7, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_7, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_7, {4, 4});

ADD_CASE_INPUT(test_find_prev_detach_8, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_8, {1, 2, 3, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_8, {5, 4});

ADD_CASE_INPUT(test_find_prev_detach_9, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_9, {1, 2, 3, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_9, {7, 4});

ADD_CASE_INPUT(test_find_prev_detach_10, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_10, {1, 2, 3, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_10, {8, 4});

ADD_CASE_INPUT(test_find_prev_detach_11, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_11, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_11, {8, 8});

ADD_CASE_INPUT(test_find_prev_detach_12, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_EXPECTED(test_find_prev_detach_12, {1, 2, 3, 4, 5, 6, 7, 8});
ADD_CASE_ARUMENTS(test_find_prev_detach_12, {10, 4});

static test_case_t test_cases[] = {
        CASE_T_INIT(simple_create_1, NULL),
        CASE_T_INIT(simple_create_2, NULL),
        CASE_T_INIT(simple_create_3, NULL),
        CASE_T_INIT(simple_create_4, NULL),
        CASE_T_INIT(test_push_empty_1, test_push),
        CASE_T_INIT(test_push_empty_2, test_push),
        CASE_T_INIT(test_push_empty_3, test_push),
        CASE_T_INIT(test_push_back_empty_1, test_push_back),
        CASE_T_INIT(test_push_back_empty_2, test_push_back),
        CASE_T_INIT(test_push_back_empty_3, test_push_back),
        CASE_T_INIT(test_pop_empty, test_pop),
        CASE_T_INIT(test_pop_1, test_pop),
        CASE_T_INIT(test_pop_2, test_pop),
        CASE_T_INIT(test_pop_3, test_pop),
        CASE_T_INIT(test_pop_back_empty, test_pop_back),
        CASE_T_INIT(test_pop_back_1, test_pop_back),
        CASE_T_INIT(test_pop_back_2, test_pop_back),
        CASE_T_INIT(test_pop_back_3, test_pop_back),
        CASE_T_INIT(test_find_first_detach_1, test_find_first_detach),
        CASE_T_INIT(test_find_first_detach_2, test_find_first_detach),
        CASE_T_INIT(test_find_first_detach_3, test_find_first_detach),
        CASE_T_INIT(test_find_first_detach_4, test_find_first_detach),
        CASE_T_INIT(test_find_first_detach_5, test_find_first_detach),
        CASE_T_INIT(test_find_first_detach_6, test_find_first_detach),
        CASE_T_INIT(test_find_first_detach_7, test_find_first_detach),
        CASE_T_INIT(test_find_last_detach_1, test_find_last_detach),
        CASE_T_INIT(test_find_last_detach_2, test_find_last_detach),
        CASE_T_INIT(test_find_last_detach_3, test_find_last_detach),
        CASE_T_INIT(test_find_last_detach_4, test_find_last_detach),
        CASE_T_INIT(test_find_last_detach_5, test_find_last_detach),
        CASE_T_INIT(test_find_last_detach_6, test_find_last_detach),
        CASE_T_INIT(test_find_last_detach_7, test_find_last_detach),
        CASE_T_INIT(test_find_next_detach_1, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_2, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_3, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_4, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_5, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_6, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_7, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_8, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_9, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_10, test_find_next_detach),
        CASE_T_INIT(test_find_next_detach_11, test_find_next_detach),
        CASE_T_INIT(test_find_prev_detach_1, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_2, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_3, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_4, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_5, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_6, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_7, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_8, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_9, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_10, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_11, test_find_prev_detach),
        CASE_T_INIT(test_find_prev_detach_12, test_find_prev_detach),
};

int main(void) {
    list_node_t *head;
    int tests = sizeof(test_cases) / sizeof(test_case_t);

    printf("test case count: %d\n", tests);
    for (int i = 0; i < tests; i++) {
        printf("\t* %s - %s():\n", test_cases[i].case_name, test_cases[i].func_name);

        head = make_list(test_cases[i].test_case, test_cases[i].test_length);
        if (test_cases[i].actions != NULL) {
            test_cases[i].actions(head, test_cases[i].arguments_length, test_cases[i].arguments);
        }

        if (test_cases[i].expected_result == NULL) {
            // ok
        } else if (compare_list_arr(head, test_cases[i].expected_result, test_cases[i].expected_result_length)) {
            SUCCESS();
        } else {
            FAILURE();
            printf("case (%d) input array:     ", i);
            print_array(test_cases[i].test_case, test_cases[i].test_length);
            printf("case (%d) expected output: ", i);
            print_array(test_cases[i].expected_result, test_cases[i].expected_result_length);
            printf("The output list is:\n");
            print_list(head);
        }
        destroy_list(head);
    }

    PRINT_SUMMARY();

    return 0;
}