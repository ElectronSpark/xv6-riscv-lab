/**
 * @file xarray_config.h
 * @brief Kernel-tailored configuration for the standalone XArray.
 *
 * This header customises the portable XArray implementation so it can
 * link against the xv6 kernel: slab-backed allocation, kernel spinlocks,
 * kernel RCU, and kernel memory barriers.
 */

#ifndef XARRAY_CONFIG_H
#define XARRAY_CONFIG_H

#include "types.h"
#include "compiler.h"

/* Freestanding headers (provided by the compiler). */
#include <stddef.h>
#include <stdint.h>
/* Note: deliberately do NOT include <stdbool.h>.  The xv6 kernel's
 * types.h defines `bool` as an enum when stdbool.h isn't pulled in;
 * mixing that with stdbool.h's `#define bool _Bool` yields function
 * pointer mismatches across translation units. */

#include "lock/spinlock.h"
#include "lock/rcu_type.h"
#include <smp/atomic.h>

/* ====================================================================== */
/*  Feature toggles                                                        */
/* ====================================================================== */

#define XA_CONFIG_LOCK
#define XA_CONFIG_RCU

/*
 * Suppress the default stdlib-based hooks that the portable header
 * would otherwise emit.  We provide kernel-backed implementations below.
 */
#define XA_CUSTOM_ALLOC
#define XA_CUSTOM_LOCK
#define XA_CUSTOM_RCU
#define XA_CUSTOM_BARRIERS

/* ====================================================================== */
/*  Allocation — slab-backed                                               */
/* ====================================================================== */

/* Defined in kernel/xarray.c */
void *xa_alloc_fn(size_t size);
void  xa_free_fn(void *ptr);

/* ====================================================================== */
/*  Locking — kernel spinlock                                              */
/* ====================================================================== */

typedef spinlock_t xa_lock_t;

/* Forward declarations for the kernel spinlock API.  We don't pull in
 * defs.h here because this header is indirectly included by almost every
 * kernel translation unit and defs.h has its own dependencies. */
void spin_init(spinlock_t *lock, char *name);
void spin_lock(spinlock_t *lock) __acquires(lock);
void spin_unlock(spinlock_t *lock) __releases(lock);

#define XA_LOCK_INITIALIZER(name) SPINLOCK_INITIALIZED(#name ".xa_lock")

static inline void xa_lock_init(xa_lock_t *lock)
{
    spin_init(lock, "xa_lock");
}

static inline void xa_spin_lock(xa_lock_t *lock) __acquires(lock)
{
    spin_lock(lock);
}

static inline void xa_spin_unlock(xa_lock_t *lock) __releases(lock)
{
    spin_unlock(lock);
}

/* ====================================================================== */
/*  RCU — kernel RCU                                                        */
/* ====================================================================== */

typedef void (*xa_rcu_callback_t)(void *);

void xa_rcu_read_lock(void) __acquires(__rcu_context);
void xa_rcu_read_unlock(void) __releases(__rcu_context);

/*
 * xa_call_rcu() takes a void-pointer callback and a node pointer.
 * The kernel call_rcu() requires a per-object rcu_head_t.  We rely on
 * struct xa_node carrying an embedded rcu_head_t (see xarray_type.h)
 * and dispatch through a small trampoline defined in xarray.c.
 */
void xa_call_rcu(xa_rcu_callback_t cb, void *node);

/* ====================================================================== */
/*  Memory barriers                                                        */
/* ====================================================================== */

#if defined(__x86_64__) || defined(__i386__)
#define xa_smp_rmb()  __asm__ volatile("" ::: "memory")
#define xa_smp_wmb()  __asm__ volatile("" ::: "memory")
#define xa_smp_mb()   __asm__ volatile("mfence" ::: "memory")
#elif defined(__aarch64__)
#define xa_smp_rmb()  __asm__ volatile("dmb ishld" ::: "memory")
#define xa_smp_wmb()  __asm__ volatile("dmb ishst" ::: "memory")
#define xa_smp_mb()   __asm__ volatile("dmb ish"   ::: "memory")
#elif defined(__riscv)
#define xa_smp_rmb()  __asm__ volatile("fence r,r"   ::: "memory")
#define xa_smp_wmb()  __asm__ volatile("fence w,w"   ::: "memory")
#define xa_smp_mb()   __asm__ volatile("fence rw,rw" ::: "memory")
#else
#define xa_smp_rmb()  __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define xa_smp_wmb()  __atomic_thread_fence(__ATOMIC_RELEASE)
#define xa_smp_mb()   __atomic_thread_fence(__ATOMIC_SEQ_CST)
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
/*  Slot / flag access (RCU-ordered)                                       */
/* ====================================================================== */

static inline void *xa_slot_load(void **slot)
{
    void *p = *(void *volatile *)slot;
    xa_smp_rmb();
    return p;
}

static inline void xa_slot_store(void **slot, void *entry)
{
    xa_smp_wmb();
    *(void *volatile *)slot = entry;
}

static inline unsigned int xa_flags_load(const unsigned int *flags)
{
    unsigned int v = *(const volatile unsigned int *)flags;
    xa_smp_rmb();
    return v;
}

static inline void xa_flags_or(unsigned int *flags, unsigned int bits)
{
    unsigned int v = *flags;
    v |= bits;
    xa_smp_wmb();
    *(volatile unsigned int *)flags = v;
}

static inline void xa_flags_and(unsigned int *flags, unsigned int bits)
{
    unsigned int v = *flags;
    v &= bits;
    xa_smp_wmb();
    *(volatile unsigned int *)flags = v;
}

#endif /* XARRAY_CONFIG_H */
