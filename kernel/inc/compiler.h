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

#ifdef __CHECKER__
#define __force __attribute__((force))
#define __bitwise __attribute__((bitwise))
#define __acquires(x) __attribute__((context(x, 0, 1)))
#define __releases(x) __attribute__((context(x, 1, 0)))
#define __must_hold(x) __attribute__((context(x, 1, 1)))
#define __acquire_context(x) __context__(x, 1)
#define __release_context(x) __context__(x, -1)
#else
#define __force
#define __bitwise
#define __acquires(x)
#define __releases(x)
#define __must_hold(x)
#define __acquire_context(x)
#define __release_context(x)
#endif

#ifdef __CHECKER__
/*
 * Sparse is interested in type and context flow, not the implementation
 * details of GCC atomics.  Model them as plain accesses so enum and bitfield
 * users remain analyzable.
 */
#define __atomic_load_n(ptr, memorder) (*(ptr))
#define __atomic_store_n(ptr, val, memorder) ((void)(*(ptr) = (val)))
#define __atomic_fetch_add(ptr, val, memorder)                                \
    ({                                                                        \
        typeof(*(ptr)) __old = *(ptr);                                        \
        *(ptr) += (val);                                                      \
        __old;                                                                \
    })
#define __atomic_add_fetch(ptr, val, memorder) (*(ptr) += (val))
#define __atomic_fetch_sub(ptr, val, memorder)                                \
    ({                                                                        \
        typeof(*(ptr)) __old = *(ptr);                                        \
        *(ptr) -= (val);                                                      \
        __old;                                                                \
    })
#define __atomic_sub_fetch(ptr, val, memorder) (*(ptr) -= (val))
#define __atomic_fetch_and(ptr, val, memorder)                                \
    ({                                                                        \
        typeof(*(ptr)) __old = *(ptr);                                        \
        *(ptr) &= (val);                                                      \
        __old;                                                                \
    })
#define __atomic_and_fetch(ptr, val, memorder) (*(ptr) &= (val))
#define __atomic_fetch_or(ptr, val, memorder)                                 \
    ({                                                                        \
        typeof(*(ptr)) __old = *(ptr);                                        \
        *(ptr) |= (val);                                                      \
        __old;                                                                \
    })
#define __atomic_or_fetch(ptr, val, memorder) (*(ptr) |= (val))
#define __atomic_exchange_n(ptr, val, memorder)                               \
    ({                                                                        \
        typeof(*(ptr)) __old = *(ptr);                                        \
        *(ptr) = (val);                                                       \
        __old;                                                                \
    })
#define __atomic_compare_exchange_n(ptr, expected, desired, weak, success,     \
                                    failure)                                  \
    ({                                                                        \
        int __ok = (*(ptr) == *(expected));                                   \
        if (__ok)                                                             \
            *(ptr) = (desired);                                               \
        else                                                                  \
            *(expected) = *(ptr);                                             \
        __ok;                                                                 \
    })
#define __atomic_test_and_set(ptr, memorder)                                  \
    ({                                                                        \
        int __old = !!*(ptr);                                                 \
        *(ptr) = 1;                                                           \
        __old;                                                                \
    })
#define __atomic_clear(ptr, memorder) ((void)(*(ptr) = 0))
#define __atomic_thread_fence(memorder) ((void)0)
#define __atomic_signal_fence(memorder) ((void)0)
#endif

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
#define ALWAYS_INLINE __attribute__((always_inline)) inline

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
