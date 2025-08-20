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

#define WEAK __attribute__((weak))

#define BUILD_BUG_ON(condition) static inline void  \
__BUILD_BUG_ON_##__FILE__##__##__LINE__(void) {     \
    ((void)sizeof(char[1 - 2*!!(condition)]));      \
}

#endif // __KERNEL_COMPILER_H