#ifndef KERNEL_INC_ATOMIC_H
#define KERNEL_INC_ATOMIC_H

#include "types.h"

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

#endif // KERNEL_INC_ATOMIC_H
