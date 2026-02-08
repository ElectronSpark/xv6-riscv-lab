#ifndef __KERNEL_PER_CPU_H
#define __KERNEL_PER_CPU_H

#include "param.h"
#include "riscv.h"
#include <smp/percpu_types.h>
#include "printf.h"
#include "bits.h"

extern struct cpu_local cpus[NCPU];

/** @defgroup cpu_flags CPU Flags
 * @brief Per-CPU status flags for scheduler and panic handling
 * @{
 */
#define CPU_FLAG_NEEDS_RESCHED  1  /**< CPU should reschedule at next opportunity */
#define CPU_FLAG_BOOT_HART      2  /**< This is the boot hart */
#define CPU_FLAG_IN_ITR         4  /**< CPU is currently in interrupt handler */
#define CPU_FLAG_CRASHED        8  /**< CPU has received panic IPI and halted */
/** @} */

#if !defined(ON_HOST_OS)

#define SET_NEEDS_RESCHED() \
  do { mycpu()->flags |= CPU_FLAG_NEEDS_RESCHED; } while(0)
#define CLEAR_NEEDS_RESCHED() \
  do { mycpu()->flags &= ~CPU_FLAG_NEEDS_RESCHED; } while(0)
#define NEEDS_RESCHED() \
  (!!(mycpu()->flags & CPU_FLAG_NEEDS_RESCHED))
#define CPU_SET_IN_ITR() \
  do { mycpu()->flags |= CPU_FLAG_IN_ITR; } while(0)
#define CPU_CLEAR_IN_ITR() \
  do { mycpu()->flags &= ~CPU_FLAG_IN_ITR; } while(0)
#define CPU_IN_ITR() \
  (!!(mycpu()->flags & CPU_FLAG_IN_ITR))
#define SET_BOOT_HART() \
  do { mycpu()->flags |= CPU_FLAG_BOOT_HART; } while(0)
#define CLEAR_BOOT_HART() \
  do { mycpu()->flags &= ~CPU_FLAG_BOOT_HART; } while(0)
#define IS_BOOT_HART() \
  (!!(mycpu()->flags & CPU_FLAG_BOOT_HART))

/**
 * @name CPU Crashed Flag Macros
 * @brief Mark/check if this CPU has received a panic notification
 * Used to prevent IPI storms during system panic.
 * @{
 */
#define SET_CPU_CRASHED() \
  do { mycpu()->flags |= CPU_FLAG_CRASHED; } while(0)
#define CPU_CRASHED() \
  (!!(mycpu()->flags & CPU_FLAG_CRASHED))
#define CLEAR_CPU_CRASHED() \
  do { mycpu()->flags &= ~CPU_FLAG_CRASHED; } while(0)
/** @} */

// Return this CPU's cpu struct.
// Interrupts must be disabled.
#define mycpu()     ((struct cpu_local *)r_tp())

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

#define push_off() do {             \
    int __OLD = intr_get();         \
    if (__OLD) {                    \
        intr_off();                 \
    }                               \
    if(mycpu()->noff++ == 0) {      \
        mycpu()->intena = __OLD;    \
    }                               \
} while (0)

#define pop_off() do {                              \
    struct cpu_local *c = mycpu();                  \
    assert(!intr_get(), "pop_off - interruptible"); \
    assert(c->noff >= 1, "pop_off");                \
    c->noff -= 1;                                   \
    if(c->noff == 0 && c->intena) {                 \
        intr_on();                                  \
    }                                               \
} while (0)


// Must be called with interrupts disabled,
// to prevent race with thread being moved
// to a different CPU.
#define cpuid()     ({                                          \
    /* Calculate cpuid from offset within page */               \
    /* works for both physical and virtual addresses */         \
    struct cpu_local *offset = (void *)(r_tp() & PAGE_MASK);    \
    offset - (struct cpu_local *)0;                             \
})

// Return the current struct thread *, or zero if none.
#define __current_thread() ({                       \
    int __OLD = intr_get();                         \
    if (__OLD) {                                    \
        intr_off();                                 \
    }                                               \
    struct cpu_local *__CPU_LOCAL = mycpu();        \
    struct thread *__CURRENT = __CPU_LOCAL->proc;   \
    if (__OLD) {                                    \
        intr_on();                                  \
    }                                               \
    __CURRENT;                                      \
})

#define current __current_thread()

#else /* ON_HOST_OS */

/*
 * Host OS declarations for unit testing - these are function declarations
 * that can be intercepted by the linker's --wrap mechanism for mocking.
 * The actual implementations are in test/src/wrappers/proc_wrappers.c
 */
#define SET_NEEDS_RESCHED()     do { } while(0)
#define CLEAR_NEEDS_RESCHED()   do { } while(0)
#define NEEDS_RESCHED()         (0)
#define CPU_SET_IN_ITR()        do { } while(0)
#define CPU_CLEAR_IN_ITR()      do { } while(0)
#define CPU_IN_ITR()            (0)
#define SET_BOOT_HART()         do { } while(0)
#define CLEAR_BOOT_HART()       do { } while(0)
#define IS_BOOT_HART()          (0)

/* Function declarations for wrappable functions */
struct cpu_local *mycpu(void);
void push_off(void);
void pop_off(void);
int cpuid(void);
struct thread *myproc(void);

#endif /* ON_HOST_OS */

void cpus_init(void);
void mycpu_init(uint64 hartid, bool trampoline);
cpumask_t get_cpu_active_mask(void);

#define PERCPU_NCPU_MASK   ((1UL << NCPU) - 1)

#define cpu_for_each_in_mask(cpu, mask) \
    bits_foreach_set_bit((mask), cpu)

#define cpu_for_each_active(cpu) \
    cpu_for_each_in_mask(cpu, get_cpu_active_mask())

#define cpu_for_each_all(cpu) \
    for (cpu = 0; cpu < NCPU; cpu++)


#endif        /* __KERNEL_PER_CPU_H */
