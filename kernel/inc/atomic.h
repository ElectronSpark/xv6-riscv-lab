#ifndef KERNEL_INC_ATOMIC_H
#define KERNEL_INC_ATOMIC_H

#include "types.h"

/* <--- Compiler Barrier Primitives ---> */

/**
 * READ_ONCE - Prevent compiler from optimizing away or reordering reads
 * @x: the value to read
 * 
 * Returns the value of @x, ensuring the compiler generates a single read
 * and doesn't cache or reorder it.
 */
#define READ_ONCE(x) (*(volatile typeof(x) *)&(x))

/**
 * WRITE_ONCE - Prevent compiler from optimizing away or reordering writes
 * @x: the variable to write to
 * @val: the value to write
 * 
 * Ensures the compiler generates a single write and doesn't cache or reorder it.
 */
#define WRITE_ONCE(x, val) (*(volatile typeof(x) *)&(x) = (val))


static inline bool atomic_dec_unless(int *value, int unless) {
    int old = __atomic_load_n(value, __ATOMIC_SEQ_CST);
    while (old != unless) {
        int new_val = old - 1;
        if (__atomic_compare_exchange_n(value, &old, new_val,
                                        false, __ATOMIC_SEQ_CST, 
                                        __ATOMIC_SEQ_CST)) {
            return true;
        }
        // old is updated with the current value of *value
    }
    return false;
}

static inline bool atomic_inc_unless(int *value, int unless) {
    int old = __atomic_load_n(value, __ATOMIC_SEQ_CST);
    while (old != unless) {
        int new_val = old + 1;
        if (__atomic_compare_exchange_n(value, &old, new_val,
                                        false, __ATOMIC_SEQ_CST, 
                                        __ATOMIC_SEQ_CST)) {
            return true;
        }
        // old is updated with the current value of *value
    }
    return false;
}

static inline void atomic_dec(int *value) {
    __atomic_fetch_sub(value, 1, __ATOMIC_SEQ_CST);
}

static inline void atomic_inc(int *value) {
    __atomic_fetch_add(value, 1, __ATOMIC_SEQ_CST);
}

#define atomic_cas_ptr(ptr, old, new_val) ({                                \
    bool __RES = __atomic_compare_exchange_n((ptr), (old), (new_val),       \
                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    __RES;                                                                  \
})

#define atomic_cas(ptr, old, new_val) ({        \
    typeof(*(ptr)) __OLD = (old);                    \
    atomic_cas_ptr((ptr), &__OLD, (new_val));   \
})

// From Linux barrier.h
#define __mb()       __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define __rmb()      __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define __wmb()      __atomic_thread_fence(__ATOMIC_RELEASE)

#define __smp_store_release(p, v) \
    __atomic_store_n((p), (v), __ATOMIC_RELEASE)

#define __smp_load_acquire(p) \
    __atomic_load_n((p), __ATOMIC_ACQUIRE)


#define mb()    __mb()
#define rmb()   __rmb()
#define wmb()   __wmb()

// SMP memory barriers - on SMP these are real barriers
#define smp_mb()    __mb()
#define smp_rmb()   __rmb()
#define smp_wmb()   __wmb()

#define smp_store_release(p, v) __smp_store_release((p), (v))
#define smp_load_acquire(p) __smp_load_acquire((p))

// Hint to the CPU that we're in a spin-wait loop
#define cpu_relax() asm volatile("nop" ::: "memory")

#define smp_cond_load_acquire(ptr, cond) ({             \
    typeof(ptr) __PTR = (ptr);                          \
    typeof(*ptr) VAL;                                   \
    for (;;) {                                          \
        VAL = smp_load_acquire(__PTR);                  \
        if (cond) {                                     \
            break;                                      \
        }                                               \
        cpu_relax();                                    \
    }                                                   \
    VAL;                                                \
})


#endif // KERNEL_INC_ATOMIC_H
