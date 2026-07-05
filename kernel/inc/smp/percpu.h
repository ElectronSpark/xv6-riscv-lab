#ifndef __KERNEL_PER_CPU_H
#define __KERNEL_PER_CPU_H

#include "param.h"
#include "riscv.h"
#include <smp/percpu_types.h>
#include "printf.h"
#include "bits.h"
#if defined(__x86_64__) && !defined(ON_HOST_OS)
#include "memlayout.h"
#endif

extern struct cpu_local cpus[];

/** @defgroup cpu_flags CPU Flags
 * @brief Per-CPU status flags for scheduler and panic handling
 * @{
 */
/**< CPU should reschedule at next opportunity */
#define CPU_FLAG_NEEDS_RESCHED 1
#define CPU_FLAG_BOOT_HART 2 /**< This is the boot hart */
#define CPU_FLAG_IN_ITR 4    /**< CPU is currently in interrupt handler */
#define CPU_FLAG_CRASHED 8   /**< CPU has received panic IPI and halted */
#define CPU_FLAG_HALTED 16    /**< CPU is in the idle interrupt-wait path */
/** @} */

#if !defined(ON_HOST_OS)

#define SET_NEEDS_RESCHED()                                                    \
    do {                                                                       \
        mycpu()->flags |= CPU_FLAG_NEEDS_RESCHED;                              \
    } while (0)
#define CLEAR_NEEDS_RESCHED()                                                  \
    do {                                                                       \
        mycpu()->flags &= ~CPU_FLAG_NEEDS_RESCHED;                             \
    } while (0)
#define NEEDS_RESCHED() (!!(mycpu()->flags & CPU_FLAG_NEEDS_RESCHED))
#define CPU_SET_IN_ITR()                                                       \
    do {                                                                       \
        mycpu()->flags |= CPU_FLAG_IN_ITR;                                     \
    } while (0)
#define CPU_CLEAR_IN_ITR()                                                     \
    do {                                                                       \
        mycpu()->flags &= ~CPU_FLAG_IN_ITR;                                    \
    } while (0)
#define CPU_IN_ITR() (!!(mycpu()->flags & CPU_FLAG_IN_ITR))
#define CPU_SET_HALTED()                                                     \
    do {                                                                     \
        mycpu()->flags |= CPU_FLAG_HALTED;                                   \
    } while (0)
#define CPU_CLEAR_HALTED()                                                   \
    do {                                                                     \
        mycpu()->flags &= ~CPU_FLAG_HALTED;                                  \
    } while (0)
#define SET_BOOT_HART()                                                        \
    do {                                                                       \
        mycpu()->flags |= CPU_FLAG_BOOT_HART;                                  \
    } while (0)
#define CLEAR_BOOT_HART()                                                      \
    do {                                                                       \
        mycpu()->flags &= ~CPU_FLAG_BOOT_HART;                                 \
    } while (0)
#define IS_BOOT_HART() (!!(mycpu()->flags & CPU_FLAG_BOOT_HART))

/**
 * @name CPU Crashed Flag Macros
 * @brief Mark/check if this CPU has received a panic notification
 * Used to prevent IPI storms during system panic.
 * @{
 */
#define SET_CPU_CRASHED()                                                      \
    do {                                                                       \
        mycpu()->flags |= CPU_FLAG_CRASHED;                                    \
    } while (0)
#define CPU_CRASHED() (!!(mycpu()->flags & CPU_FLAG_CRASHED))
#define CLEAR_CPU_CRASHED()                                                    \
    do {                                                                       \
        mycpu()->flags &= ~CPU_FLAG_CRASHED;                                   \
    } while (0)
/** @} */

// Return this CPU's cpu struct.
// Interrupts must be disabled.
#define mycpu() ((struct cpu_local *)r_tp())

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

#define push_off()                                                             \
    do {                                                                       \
        int __OLD = intr_get();                                                \
        if (__OLD) {                                                           \
            intr_off();                                                        \
        }                                                                      \
        if (mycpu()->noff++ == 0) {                                            \
            mycpu()->intena = __OLD;                                           \
        }                                                                      \
    } while (0)

#define pop_off()                                                              \
    do {                                                                       \
        struct cpu_local *c = mycpu();                                         \
        assert(!intr_get(), "pop_off - interruptible");                        \
        assert(c->noff >= 1, "pop_off");                                       \
        c->noff -= 1;                                                          \
        if (c->noff == 0 && c->intena) {                                       \
            intr_on();                                                         \
        }                                                                      \
    } while (0)

