#ifndef __HOST_TEST_COMMON_H
#define __HOST_TEST_COMMON_H

#include <stdio.h>

#define PRINT_COLOR_BLACK "\033[0;30m"
#define PRINT_COLOR_RED "\033[0;31m"
#define PRINT_COLOR_GREEN "\033[0;32m"
#define PRINT_COLOR_YELLOW "\033[0;33m"
#define PRINT_COLOR_BLUE "\033[0;34m"
#define PRINT_COLOR_PINK "\033[0;35m"
#define PRINT_COLOR_TEAL "\033[0;36m"
#define PRINT_COLOR_WHITE "\033[0;37m"
#define PRINT_COLOR_DEFAULT "\033[0m"

#define __PRINT_RESULT(__color, __msg_type)                                    \
    do {                                                                       \
        printf("[ " __color __msg_type PRINT_COLOR_DEFAULT " ]"                \
               " - %s(%d): %s\n",                                              \
               __FILE__, __LINE__, __FUNCTION__);                              \
    } while (0)

static int __attribute__((unused)) __failure_count = 0;
static int __attribute__((unused)) __success_count = 0;

#define FAILURE_COUNT()                                                        \
    do {                                                                       \
        __failure_count++;                                                     \
    } while (0)
#define SUCCESS_COUNT()                                                        \
    do {                                                                       \
        __success_count++;                                                     \
    } while (0)

#define SUCCESS()                                                              \
    do {                                                                       \
        __PRINT_RESULT(PRINT_COLOR_GREEN, "SUCCESS");                          \
        SUCCESS_COUNT();                                                       \
    } while (0)

#define FAILURE()                                                              \
    do {                                                                       \
        __PRINT_RESULT(PRINT_COLOR_RED, "FAILURE");                            \
        FAILURE_COUNT();                                                       \
    } while (0)

#define PRINT_SUMMARY()                                                        \
    do {                                                                       \
        printf("------------------------ summary ------------------------\n"); \
        printf("success: %d\nfailure: %d\n", __success_count,                  \
               __failure_count);                                               \
    } while (0)

#endif /* __HOST_TEST_COMMON_H */
