#ifndef __KERNEL_PROC_H
#define __KERNEL_PROC_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"
#include "proc/proc_queue_type.h"
#include "proc/rq_types.h"
#include "trapframe.h"
#include "signal_types.h"
#include "mm/vm_types.h"
#include "vfs/vfs_types.h"
#include <smp/atomic.h>
#include <smp/percpu.h>
#include "lock/rcu_type.h"
#include "proc/proc_types.h"

#define PSTATE_IS_SLEEPING(state)                                              \
    ({                                                                         \
        (state) == PSTATE_INTERRUPTIBLE ||                                     \
            (state) == PSTATE_UNINTERRUPTIBLE || (state) == STATE_KILLABLE ||  \
            (state) == STATE_TIMER || (state) == STATE_KILLABLE_TIMER;         \
    })

#define PSTATE_IS_KILLABLE(state)                                              \
    ({                                                                         \
        (state) == STATE_KILLABLE || (state) == STATE_KILLABLE_TIMER ||        \
            (state) == PSTATE_INTERRUPTIBLE;                                   \
    })

#define PSTATE_IS_TIMER(state)                                                 \
    ({                                                                         \
        (state) == STATE_TIMER || (state) == STATE_KILLABLE_TIMER ||           \
            (state) == PSTATE_INTERRUPTIBLE;                                   \
    })

#define PSTATE_IS_INTERRUPTIBLE(state) ({ (state) == PSTATE_INTERRUPTIBLE; })

#define PSTATE_IS_AWOKEN(state)                                                \
    ({ (state) == PSTATE_RUNNING || (state) == PSTATE_WAKENING; })

#define PSTATE_IS_RUNNING(state) ({ (state) == PSTATE_RUNNING; })

#define PSTATE_IS_ZOMBIE(state) ({ (state) == PSTATE_ZOMBIE; })

#define PSTATE_IS_STOPPED(state) ({ (state) == PSTATE_STOPPED; })

static inline uint64 proc_flags(struct proc *p) {
    if (p == NULL) {
        return 0;
    }
    return __atomic_load_n(&p->flags, __ATOMIC_SEQ_CST);
}

static inline void proc_set_flags(struct proc *p, uint64 flags) {
    if (p == NULL) {
        return;
    }
    __atomic_or_fetch(&p->flags, flags, __ATOMIC_SEQ_CST);
}

static inline void proc_clear_flags(struct proc *p, uint64 flags) {
    if (p == NULL) {
        return;
    }
    __atomic_and_fetch(&p->flags, ~flags, __ATOMIC_SEQ_CST);
}

#define DEFINE_PROC_FLAG(__NAME, __VALUE)                                      \
    static inline bool PROC_##__NAME(struct proc *p) {                         \
        if (p == NULL) {                                                       \
            return false;                                                      \
        }                                                                      \
        uint64 __flags = smp_load_acquire(&p->flags);                          \
        return !!(__flags & (1ULL << (__VALUE)));                              \
    }                                                                          \
    static inline void PROC_SET_##__NAME(struct proc *p) {                     \
        if (p == NULL) {                                                       \
            return;                                                            \
        }                                                                      \
        __atomic_or_fetch(&p->flags, (1ULL << (__VALUE)), __ATOMIC_SEQ_CST);   \
    }                                                                          \
    static inline void PROC_CLEAR_##__NAME(struct proc *p) {                   \
        if (p == NULL) {                                                       \
            return;                                                            \
        }                                                                      \
        __atomic_and_fetch(&p->flags, ~(1ULL << (__VALUE)), __ATOMIC_SEQ_CST); \
    }

DEFINE_PROC_FLAG(USER_SPACE, PROC_FLAG_USER_SPACE)
DEFINE_PROC_FLAG(VALID, PROC_FLAG_VALID)
DEFINE_PROC_FLAG(KILLED, PROC_FLAG_KILLED)
DEFINE_PROC_FLAG(ONCHAN, PROC_FLAG_ONCHAN)

static inline const char *procstate_to_str(enum procstate state) {
    switch (state) {
    case PSTATE_UNUSED:
        return "unused";
    case PSTATE_USED:
        return "used";
    case PSTATE_INTERRUPTIBLE:
        return "interruptible";
    case STATE_KILLABLE:
        return "killable";
    case STATE_TIMER:
        return "timer";
    case STATE_KILLABLE_TIMER:
        return "killable_timer";
    case PSTATE_UNINTERRUPTIBLE:
        return "uninterruptible";
    case PSTATE_WAKENING:
        return "wakening";
    case PSTATE_RUNNING:
        return "running";
    case PSTATE_STOPPED:
        return "stopped";
    case PSTATE_EXITING:
        return "exiting";
    case PSTATE_ZOMBIE:
        return "zombie";
    default:
        return "*unknown";
    }
}

static inline enum procstate __proc_get_pstate(struct proc *p) {
    if (p == NULL) {
        return PSTATE_UNUSED;
    }
    return __atomic_load_n(&p->state, __ATOMIC_SEQ_CST);
}

static inline void __proc_set_pstate(struct proc *p, enum procstate state) {
    if (p == NULL) {
        return;
    }
    __atomic_store_n(&p->state, state, __ATOMIC_SEQ_CST);
}

#define PROC_AWOKEN(p) PSTATE_IS_AWOKEN(__proc_get_pstate(p))
#define PROC_RUNNING(p) PSTATE_IS_RUNNING(__proc_get_pstate(p))
#define PROC_SLEEPING(p) PSTATE_IS_SLEEPING(__proc_get_pstate(p))
#define PROC_ZOMBIE(p) PSTATE_IS_ZOMBIE(__proc_get_pstate(p))
#define PROC_STOPPED(p) PSTATE_IS_STOPPED(__proc_get_pstate(p))
#define PROC_KILLABLE(p) PSTATE_IS_KILLABLE(__proc_get_pstate(p))
#define PROC_TIMER(p) PSTATE_IS_TIMER(__proc_get_pstate(p))
#define PROC_INTERRUPTIBLE(p) PSTATE_IS_INTERRUPTIBLE(__proc_get_pstate(p))

struct clone_args;

int             proctab_get_pid_proc(int pid, struct proc **pp);
void            exit(int);
int             proc_clone(struct clone_args *args);
void            attach_child(struct proc *parent, struct proc *child);
void            detach_child(struct proc *parent, struct proc *child);
int             kernel_proc_create(const char *name, struct proc **retp, void *entry,
                                   uint64 arg1, uint64 arg2, int stack_order);
struct proc     *allocproc(void *entry, uint64 arg1, uint64 arg2, int kstack_order);
void            freeproc(struct proc *p);
int             growproc(int64);
void            proc_mapstacks(pagetable_t);
int             proc_pagetable(struct proc *);
void            proc_freepagetable(struct proc *);
int             kill(int, int);
int             killed(struct proc*);
void            proc_lock(struct proc *p);
void            proc_unlock(struct proc *p);
void            proc_assert_holding(struct proc *p);
void            procinit(void);
void            sched(void);
void            userinit(void);
void            install_user_root(void);
int             wait(uint64);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);
void            procdump_bt(void);
void            procdump_bt_pid(int pid);
struct proc     *process_switch_to(struct proc *current, struct proc *target);

#endif /* __KERNEL_PROC_H */
