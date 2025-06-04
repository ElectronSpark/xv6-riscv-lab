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

#endif // __KERNEL_COMPILER_H