static inline int cpuid_from_tp(uint64 tp)
{
    uint64 base = (uint64)cpus;
    uint64 limit = base + sizeof(struct cpu_local) * MAX_CPUS;

    if (tp >= base && tp < limit)
        return (int)(((struct cpu_local *)tp) - cpus);

#if defined(__x86_64__)
    if (tp >= TRAMPOLINE_CPULOCAL &&
        tp < TRAMPOLINE_CPULOCAL + TRAMPOLINE_CPULOCAL_BYTES) {
        uint64 translated = base + (tp - TRAMPOLINE_CPULOCAL);
        if (translated >= base && translated < limit)
            return (int)(((struct cpu_local *)translated) - cpus);
    }
#endif

    /*
     * RISC-V and very early x86 boot historically derived the CPU id from the
     * offset inside the low page.  Keep that fallback for firmware paths that
     * have not switched to the full cpus[] address yet.
     */
    struct cpu_local *offset = (void *)(tp & PAGE_MASK);
    return (int)(offset - (struct cpu_local *)0);
}

// Must be called with interrupts disabled,
// to prevent race with thread being moved
// to a different CPU.
#define cpuid() cpuid_from_tp(r_tp())

// Return the current struct thread *, or zero if none.
#define __current_thread()                                                     \
    ({                                                                         \
        int __OLD = intr_get();                                                \
        if (__OLD) {                                                           \
            intr_off();                                                        \
        }                                                                      \
        struct cpu_local *__CPU_LOCAL = mycpu();                               \
        struct thread *__CURRENT = __CPU_LOCAL->proc;                          \
        if (__OLD) {                                                           \
            intr_on();                                                         \
        }                                                                      \
        __CURRENT;                                                             \
    })

#define current __current_thread()

#else /* ON_HOST_OS */

/*
 * Host OS declarations for unit testing - these are function declarations
 * that can be intercepted by the linker's --wrap mechanism for mocking.
 * The actual implementations are in test/src/wrappers/proc_wrappers.c
 */
#define SET_NEEDS_RESCHED()                                                    \
    do {                                                                       \
    } while (0)
#define CLEAR_NEEDS_RESCHED()                                                  \
    do {                                                                       \
    } while (0)
#define NEEDS_RESCHED() (0)
#define CPU_SET_IN_ITR()                                                       \
    do {                                                                       \
    } while (0)
#define CPU_CLEAR_IN_ITR()                                                     \
    do {                                                                       \
    } while (0)
#define CPU_IN_ITR() (0)
#define CPU_SET_HALTED()                                                       \
    do {                                                                       \
    } while (0)
#define CPU_CLEAR_HALTED()                                                     \
    do {                                                                       \
    } while (0)
#define SET_BOOT_HART()                                                        \
    do {                                                                       \
    } while (0)
#define CLEAR_BOOT_HART()                                                      \
    do {                                                                       \
    } while (0)
#define IS_BOOT_HART() (0)

/* Function declarations for wrappable functions */
struct cpu_local *mycpu(void);
void push_off(void);
void pop_off(void);
int cpuid(void);
struct thread *myproc(void);
struct thread *__current_thread(void);

#define current __current_thread()

#endif /* ON_HOST_OS */

/*
 * Snapshot this CPU's spinlock-nesting depth with migration excluded.
 *
 * With IRQ-exit kernel preemption, a thread running with interrupts
 * ENABLED can be preempted at any instruction and resume on another CPU.
 * A bare mycpu()->spin_depth read from such a context can therefore
 * observe a *different* CPU's transient counter (e.g. 1 while that CPU's
 * scheduler holds its rq_lock during a pick), even though the calling
 * thread holds nothing.  Sampling under push_off() pins the thread to
 * one CPU for the read; with IRQs off, a nonzero value can only reflect
 * spinlocks held by the caller itself (a caller that really holds a
 * spinlock has IRQs masked already, so it cannot have migrated and the
 * read is exact).  Use this in debug assertions instead of touching
 * mycpu()->spin_depth directly from preemptible (IF=1) contexts.
 */
static inline int spin_depth_snapshot(void)
{
    int depth;

    push_off();
    depth = mycpu()->spin_depth;
    pop_off();
    return depth;
}

void cpus_init(void);
void mycpu_init(uint64 hartid, bool trampoline);
cpumask_t get_cpu_active_mask(void);
int cpu_possible_count(void);
cpumask_t cpu_possible_mask(void);

#define PERCPU_CPU_MASK cpu_possible_mask()

#define cpu_for_each_in_mask(cpu, mask) bits_foreach_set_bit((mask), cpu)

#define cpu_for_each_active(cpu)                                               \
    cpu_for_each_in_mask(cpu, get_cpu_active_mask())

#define cpu_for_each_possible(cpu) for (cpu = 0; cpu < cpu_possible_count(); cpu++)
#define cpu_for_each_all(cpu) cpu_for_each_possible(cpu)

#endif /* __KERNEL_PER_CPU_H */
