/**
 * @file printf.h
 * @brief Kernel printf, panic, and assertion facilities
 */

#ifndef __KERNEL_PRINTF_H
#define __KERNEL_PRINTF_H

/**
 * @brief Check if system is in panic state
 * @return Non-zero if any core has panicked
 * @note Used by spinlock to detect if another core panicked and
 *       enable IPI-only interrupts to receive crash notification.
 */
int panic_state(void);

/**
 * @brief Trigger a system panic and halt all CPUs
 * @note Sends IPI_REASON_CRASH to all cores, then halts.
 *       Called after __panic_start() has printed the panic message.
 */
void trigger_panic(void) __attribute__((noreturn));

/**
 * @brief Acquire the panic message lock
 * @note Used to serialize panic output across multiple cores.
 *       Must be paired with panic_msg_unlock().
 */
void panic_msg_lock(void);

/**
 * @brief Release the panic message lock
 */
void panic_msg_unlock(void);

/**
 * @brief Kernel printf function
 * @param fmt Format string (printf-style)
 * @return Number of characters printed
 */
int             printf(char*, ...) __attribute__ ((format (printf, 1, 2)));

/**
 * @brief Start a kernel panic
 * 
 * Disables interrupts and acquires the panic lock. Must be followed
 * by __panic_end() which does not return.
 */
void            __panic_start(void);

/**
 * @brief Complete a kernel panic and halt the system
 * 
 * Prints backtrace (if enabled) and halts all CPUs.
 * This function does not return.
 */
void            __panic_end(void) __attribute__((noreturn));

/**
 * @brief Disable backtrace printing during panic
 * 
 * Call before __panic_end() to suppress backtrace output.
 */
void            panic_disable_bt(void);

/**
 * @brief Internal panic implementation macro
 * @param type Panic type string (PANIC or ASSERTION_FAILURE)
 * @param fmt Format string for panic message
 */
#define __panic(type, fmt, ...) \
    do { \
        __panic_start(); \
        printf( #type " %s:%d: In function '%s':\n", __FILE__, __LINE__, __FUNCTION__); \
        printf(fmt, ##__VA_ARGS__); \
        printf("\n"); \
        __panic_end(); \
    } while (0)

/**
 * @brief Trigger a kernel panic with a message
 * @param fmt Format string for panic message
 * 
 * Halts the system and prints the given message with file/line info.
 */
#define panic(fmt, ...) \
    __panic(PANIC, fmt, ##__VA_ARGS__)

/**
 * @brief Assert a condition, panic if false
 * @param expr Expression that must be true
 * @param fmt Format string for failure message
 * 
 * If @p expr evaluates to false, triggers a kernel panic with the message.
 */
#define assert(expr, fmt, ...) \
    do { \
        if (!(expr)) { \
            __panic(ASSERTION_FAILURE, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

/**
 * @brief Initialize the printf subsystem
 * 
 * Sets up the console lock for synchronized output.
 */
void            printfinit(void);

#endif // __KERNEL_PRINTF_H
