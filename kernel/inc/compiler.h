/**
 * @file compiler.h
 * @brief Compiler attributes and macros for kernel code
 */

#ifndef __KERNEL_COMPILER_H
#define __KERNEL_COMPILER_H

#ifdef HOST_TEST
/** @brief Expose static functions to the host test */
#define STATIC
#define STATIC_INLINE
#else
#define STATIC static
#define STATIC_INLINE static inline
#endif

#define __force __attribute__((force))

/** @brief CPU cache line size in bytes */
#define CACHELINE_SIZE 64UL
/** @brief Mask for cache line alignment */
#define CACHELINE_MASK (CACHELINE_SIZE - 1UL)
/** @brief Align variable/struct to specified byte boundary */
#define __ALIGNED(x) __attribute__((aligned(x)))
/** @brief Align variable/struct to cache line boundary */
#define __ALIGNED_CACHELINE __ALIGNED(CACHELINE_SIZE)
/** @brief Align variable/struct to page boundary */
#define __ALIGNED_PAGE __ALIGNED(4096)
#define __PACKED __attribute__((packed))
/** @brief Place variable in specified linker section */
#define __SECTION(seg_name) __attribute__((section(#seg_name)))
#define WEAK __attribute__((weak))

/**
 * @brief Create an anonymous struct with specified alignment
 * @param x Alignment in bytes
 *
 * Used to add padding in structures to enforce alignment of subsequent fields.
 */
#define __STRUCT_ALIGNMENT(x)                                                  \
    struct {                                                                   \
    } __attribute__((aligned(x)))

/**
 * @brief Add cache line padding in a structure
 *
 * Ensures the following field starts on a new cache line to prevent
 * false sharing between CPU cores.
 */
#define __STRUCT_CACHELINE_PADDING __STRUCT_ALIGNMENT(CACHELINE_SIZE)

/** @cond INTERNAL */
#define __BUILD_BUG_ON_PASTE(a, b) a##b
#define __BUILD_BUG_ON_PASTE2(a, b) __BUILD_BUG_ON_PASTE(a, b)
/** @endcond */

/**
 * @brief Compile-time assertion macro
 * @param condition The condition that should be false
 *
 * Causes a compile-time error if @p condition evaluates to true.
 * Uses a negative array size trick to trigger the error.
 *
 * Example:
 * @code
 * BUILD_BUG_ON(sizeof(struct foo) > 64);  // Error if struct is too large
 * @endcode
 */
#define BUILD_BUG_ON(condition)                                                \
    static inline void __BUILD_BUG_ON_PASTE2(__build_bug_on_,                  \
                                             __LINE__)(void) {                 \
        ((void)sizeof(char[1 - 2 * !!(condition)]));                           \
    }

#endif // __KERNEL_COMPILER_H