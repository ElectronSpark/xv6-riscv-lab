// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "lock/spinlock.h"
#include "riscv.h"
#include "proc/proc.h"
#include "percpu.h"
#include "defs.h"
#include "printf.h"

void spin_init(struct spinlock *lk, char *name) {
    lk->name = name;
    lk->locked = 0;
    lk->cpu = 0;
}

// Acquire the lock.
// Caller must disable interrupts before calling spin_lock.
void spin_acquire(struct spinlock *lk) {
    assert(lk && !spin_holding(lk), "spin_lock reentry");

    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    int __debug_count = 0;
    while (__atomic_test_and_set(&lk->locked, __ATOMIC_ACQUIRE) != 0) {
        __debug_count += 1;
        if (__debug_count >= 10) {
            cpu_relax();
        };
    }

    // Record info about lock acquisition for spin_holding() and debugging.
    __atomic_store_n(&lk->cpu, mycpu(), __ATOMIC_RELAXED);
    mycpu()->spin_depth++;
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
}

// Release the lock.
void spin_release(struct spinlock *lk) {
    assert(lk && spin_holding(lk), "spin_unlock");

    // Tell the C compiler and the CPU to not move loads or stores
    // past this point, to ensure that all the stores in the critical
    // section are visible to other CPUs before the lock is released,
    // and that loads in the critical section occur strictly before
    // the lock is released.
    // On RISC-V, this emits a fence instruction.
    __atomic_store_n(&lk->cpu, 0, __ATOMIC_RELEASE);

    // Release the lock, equivalent to lk->locked = 0.
    // This code doesn't use a C assignment, since the C standard
    // implies that an assignment might be implemented with
    // multiple store instructions.
    // On RISC-V, sync_lock_release turns into an atomic swap:
    //   s1 = &lk->locked
    //   amoswap.w zero, zero, (s1)
    __atomic_clear(&lk->locked, __ATOMIC_RELEASE);
    mycpu()->spin_depth--;
}

// Try to acquire the lock without spinning.
// Caller needs to record the preempt state. before calling spin_trylock.
// Returns 1 if the lock was acquired, 0 if not.
int spin_trylock(struct spinlock *lk) {
    push_off(); // disable interrupts
    if (spin_holding(lk)) {
        pop_off();
        return 0; // Already holding the lock (deadlock prevention)
    }

    // Try to atomically set locked to 1
    if (__atomic_test_and_set(&lk->locked, __ATOMIC_ACQUIRE) != 0) {
        // Lock was already held
        pop_off();
        return 0;
    }

    // Successfully acquired the lock
    __atomic_store_n(&lk->cpu, mycpu(), __ATOMIC_RELAXED);
    mycpu()->spin_depth++;
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
    return 1;
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int spin_holding(struct spinlock *lk) {
    if (__atomic_load_n(&lk->cpu, __ATOMIC_ACQUIRE) != mycpu()) {
        return 0;
    }
    return 1;
}

// Default spin lock - disables interrupts and acquires lock
void spin_lock(struct spinlock *lk) {
    push_off(); // disable interrupts to avoid deadlock
    spin_acquire(lk);
}

// Default spin unlock - releases lock and restores interrupt state
void spin_unlock(struct spinlock *lk) {
    spin_release(lk);
    pop_off();
}

int spin_lock_irqsave(struct spinlock *lk) {
    int intena = intr_off_save();
    spin_acquire(lk);
    return intena;
}

void spin_unlock_irqrestore(struct spinlock *lk, int intena) {
    spin_release(lk);
    intr_restore(intena);
}
