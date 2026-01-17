#ifndef __KERNEL_COMPILER_H
#define __KERNEL_COMPILER_H

#ifdef HOST_TEST
// Expose static functions to the host test
#define STATIC
#define STATIC_INLINE
#else
#define STATIC static
#define STATIC_INLINE static inline
#endif

#define __force __attribute__((force))

#define CACHELINE_SIZE 64UL
#define CACHELINE_MASK (CACHELINE_SIZE - 1UL)
#define __ALIGNED(x) __attribute__((aligned(x)))
#define __ALIGNED_CACHELINE __ALIGNED(CACHELINE_SIZE)
#define __ALIGNED_PAGE __ALIGNED(4096)
#define __PACKED __attribute__((packed))
#define __SECTION(seg_name) __attribute__((section(#seg_name)))
#define WEAK __attribute__((weak))

#define BUILD_BUG_ON(condition) static inline void  \
__BUILD_BUG_ON_##__FILE__##__##__LINE__(void) {     \
    ((void)sizeof(char[1 - 2*!!(condition)]));      \
}

#endif // __KERNEL_COMPILER_H