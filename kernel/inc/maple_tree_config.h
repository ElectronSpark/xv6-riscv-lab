/**
 * @file maple_tree_config.h
 * @brief Kernel-tailored configuration for the standalone maple tree.
 */

#ifndef MAPLE_TREE_CONFIG_H
#define MAPLE_TREE_CONFIG_H

#include "types.h"
#include "compiler.h"

#include <stddef.h>
#include <stdint.h>
/* See xarray_config.h for why stdbool.h is deliberately omitted. */

#include "lock/spinlock.h"
#include "lock/rcu_type.h"
#include <smp/atomic.h>

/* ====================================================================== */
/*  Feature toggles                                                        */
/* ====================================================================== */

#define MT_CONFIG_LOCK
#define MT_CONFIG_RCU

#define MT_CUSTOM_ALLOC
#define MT_CUSTOM_LOCK
#define MT_CUSTOM_RCU
#define MT_CUSTOM_BARRIERS

/* ====================================================================== */
/*  Allocation                                                             */
/* ====================================================================== */

void *mt_alloc_fn(size_t size);
void  mt_free_fn(void *ptr);

/* ====================================================================== */
/*  Locking                                                                */
/* ====================================================================== */

typedef spinlock_t mt_lock_t;

/* Forward declarations for the kernel spinlock API. */
void spin_init(spinlock_t *lock, char *name);
void spin_lock(spinlock_t *lock) __acquires(lock);
void spin_unlock(spinlock_t *lock) __releases(lock);

#define MT_LOCK_INITIALIZER SPINLOCK_INITIALIZED("maple_tree.ma_lock")

static inline void mt_lock_init(mt_lock_t *lock)
{
    spin_init(lock, "maple_tree");
}

static inline void mt_spin_lock(mt_lock_t *lock) __acquires(lock)
{
    spin_lock(lock);
}

static inline void mt_spin_unlock(mt_lock_t *lock) __releases(lock)
{
    spin_unlock(lock);
}

/* ====================================================================== */
/*  RCU                                                                    */
/* ====================================================================== */

typedef void (*mt_rcu_callback_t)(void *);

void mt_rcu_read_lock(void) __acquires(__rcu_context);
void mt_rcu_read_unlock(void) __releases(__rcu_context);
void mt_call_rcu(mt_rcu_callback_t cb, void *node);

/* ====================================================================== */
/*  Memory barriers                                                        */
/* ====================================================================== */

#if defined(__x86_64__) || defined(__i386__)
#define mt_smp_rmb()  __asm__ volatile("" ::: "memory")
#define mt_smp_wmb()  __asm__ volatile("" ::: "memory")
#define mt_smp_mb()   __asm__ volatile("mfence" ::: "memory")
#elif defined(__aarch64__)
#define mt_smp_rmb()  __asm__ volatile("dmb ishld" ::: "memory")
#define mt_smp_wmb()  __asm__ volatile("dmb ishst" ::: "memory")
#define mt_smp_mb()   __asm__ volatile("dmb ish"   ::: "memory")
#elif defined(__riscv)
#define mt_smp_rmb()  __asm__ volatile("fence r,r"   ::: "memory")
#define mt_smp_wmb()  __asm__ volatile("fence w,w"   ::: "memory")
#define mt_smp_mb()   __asm__ volatile("fence rw,rw" ::: "memory")
#else
#define mt_smp_rmb()  __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define mt_smp_wmb()  __atomic_thread_fence(__ATOMIC_RELEASE)
#define mt_smp_mb()   __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

/* ====================================================================== */
/*  Compiler helpers                                                       */
/* ====================================================================== */

#ifndef READ_ONCE
#define READ_ONCE(x)       (*(const volatile __typeof__(x) *)&(x))
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, val) do { *(volatile __typeof__(x) *)&(x) = (val); } while (0)
#endif

/* ====================================================================== */
/*  RCU-ordered pointer access                                             */
/* ====================================================================== */

static inline void *mt_rcu_dereference(void *const *slot)
{
    void *p = *(void *const volatile *)slot;
    mt_smp_rmb();
    return p;
}

static inline void mt_rcu_assign_pointer(void **slot, void *val)
{
    mt_smp_wmb();
    *(void *volatile *)slot = val;
}

#endif /* MAPLE_TREE_CONFIG_H */
