#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>
#include "page.h"

static void page_refinc_test(page_t *page) {
    (void)page;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(page_refinc_test),
    };

    int result = cmocka_run_group_tests(tests, NULL, NULL);
    return result;
}

