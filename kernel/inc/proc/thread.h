#ifndef __KERNEL_THREAD_H
#define __KERNEL_THREAD_H

#include "compiler.h"
#include "list_type.h"
#include "hlist_type.h"
#include "proc/tq_type.h"
#include "proc/rq_types.h"
#include "trapframe.h"
#include "signal_types.h"
#include "mm/vm_types.h"
#include "vfs/vfs_types.h"
#include <smp/atomic.h>
#include <smp/percpu.h>
#include "lock/rcu_type.h"
#include "proc/thread_types.h"
#include "proc/thread_group.h"

#define THREAD_IS_SLEEPING(state)                                              \
    ({                                                                         \
        (state) == THREAD_INTERRUPTIBLE ||                                     \
            (state) == THREAD_UNINTERRUPTIBLE || (state) == THREAD_KIILABLE || \
            (state) == THREAD_TIMER || (state) == THREAD_KIILABLE_TIMER;       \
    })

#define THREAD_IS_KILLABLE(state)                                              \
    ({                                                                         \
        (state) == THREAD_KIILABLE || (state) == THREAD_KIILABLE_TIMER ||      \
            (state) == THREAD_INTERRUPTIBLE;                                   \
    })

#define THREAD_IS_TIMER(state)                                                 \
    ({                                                                         \
        (state) == THREAD_TIMER || (state) == THREAD_KIILABLE_TIMER ||         \
            (state) == THREAD_INTERRUPTIBLE;                                   \
    })

#define THREAD_IS_INTERRUPTIBLE(state) ({ (state) == THREAD_INTERRUPTIBLE; })

#define THREAD_IS_AWOKEN(state)                                                \
    ({ (state) == THREAD_RUNNING || (state) == THREAD_WAKENING; })

#define THREAD_IS_RUNNING(state) ({ (state) == THREAD_RUNNING; })

#define THREAD_IS_ZOMBIE(state) ({ (state) == THREAD_ZOMBIE; })

#define THREAD_IS_STOPPED(state) ({ (state) == THREAD_STOPPED; })

static inline uint64 thread_flags(struct thread *p) {
    if (p == NULL) {
        return 0;
    }
    return __atomic_load_n(&p->flags, __ATOMIC_SEQ_CST);
}

static inline void thread_flags_set(struct thread *p, uint64 flags) {
    if (p == NULL) {
        return;
    }
    __atomic_or_fetch(&p->flags, flags, __ATOMIC_SEQ_CST);
}

static inline void thread_flags_clear(struct thread *p, uint64 flags) {
    if (p == NULL) {
        return;
    }
    __atomic_and_fetch(&p->flags, ~flags, __ATOMIC_SEQ_CST);
}

#define DEFINE_THREAD_FLAG(__NAME, __VALUE)                                    \
    static inline bool THREAD_##__NAME(struct thread *p) {                     \
        if (p == NULL) {                                                       \
            return false;                                                      \
        }                                                                      \
        uint64 __flags = smp_load_acquire(&p->flags);                          \
        return !!(__flags & (1ULL << (__VALUE)));                              \
    }                                                                          \
    static inline void THREAD_SET_##__NAME(struct thread *p) {                 \
        if (p == NULL) {                                                       \
            return;                                                            \
        }                                                                      \
        __atomic_or_fetch(&p->flags, (1ULL << (__VALUE)), __ATOMIC_SEQ_CST);   \
    }                                                                          \
    static inline void THREAD_CLEAR_##__NAME(struct thread *p) {               \
        if (p == NULL) {                                                       \
            return;                                                            \
        }                                                                      \
        __atomic_and_fetch(&p->flags, ~(1ULL << (__VALUE)), __ATOMIC_SEQ_CST); \
    }

DEFINE_THREAD_FLAG(USER_SPACE, THREAD_FLAG_USER_SPACE)
DEFINE_THREAD_FLAG(VALID, THREAD_FLAG_VALID)
DEFINE_THREAD_FLAG(KILLED, THREAD_FLAG_KILLED)
DEFINE_THREAD_FLAG(ONCHAN, THREAD_FLAG_ONCHAN)
DEFINE_THREAD_FLAG(SIGPENDING, THREAD_FLAG_SIGPENDING)

