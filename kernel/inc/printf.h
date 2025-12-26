#ifndef __KERNEL_PRINTF_H
#define __KERNEL_PRINTF_H

int             printf(char*, ...) __attribute__ ((format (printf, 1, 2)));
void            __panic_start(void);
void            __panic_end(void) __attribute__((noreturn));
int             panic_state(void);
void            panic_disable_bt(void);
#define __panic(type, fmt, ...) \
    do { \
        __panic_start(); \
        printf( #type " %s:%d: In function '%s':\n", __FILE__, __LINE__, __FUNCTION__); \
        printf(fmt, ##__VA_ARGS__); \
        printf("\n"); \
        __panic_end(); \
    } while (0)
#define panic(fmt, ...) \
    __panic(PANIC, fmt, ##__VA_ARGS__)
#define assert(expr, fmt, ...) \
    do { \
        if (!(expr)) { \
            __panic(ASSERTION_FAILURE, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
void            printfinit(void);

#endif // __KERNEL_PRINTF_H