static inline const char *thread_state_to_str(enum thread_state state) {
    switch (state) {
    case THREAD_UNUSED:
        return "unused";
    case THREAD_USED:
        return "used";
    case THREAD_INTERRUPTIBLE:
        return "interruptible";
    case THREAD_KIILABLE:
        return "killable";
    case THREAD_TIMER:
        return "timer";
    case THREAD_KIILABLE_TIMER:
        return "killable_timer";
    case THREAD_UNINTERRUPTIBLE:
        return "uninterruptible";
    case THREAD_WAKENING:
        return "wakening";
    case THREAD_RUNNING:
        return "running";
    case THREAD_STOPPED:
        return "stopped";
    case THREAD_EXITING:
        return "exiting";
    case THREAD_ZOMBIE:
        return "zombie";
    default:
        return "*unknown";
    }
}

static inline enum thread_state __thread_state_get(struct thread *p) {
    if (p == NULL) {
        return THREAD_UNUSED;
    }
    return __atomic_load_n(&p->state, __ATOMIC_SEQ_CST);
}

static inline void __thread_state_set(struct thread *p,
                                      enum thread_state state) {
    if (p == NULL) {
        return;
    }
    __atomic_store_n(&p->state, state, __ATOMIC_SEQ_CST);
}

#define THREAD_AWOKEN(p) THREAD_IS_AWOKEN(__thread_state_get(p))
#define THREAD_RUNNING(p) THREAD_IS_RUNNING(__thread_state_get(p))
#define THREAD_SLEEPING(p) THREAD_IS_SLEEPING(__thread_state_get(p))
#define THREAD_ZOMBIE(p) THREAD_IS_ZOMBIE(__thread_state_get(p))
#define THREAD_STOPPED(p) THREAD_IS_STOPPED(__thread_state_get(p))
#define THREAD_KILLABLE(p) THREAD_IS_KILLABLE(__thread_state_get(p))
#define THREAD_TIMER(p) THREAD_IS_TIMER(__thread_state_get(p))
#define THREAD_INTERRUPTIBLE(p) THREAD_IS_INTERRUPTIBLE(__thread_state_get(p))

struct clone_args;

int get_pid_thread(int pid, struct thread **pp);
void exit(int);
void vfork_done(struct thread *p);
int thread_clone(struct clone_args *args);
void attach_child(struct thread *parent, struct thread *child);
void detach_child(struct thread *parent, struct thread *child);
struct thread *kthread_create(const char *name, void *entry, uint64 arg1,
                              uint64 arg2, int stack_order);
struct thread *thread_create(void *entry, uint64 arg1, uint64 arg2,
                             int kstack_order);
void thread_destroy(struct thread *p);
void tcb_lock(struct thread *p);
void tcb_unlock(struct thread *p);
void proc_assert_holding(struct thread *p);
void thread_init(void);
void userinit(void);
void install_user_root(void);
int wait(uint64);
void procdump(void);
void procdump_bt(void);
void procdump_bt_pid(int pid);
struct thread *switch_to(struct thread *cur, struct thread *target);

// pid_lock (rwlock) protects the parent-child hierarchy, the proc_table
// hash table, and PID allocation/freeing. It must be acquired before any
// thread's tcb_lock when both are needed, to maintain lock ordering.
// Use pid_rlock/pid_runlock for read-only traversal (e.g., wait scanning,
// procdump). Use pid_wlock/pid_wunlock for mutations (attach/detach child,
// proc table add/remove, PID alloc/free).
void pid_wlock(void);
void pid_wunlock(void);
void pid_rlock(void);
void pid_runlock(void);
bool pid_try_lock_upgrade(void);
bool pid_wholding(void);
void pid_assert_wholding(void);

#endif /* __KERNEL_THREAD_H */
