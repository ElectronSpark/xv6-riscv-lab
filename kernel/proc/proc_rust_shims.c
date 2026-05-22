// Tiny C shim functions called by the Rust port of `kernel/proc/`.
//
// These exist solely to avoid replicating large or fragile C struct
// layouts (struct thread, pgroup, thread_group, session, proc_table,
// hlist_t, rwlock, _Atomic int, bitfields, ...) in Rust just to read
// or mutate a single field. Each shim is a real linker symbol so the
// Rust side can call it via `unsafe extern "C"`.

#include "types.h"
#include "errno.h"
#include "param.h"
#include "string.h"
#include "list.h"
#include "hlist.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/pgroup.h"
#include "proc/sched.h"
#include "proc_private.h"
#include "tty/session.h"
#include "signal.h"
#include "uabi/signal.h"
#include "smp/atomic.h"
#include "smp/percpu.h"
#include "lock/rcu.h"
#include "lock/rwlock.h"
#include "mm/slab.h"

// Forward decls from the moved thread_group.c body (SECTION 15) so that
// earlier sections that still call these functions compile.
struct thread_group;
struct ksiginfo;
void xv6_tgport_thread_group_init(struct thread *initproc);
void xv6_tgport_thread_group_get(struct thread_group *tg);
void xv6_tgport_thread_group_put(struct thread_group *tg);
void xv6_tgport_thread_group_live_dec(struct thread_group *tg);
struct thread_group *xv6_tgport_get_thread_group(pid_t tgid);
void xv6_tgport_tg_shared_pending_init(struct thread_group *tg);
void xv6_tgport_tg_shared_pending_destroy(struct thread_group *tg);
int  xv6_tgport_thread_group_alloc(struct thread *leader);
int  xv6_tgport_thread_group_alloc_kernel(struct thread_group **out_tg, pid_t tgid);
void xv6_tgport_thread_group_add(struct thread_group *tg, struct thread *child);
bool xv6_tgport_thread_group_remove(struct thread *p);
bool xv6_tgport_thread_is_group_leader(struct thread *p);
int  xv6_tgport_thread_tgid(struct thread *p);
void xv6_tgport_thread_group_exit(struct thread *p, int code);
int  xv6_tgport_tg_signal_send(struct thread_group *tg, struct ksiginfo *info);
bool xv6_tgport_tg_signal_pending(struct thread_group *tg, struct thread *p);
struct ksiginfo *xv6_tgport_tg_dequeue_signal(struct thread_group *tg, int signo);
void xv6_tgport_tg_sigpending_empty(struct thread_group *tg, int signo);
void xv6_tgport_tg_recalc_sigpending(struct thread_group *tg);

// Forward decls from the moved thread_queue.c body (SECTION 16).
struct tq;        typedef struct tq tq_t;
struct ttree;     typedef struct ttree ttree_t;
struct tnode;     typedef struct tnode tnode_t;
typedef int (*sleep_callback_t)(void *);
typedef void (*wakeup_callback_t)(void *, int);
void xv6_tqport_tq_init(tq_t *q, const char *name, spinlock_t *lock);
void xv6_tqport_tq_set_lock(tq_t *q, spinlock_t *lock);
void xv6_tqport_ttree_init(ttree_t *q, const char *name, spinlock_t *lock);
void xv6_tqport_ttree_set_lock(ttree_t *q, spinlock_t *lock);
void xv6_tqport_tnode_init(tnode_t *node);
int  xv6_tqport_tq_size(tq_t *q);
int  xv6_tqport_ttree_size(ttree_t *q);
tq_t *xv6_tqport_tnode_get_queue(tnode_t *node);
ttree_t *xv6_tqport_tnode_get_tree(tnode_t *node);
struct thread *xv6_tqport_tnode_get_thread(tnode_t *node);
int  xv6_tqport_tnode_get_errno(tnode_t *node, int *error_no);
int  xv6_tqport_tq_push(tq_t *q, tnode_t *node);
tnode_t *xv6_tqport_tq_first(tq_t *q);
tnode_t *xv6_tqport_tq_pop(tq_t *q);
int  xv6_tqport_tq_remove(tq_t *q, tnode_t *node);
int  xv6_tqport_tq_bulk_move(tq_t *to, tq_t *from);
int  xv6_tqport_ttree_add(ttree_t *q, tnode_t *node);
tnode_t *xv6_tqport_ttree_first(ttree_t *q);
int  xv6_tqport_ttree_key_min(ttree_t *q, uint64 *key);
int  xv6_tqport_ttree_remove(ttree_t *q, tnode_t *node);
int  xv6_tqport_tq_wait(tq_t *q, spinlock_t *lock, uint64 *rdata);
int  xv6_tqport_tq_wait_cb(tq_t *q, sleep_callback_t sc, wakeup_callback_t wc,
                           void *arg, uint64 *rdata);
struct thread *xv6_tqport_tq_wakeup(tq_t *q, int error_no, uint64 rdata);
int  xv6_tqport_tq_wakeup_all(tq_t *q, int error_no, uint64 rdata);
int  xv6_tqport_ttree_wait(ttree_t *q, uint64 key, spinlock_t *lock, uint64 *rdata);
int  xv6_tqport_ttree_wait_cb(ttree_t *q, uint64 key, sleep_callback_t sc,
                              wakeup_callback_t wc, void *arg, uint64 *rdata);
struct thread *xv6_tqport_ttree_wakeup_one(ttree_t *q, uint64 key, int error_no,
                                            uint64 rdata);
int  xv6_tqport_ttree_wakeup_key(ttree_t *q, uint64 key, int error_no, uint64 rdata);
int  xv6_tqport_ttree_wakeup_all(ttree_t *q, int error_no, uint64 rdata);

// Forward decls from the moved thread.c body (SECTION 17).
struct clone_args;
void xv6_thport_tcb_lock(struct thread *p);
void xv6_thport_tcb_unlock(struct thread *p);
void xv6_thport_proc_assert_holding(struct thread *p);
void xv6_thport_thread_init(void);
void xv6_thport_attach_child(struct thread *parent, struct thread *child);
void xv6_thport_detach_child(struct thread *parent, struct thread *child);
struct thread *xv6_thport_thread_create(void *entry, uint64 arg1, uint64 arg2,
                                        int kstack_order);
struct thread *xv6_thport_kthread_create(const char *name, void *entry,
                                          uint64 arg1, uint64 arg2,
                                          int stack_order);
void xv6_thport_idle_thread_init(void);
void xv6_thport_thread_destroy(struct thread *p);
void xv6_thport_userinit(void);
void xv6_thport_install_user_root(void);

// Forward decls from the moved sched.c body (SECTION 18).
enum thread_state;
int  xv6_schport_chan_holding(void);
void xv6_schport_sleep_lock(void);
void xv6_schport_sleep_unlock(void);
int  xv6_schport_sleep_lock_irqsave(void);
void xv6_schport_sleep_unlock_irqrestore(int state);
int  xv6_schport_sched_holding(void);
void xv6_schport_scheduler_init(void);
struct thread *xv6_schport_switch_to(struct thread *cur, struct thread *target);
void xv6_schport_scheduler_yield(void);
void xv6_schport_scheduler_sleep(spinlock_t *lk, enum thread_state s);
void xv6_schport_scheduler_wakeup(struct thread *p);
void xv6_schport_scheduler_wakeup_timeout(struct thread *p);
void xv6_schport_scheduler_wakeup_killable(struct thread *p);
void xv6_schport_scheduler_wakeup_interruptible(struct thread *p);
void xv6_schport_scheduler_wakeup_stopped(struct thread *p);
void xv6_schport_sleep_on_chan(void *chan, spinlock_t *lk);
int  xv6_schport_sleep_on_chan_interruptible(void *chan, spinlock_t *lk);
void xv6_schport_wakeup_on_chan(void *chan);
void xv6_schport_scheduler_dump_chan_queue(void);
void xv6_schport_wakeup(struct thread *p);
void xv6_schport_wakeup_timeout(struct thread *p);
void xv6_schport_wakeup_killable(struct thread *p);
void xv6_schport_wakeup_interruptible(struct thread *p);
uint64 xv6_schport_sys_dumpchan(void);
void xv6_schport_context_switch_prepare(struct thread *prev, struct thread *next);
void xv6_schport_context_switch_finish(struct thread *prev, struct thread *next, int intr);

// Forward decls from the moved rq.c body (SECTION 19).
struct rq;
struct rq_percpu;
struct sched_entity;
struct sched_class;
struct sched_attr;
typedef uint64 _rqport_cpumask_t;
bool xv6_rqport_rq_is_initialized(void);
void xv6_rqport_rq_set_ready(int cls_id, int cpu_id);
void xv6_rqport_rq_clear_ready(int cls_id, int cpu_id);
struct rq *xv6_rqport_get_rq_for_cpu(int cls_id, int cpu_id);
struct rq *xv6_rqport_pick_next_rq(void);
void xv6_rqport_rq_global_init(void);
void xv6_rqport_rq_init(struct rq *rq);
void xv6_rqport_rq_register(struct rq *rq, int cls_id, int cpu_id);
void xv6_rqport_sched_entity_init(struct sched_entity *se, struct thread *p);
void xv6_rqport_sched_class_register(int id, struct sched_class *cls);
void xv6_rqport_rq_lock(int cpu_id);
int  xv6_rqport_rq_trylock(int cpu_id);
void xv6_rqport_rq_unlock(int cpu_id);
int  xv6_rqport_rq_lock_irqsave(int cpu_id);
void xv6_rqport_rq_unlock_irqrestore(int cpu_id, int state);
int  xv6_rqport_rq_lock_current_irqsave(void);
void xv6_rqport_rq_unlock_current_irqrestore(int state);
void xv6_rqport_rq_lock_two(int cpu_id1, int cpu_id2);
int  xv6_rqport_rq_trylock_two(int cpu_id1, int cpu_id2);
void xv6_rqport_rq_unlock_two(int cpu_id1, int cpu_id2);
void xv6_rqport_rq_lock_current(void);
void xv6_rqport_rq_unlock_current(void);
int  xv6_rqport_rq_holding(int cpu_id);
int  xv6_rqport_rq_holding_current(void);
struct rq_percpu *xv6_rqport_rq_percpu_lock_get(int cpu_id);
struct rq_percpu *xv6_rqport_rq_percpu_lock_get_current(void);
void xv6_rqport_rq_percpu_put_unlock(struct rq_percpu *rq_pc);
struct rq *xv6_rqport_rq_select_task_rq(struct sched_entity *se, _rqport_cpumask_t mask);
void xv6_rqport_rq_enqueue_task(struct rq *rq, struct sched_entity *se);
void xv6_rqport_rq_dequeue_task(struct rq *rq, struct sched_entity *se);
struct sched_entity *xv6_rqport_rq_pick_next_task(struct rq *rq);
void xv6_rqport_rq_put_prev_task(struct sched_entity *se);
void xv6_rqport_rq_set_next_task(struct sched_entity *se);
bool xv6_rqport_rq_cpu_allowed(struct sched_entity *se, int cpu_id);
void xv6_rqport_rq_task_tick(struct sched_entity *se);
void xv6_rqport_rq_task_fork(struct sched_entity *se);
void xv6_rqport_rq_task_dead(struct sched_entity *se);
void xv6_rqport_rq_yield_task(void);
bool xv6_rqport_rq_cpu_is_idle(int cpu_id);
int  xv6_rqport_rq_add_wake_list(int cpu_id, struct sched_entity *se);
struct sched_entity *xv6_rqport_rq_pop_all_wake_list(struct rq_percpu *rq_pc);
void xv6_rqport_rq_flush_wake_list(int cpu_id);
void xv6_rqport_sched_attr_init(struct sched_attr *attr);
int  xv6_rqport_sched_getattr(struct sched_entity *se, struct sched_attr *attr);
int  xv6_rqport_sched_setattr(struct sched_entity *se, const struct sched_attr *attr);
void xv6_rqport_rq_cpu_activate(int cpu);
uint64 xv6_rqport_rq_get_active_cpu_mask(void);
void xv6_rqport_rq_dump(void);
uint64 xv6_rqport_sys_dumprq(void);

// Forward decls from the moved signal.c body (SECTION 20).
struct sigaction;
struct thread_signal;
bool xv6_sigport_recalc_sigpending_tsk(struct thread *p);
void xv6_sigport_recalc_sigpending(void);
void xv6_sigport_sigpending_init(struct thread *p);
void xv6_sigport_sigpending_destroy(struct thread *p);
void xv6_sigport_sigpending_clone(struct thread_signal *dst, struct thread_signal *src,
                                  uint64 clone_flags, int esignal);
void xv6_sigport_sigstack_init(stack_t *stack);
void xv6_sigport_sigacts_lock(sigacts_t *sa);
void xv6_sigport_sigacts_unlock(sigacts_t *sa);
int  xv6_sigport_sigacts_holding(sigacts_t *sa);
void xv6_sigport_ksiginfo_free(ksiginfo_t *ksi);
int  xv6_sigport_sigpending_empty(struct thread *p, int signo);
void xv6_sigport_sigacts_exec(sigacts_t *sa);
void xv6_sigport_sigacts_put(sigacts_t *sa);
void xv6_sigport_signal_init(void);
int  xv6_sigport___signal_send(struct thread *p, ksiginfo_t *info);
int  xv6_sigport_signal_send(int pid, ksiginfo_t *info);
bool xv6_sigport_signal_pending(struct thread *p);
bool xv6_sigport_signal_pending_locked(struct thread *p, sigacts_t *sa);
int  xv6_sigport_signal_notify(struct thread *p);
bool xv6_sigport_signal_terminated(struct thread *p);
bool xv6_sigport_signal_test_clear_stopped(struct thread *p);
int  xv6_sigport_signal_restore(struct thread *p, ucontext_t *context);
int  xv6_sigport_sigaction(int signum, struct sigaction *act, struct sigaction *oldact);
int  xv6_sigport_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int  xv6_sigport_sigpending(struct thread *p, sigset_t *set);
int  xv6_sigport_sigreturn(void);
void xv6_sigport_handle_signal(void);
int  xv6_sigport_kill(int pid, int signum);
int  xv6_sigport_kill_thread(struct thread *p, int signum);
int  xv6_sigport_tgkill(int tgid, int tid, int signum);
int  xv6_sigport_tkill(int tid, int signum);
int  xv6_sigport_killed(struct thread *p);
int  xv6_sigport_kill_from_kernel(int pid, int signum);
int  xv6_sigport_kill_proc(struct thread *p, int signum);
int  xv6_sigport_sigsuspend(const sigset_t *mask);
int  xv6_sigport_sigwait(const sigset_t *set, int *sig);

// Forward decl from defs.h (defs.h pulls in riscv.h which breaks here).
struct context;
void print_thread_backtrace(struct context *ctx, uint64 kstack,
                            int kstack_order);

// ===========================================================================
// SECTION 1: misc helpers used by sched_idle.rs / sched_fifo.rs / signal.rs
// ===========================================================================

// thread_sched_entity / xv6_thread_state_set / xv6_thread_state_get /
// xv6_sizeof_sigaction / xv6_is_err / xv6_err_ptr / xv6_ptr_err PORTED to
// proc_shims.rs.

// xv6_current_thread PORTED to proc_shims.rs.

_Static_assert(sizeof(struct sigaction) == 24,
               "struct sigaction layout drift; update Rust SIZEOF_SIGACTION");

// ===========================================================================
// SECTION 2: struct thread field accessors
// ===========================================================================

// SECTION 2 simple field accessors PORTED to proc_shims.rs.
// t_pg_entry_*, t_user_space, t_for_each_child, t_dmp_list_entry_is_detached
// PORTED to proc_shims.rs.

// xv6_thread_state_short / xv6_thread_state_to_str PORTED to proc_shims.rs.

// ===========================================================================
// SECTION 3: struct pgroup field accessors
// ===========================================================================

// SECTION 3 simple field accessors PORTED to proc_shims.rs.
// SECTION 3 remaining accessors PORTED to proc_shims.rs.

// ===========================================================================
// SECTION 4: struct thread_group accessors
// ===========================================================================

// tg_pgroup, tg_tgid PORTED to proc_shims.rs.
// SECTION 4 remaining accessors PORTED to proc_shims.rs.
// tg_set_pgroup PORTED to proc_shims.rs.
// tg_is_group_leader PORTED to proc_shims.rs.

// ===========================================================================
// SECTION 5: struct session accessors
// ===========================================================================

// SECTION 5 simple field accessors PORTED to proc_shims.rs.

// session_for_each_pg PORTED to proc_shims.rs.
// session_for_each_all PORTED to proc_shims.rs.

// ===========================================================================
// SECTION 6: pgroup slab cache — PORTED to kernel/proc/proc_shims.rs.
// ===========================================================================

// ===========================================================================
// SECTION 7: ksiginfo zero-fill helper — PORTED to kernel/proc/proc_shims.rs.
// ===========================================================================

// ===========================================================================
// SECTION 8: proc_table storage + most accessors — PORTED to
// kernel/proc/proc_shims.rs. Only the two foreach helpers remain here
// because they rely on the `hlist_foreach_node_rcu` macro (which in turn
// uses static-inline `hlist_first_entry_rcu` / `hlist_next_entry_rcu`).
// The Rust side owns the proc_table storage; we reach it through the
// `xv6_proctab_hlist_ptr` accessor exported by proc_shims.rs.
// ===========================================================================

extern hlist_t *xv6_proctab_hlist_ptr(void);

void xv6_proctab_foreach_rcu(void (*fn)(struct thread *, void *), void *arg) {
    struct thread *p;
    hlist_t *hl = xv6_proctab_hlist_ptr();
    rcu_read_lock();
    hlist_foreach_node_rcu(hl, p, proctab_entry) {
        fn(p, arg);
    }
    rcu_read_unlock();
}
void xv6_proctab_foreach_inner(void (*fn)(struct thread *, void *), void *arg) {
    struct thread *p;
    hlist_t *hl = xv6_proctab_hlist_ptr();
    hlist_foreach_node_rcu(hl, p, proctab_entry) {
        fn(p, arg);
    }
}

// ===========================================================================
// SECTION 9: RCU + tcb_lock wrappers — PORTED to kernel/proc/proc_shims.rs.
// ===========================================================================

// ===========================================================================
// SECTION 10: procdump print helpers — PORTED to kernel/proc/proc_shims.rs.
// Only xv6_panic remains here (it calls the `panic` macro which expands to
// a __FILE__/__LINE__ string literal — easiest to keep in C).
// ===========================================================================

__attribute__((noreturn)) void xv6_panic(const char *msg) {
    panic("%s", msg);
}

// ===========================================================================
// SECTION 11: clone.c support
// ===========================================================================

#include "clone_flags.h"
#include "trapframe.h"

// SECTION 11 simple field accessors PORTED to proc_shims.rs.
// xv6_t_set_user_space PORTED to proc_shims.rs.
// xv6_t_copy_name PORTED to proc_shims.rs.

// forkret_entry needs to call `current` and various macros after
// xv6_schport_context_switch_finish. Bundle the post-CSF tail into one helper so
// Rust just orchestrates the high-level flow.
// xv6_thread_from_context PORTED to proc_shims.rs.
// xv6_mycpu_clear_noff PORTED to proc_shims.rs.
// xv6_intr_on / xv6_smp_mb PORTED to proc_shims.rs.

// xv6_forkret_assert_user PORTED to proc_shims.rs.

// ===========================================================================
// SECTION 12: exit.c support
// ===========================================================================

#include "uabi/wait.h"
#include "uabi/signo.h"
#include "mm/vm.h"
#include "vfs/file.h"
// Forward decls — vfs/fs.h uses spin_lock as a static inline body.
struct spinlock;
void spin_lock(struct spinlock *);
void spin_unlock(struct spinlock *);
#include "vfs/fs.h"

// SECTION 12 simple field accessors PORTED to proc_shims.rs.
// xv6_t_set_self_reap PORTED to proc_shims.rs.
// xv6_thread_is_zombie / xv6_thread_is_stopped PORTED to proc_shims.rs.
// xv6_t_xstate / xv6_tg_is_exiting PORTED to proc_shims.rs.
// xv6_cpu_relax / xv6_either_copyout_int PORTED to proc_shims.rs.

// xv6_exit_reparent_do PORTED to proc_shims.rs.

// xv6_exit_find_zombie_child PORTED to proc_shims.rs.
// xv6_exit_find_stopped_child PORTED to proc_shims.rs.

// xv6_exit_reap_zombie PORTED to proc_shims.rs.


// ===========================================================================
// SECTION 13: workqueue port — PORTED to Rust in kernel/proc/workqueue.rs.
// The Rust file owns `xv6_wq_pub_*` and the canonical ABI names.
// ===========================================================================

// (workqueue C body removed — see workqueue.rs)
// SECTION 14: rq_test port — PORTED to Rust in kernel/proc/rq_test.rs.
// ===========================================================================
// (rq_test C body removed — see rq_test.rs)

// SECTION 15: thread_group port — PORTED to Rust in kernel/proc/thread_group.rs.
// ===========================================================================
// (thread_group C body removed — see thread_group.rs)
// SECTION 16: thread_queue port — PORTED to Rust in kernel/proc/thread_queue.rs.
// ===========================================================================
// (thread_queue C body removed — see thread_queue.rs)
// SECTION 17: thread.c port — PORTED to Rust in kernel/proc/thread.rs.
// ===========================================================================
// Small C helpers needed by thread.rs (struct accessors bindgen cannot see).
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "signal.h"
#include "vfs/fs.h"
#include "defs.h"
void pgroup_mark_kernel(struct pgroup *pg) { pg->is_kernel = 1; }
void session_mark_kernel(struct session *s) { s->is_kernel = 1; }
void sigstack_init_for_thread(struct thread *p) { xv6_sigport_sigstack_init(&p->signal.sig_stack); }
void install_user_root_finish(struct thread *p, struct vfs_inode *root_inode) {
    struct vfs_inode_ref cwd_ref;
    int ret = vfs_inode_get_ref(root_inode, &cwd_ref);
    if (ret < 0) { panic("install_user_root: failed to get ref to root inode"); }
    vfs_struct_lock(p->fs); p->fs->cwd = cwd_ref; vfs_struct_unlock(p->fs);
    vfs_iput(root_inode);
}
// ============================================================
// SECTION 20: signal.c port (renamed xv6_sigport_*)
// ============================================================
/*
 * Signal handling for xv6
 *
 * LOCKING:
 * Signal operations use a unified lock approach (like Linux sighand->siglock).
 * All signal state is protected by sigacts->lock:
 *   - Signal actions (sigacts->sa[])
 *   - Per-thread signal masks (thread->signal.sig_mask, sig_saved_mask)
 *   - Per-thread pending signals (thread->signal.sig_pending_mask,
 * sig_pending[])
 *
 * Key rules:
 * - sigacts->lock must be held when reading/writing any signal state
 * - Release sigacts->lock BEFORE scheduler operations (wakeup, yield)
 * - Copy data from protected structures before releasing lock if needed after
 *
 * This is simpler than the old two-lock (tcb_lock + xv6_sigport_sigacts_lock) approach
 * and matches Linux's design where sighand->siglock is THE signal lock.
 *
 * The THREAD_FLAG_SIGPENDING flag provides O(1) checks for pending signals.
 * xv6_sigport_recalc_sigpending_tsk() updates this flag and must be called after any
 * change to signal.sig_pending_mask or signal.sig_mask.
 */

#include "types.h"
#include "string.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc_private.h"
#include "signal.h"
#include "lock/rcu.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include <mm/slab.h>
#include "proc/sched.h"
#include "list.h"
#include "bits.h"
#include "smp/ipi.h"
#include "clone_flags.h"
#include "errno.h"
#include "proc/pgroup.h"

static slab_cache_t __sigacts_pool;
static slab_cache_t __ksiginfo_pool;

// Forward declarations for helper functions
static void sigacts_assert_holding(sigacts_t *sa);

sig_defact signo_default_action(int signo) {
    switch (signo) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
        return SIG_ACT_IGN;
    case SIGALRM:
    case SIGUSR1:
    case SIGUSR2:
    // case SIGCLD:
    // case SIGEMT:
    case SIGHUP:
    case SIGINT:
    case SIGIO:
    case SIGKILL:
    // case SIGLOST:
    case SIGPIPE:
    // case SIGPOLL:
    case SIGPROF:
    case SIGPWR:
    case SIGSTKFLT:
    case SIGTERM:
    case SIGVTALRM:
        return SIG_ACT_TERM;
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return SIG_ACT_STOP;
    case SIGCONT:
        return SIG_ACT_CONT;
    case SIGABRT:
    case SIGBUS:
    case SIGILL:
    // case SIGIOT:
    case SIGQUIT:
    case SIGSEGV:
    case SIGSYS:
    case SIGTRAP:
    // case SIGUNUSED
    case SIGXCPU:
    case SIGXFSZ:
    case SIGFPE:
        return SIG_ACT_CORE;
    default:
        return SIG_ACT_INVALID; // Invalid signal number
    }
}

#define SIG_MANDATORY_MASK (SIGNO_MASK(SIGKILL) | SIGNO_MASK(SIGSTOP))

/*
 * Recalculate the TIF_SIGPENDING flag for a task.
 *
 * This checks if there are any pending signals that are not blocked.
 * If so, the SIGPENDING flag is set; otherwise it's cleared.
 *
 * Following Linux's approach:
 * - Set flag if pending & ~blocked has any bits set
 * - Returns true if flag was set, false otherwise
 *
 * Caller must hold xv6_sigport_sigacts_lock (or ensure sigacts won't change).
 */
bool xv6_sigport_recalc_sigpending_tsk(struct thread *p) {
    if (!p || !p->sigacts) {
        return false;
    }

    sigset_t pending = smp_load_acquire(&p->signal.sig_pending_mask);
    sigset_t blocked = p->signal.sig_mask;

    // Also check thread group shared pending signals
    if (p->thread_group != NULL) {
        sigset_t shared =
            smp_load_acquire(&p->thread_group->shared_pending.sig_pending_mask);
        pending |= shared;
    }

    if ((pending & ~blocked) != 0) {
        THREAD_SET_SIGPENDING(p);
        return true;
    }

    /*
     * We must never clear the flag in another thread, or in current
     * when it's possible the current syscall is returning -ERESTART*.
     * So we only clear it for the current process.
     */
    return false;
}

/*
 * Recalculate TIF_SIGPENDING for the current process.
 * This can clear the flag if no signals are pending.
 */
void xv6_sigport_recalc_sigpending(void) {
    struct thread *p = current;
    if (!p || !p->sigacts) {
        return;
    }

    sigacts_t *sa = p->sigacts;
    xv6_sigport_sigacts_lock(sa);
    if (!xv6_sigport_recalc_sigpending_tsk(p)) {
        // No pending signals, safe to clear flag for current process
        THREAD_CLEAR_SIGPENDING(p);
    }
    xv6_sigport_sigacts_unlock(sa);
}

void xv6_sigport_sigpending_init(struct thread *p) {
    if (!p) {
        return; // Invalid pointer
    }
    for (int i = 0; i < NSIG; i++) {
        sigpending_t *sq = &p->signal.sig_pending[i];
        list_entry_init(&sq->queue);
    }
}

void xv6_sigport_sigpending_destroy(struct thread *p) {
    if (!p) {
        return; // Invalid pointer
    }
    // Called at process exit - sigacts should already be locked or no longer
    // shared
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }
    // Ensure all per-signal queues are already empty. Do NOT silently purge
    // here.
    for (int i = 1; i <= NSIG; i++) {
        sigpending_t *sq = &p->signal.sig_pending[i - 1];
        assert(LIST_IS_EMPTY(&sq->queue),
               "xv6_sigport_sigpending_destroy: pending signals not empty for signal %d",
               i);
    }
    assert(p->signal.sig_pending_mask == 0,
           "xv6_sigport_sigpending_destroy: pending mask not zero");
}

// Copy pending signal state from src to dst during fork/clone.
// Will suppose the caller is holding sigacts lock
void xv6_sigport_sigpending_clone(struct thread_signal *dst, struct thread_signal *src,
                      uint64 clone_flags, int esignal) {
    // Copy per-thread signal mask from parent
    dst->sig_mask = src->sig_mask;
    dst->sig_saved_mask = src->sig_saved_mask;

    if (clone_flags & CLONE_THREAD) {
        // For CLONE_THREAD, the child does not send a signal to the parent
        // on exit (Linux behavior). The exit signal is 0.
        dst->esignal = 0;
    } else {
        // signal to be sent to parent on exit
        dst->esignal = esignal;
    }
}

void xv6_sigport_sigstack_init(stack_t *stack) {
    if (!stack) {
        return; // Invalid pointer
    }
    stack->ss_sp = NULL; // No stack allocated yet
    stack->ss_flags = SS_DISABLE;
    stack->ss_size = 0; // Size is zero initially
}

ksiginfo_t *ksiginfo_alloc(void) {
    ksiginfo_t *ksi = slab_alloc(&__ksiginfo_pool);
    if (!ksi) {
        return NULL; // Allocation failed
    }
    memset(ksi, 0, sizeof(ksiginfo_t));
    list_entry_init(&ksi->list_entry);
    ksi->sender = NULL; // No sender initially
    return ksi;
}

void xv6_sigport_sigacts_lock(sigacts_t *sa) { spin_lock(&sa->lock); }

void xv6_sigport_sigacts_unlock(sigacts_t *sa) { spin_unlock(&sa->lock); }

int xv6_sigport_sigacts_holding(sigacts_t *sa) { return spin_holding(&sa->lock); }

static void sigacts_assert_holding(sigacts_t *sa) {
    assert(xv6_sigport_sigacts_holding(sa), "sigacts lock not held");
}

void xv6_sigport_ksiginfo_free(ksiginfo_t *ksi) {
    if (ksi) {
        slab_free(ksi);
    }
}

// Clean the signal queue of the given process for the specified signal number.
// If signo is 0, all signals in the queue are cleaned.
// Ksiginfo being cleaned will be freed.
// The caller must hold sigacts->lock.
// Returns 0 on success, -1 on error.
int xv6_sigport_sigpending_empty(struct thread *p, int signo) {
    if (!p) {
        return -EINVAL; // Invalid process pointer
    }
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }

    if (signo == 0) {
        // Purge all signal queues (signals numbered 1..NSIG map to index
        // signo-1)
        for (int i = 1; i <= NSIG; i++) {
            ksiginfo_t *ksi = NULL;
            ksiginfo_t *tmp = NULL;
            sigpending_t *sq = &p->signal.sig_pending[i - 1];
            list_foreach_node_safe(&sq->queue, ksi, tmp, list_entry) {
                list_node_detach(ksi, list_entry);
                xv6_sigport_ksiginfo_free(ksi);
            }
        }
        p->signal.sig_pending_mask = 0;
        // Update xv6_sigport_sigpending flag after clearing all pending signals
        THREAD_CLEAR_SIGPENDING(p);
        return 0;
    }

    if (SIGBAD(signo)) {
        return -EINVAL; // Invalid signal number
    }

    ksiginfo_t *ksi = NULL;
    ksiginfo_t *tmp = NULL;
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    list_foreach_node_safe(&sq->queue, ksi, tmp, list_entry) {
        list_node_detach(ksi, list_entry);
        xv6_sigport_ksiginfo_free(ksi); // Free the ksiginfo after removing it
    }
    sigdelset(&p->signal.sig_pending_mask, signo);
    // Update xv6_sigport_sigpending flag after modifying pending mask (caller already holds
    // sigacts lock)
    xv6_sigport_recalc_sigpending_tsk(p);
    return 0;
}

static void __sig_reset_act_mask(sigacts_t *sa, int signo) {
    sigdelset(&sa->sa_sigterm, signo);
    sigdelset(&sa->sa_sigignore, signo);
    // sigdelset(&sa->sa_usercatch, signo);
    if (signo != SIGSTOP) {
        sigdelset(&sa->sa_sigstop, signo);
    }
    if (signo != SIGCONT) {
        sigdelset(&sa->sa_sigcont, signo);
    }
    // sigdelset(&sa->sa_sigcore, signo);
}

static int __sig_setdefault(sigacts_t *sa, int signo) {
    if (!sa || SIGBAD(signo)) {
        return -EINVAL; // Invalid signal number or signal actions
    }
    sig_defact defact = signo_default_action(signo);
    if (defact == SIG_ACT_INVALID) {
        return 0; // Ignore invalid signal number
    }

    __sig_reset_act_mask(sa, signo);
    switch (defact) {
    case SIG_ACT_IGN:
        sigaddset(&sa->sa_sigignore, signo);
        break;
    case SIG_ACT_CONT:
        sigaddset(&sa->sa_sigcont, signo);
        break;
    case SIG_ACT_STOP:
        sigaddset(&sa->sa_sigstop, signo);
        break;
    case SIG_ACT_TERM:
    // @TODO: For now handle SIG_ACT_CORE and SIG_ACT_INVALID by terminating the
    // process
    case SIG_ACT_CORE:
    case SIG_ACT_INVALID:
        sigaddset(&sa->sa_sigterm, signo);
        break;
    default:
        return -EINVAL;
    }

    sa->sa[signo].sa_handler = SIG_DFL;
    sa->sa[signo].sa_flags = 0;            // Reset flags
    sigemptyset(&(sa->sa[signo].sa_mask)); // Reset signal mask
    return 0;
}

// Initialize the first signal actions
sigacts_t *sigacts_init(void) {
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (!sa) {
        return NULL;
    }
    memset(sa, 0, sizeof(sigacts_t));
    sigemptyset(&sa->sa_sigterm);
    sigemptyset(&sa->sa_sigstop);
    sigemptyset(&sa->sa_sigcont);
    sigemptyset(&sa->sa_sigignore);
    spin_init(&sa->lock, "xv6_sigport_sigacts_lock");
    sa->refcount = 1;

    for (int i = 1; i <= NSIG; i++) {
        assert(__sig_setdefault(sa, i) == 0,
               "sigacts_init: failed to set default action for signal %d", i);
    }
    return sa;
}

/*
 * Reset signal dispositions for exec (POSIX compliance).
 *
 * After exec, caught signals (sa_handler != SIG_DFL && sa_handler != SIG_IGN)
 * must be reset to SIG_DFL because the old handler function no longer exists
 * in the new address space.  Ignored signals remain ignored.  Default signals
 * remain default.
 */
void xv6_sigport_sigacts_exec(sigacts_t *sa) {
    if (!sa)
        return;
    xv6_sigport_sigacts_lock(sa);
    for (int i = 1; i <= NSIG; i++) {
        if (sa->sa[i].sa_handler != SIG_DFL &&
            sa->sa[i].sa_handler != SIG_IGN) {
            __sig_setdefault(sa, i);
        }
        /* Also clear SA_NOCLDWAIT on exec (Linux behaviour) */
        sa->sa[i].sa_flags = 0;
        sigemptyset(&sa->sa[i].sa_mask);
    }
    xv6_sigport_sigacts_unlock(sa);
}

sigacts_t *sigacts_dup(sigacts_t *psa, uint64 clone_flags) {
    if (!psa) {
        return NULL;
    }
    if (clone_flags & CLONE_SIGHAND) {
        // Share the signal actions
        // simply increase the reference count
        atomic_inc(&psa->refcount);
        return psa;
    }
    sigacts_t *sa = slab_alloc(&__sigacts_pool);
    if (sa) {
        xv6_sigport_sigacts_lock(psa);
        memmove(sa, psa, sizeof(sigacts_t));
        xv6_sigport_sigacts_unlock(psa);

        // CRITICAL: Reinitialize the lock and refcount after copying!
        // memmove copies the locked spinlock state, which would make
        // the new sigacts appear to be locked by someone else.
        spin_init(&sa->lock, "xv6_sigport_sigacts_lock");
        sa->refcount = 1;
    }
    return sa;
}

void xv6_sigport_sigacts_put(sigacts_t *sa) {
    if (sa != NULL && !atomic_dec_unless(&sa->refcount, 1)) {
        slab_free(sa);
    }
}

void xv6_sigport_signal_init(void) {
    slab_cache_init(&__sigacts_pool, "sigacts", sizeof(sigacts_t),
                    SLAB_FLAG_STATIC);
    slab_cache_init(&__ksiginfo_pool, "ksiginfo", sizeof(ksiginfo_t),
                    SLAB_FLAG_STATIC);
}

// Cap for number of queued ksiginfo entries per signal when SA_SIGINFO set.
#define MAX_SIGINFO_PER_SIGNAL 8

// Helper: count ksiginfo entries currently queued for a signal.
static int __siginfo_queue_len(struct thread *p, int signo) {
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    int n = 0;
    ksiginfo_t *pos = NULL;
    ksiginfo_t *tmp = NULL;
    list_foreach_node_safe(&sq->queue, pos, tmp, list_entry) { n++; }
    return n;
}

int xv6_sigport___signal_send(struct thread *p, ksiginfo_t *info) {
    if (p == NULL || info == NULL) {
        return -EINVAL; // No threads available
    }
    if (SIGBAD(info->signo)) {
        return -EINVAL; // Invalid thread or signal number
    }

    // Check thread validity - use atomic load for lockless initial check
    enum thread_state pstate = __thread_state_get(p);
    if (pstate == THREAD_UNUSED || pstate == THREAD_ZOMBIE ||
        THREAD_KILLED(p)) {
        return -ESRCH; // Thread is not valid or already xv6_sigport_killed
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return -EINVAL; // No signal actions available
    }

    // Lock sigacts - this is the unified signal lock
    xv6_sigport_sigacts_lock(sa);

    // ignored signals are not sent
    if (sigismember(&sa->sa_sigignore, info->signo)) {
        xv6_sigport_sigacts_unlock(sa);
        return 0;
    }

    sigaction_t *act = &sa->sa[info->signo];
    if (act->sa_flags & SA_SIGINFO) {
        assert(info->signo != SIGKILL && info->signo != SIGSTOP,
               "xv6_sigport_signal_send: SA_SIGINFO set for SIGKILL or SIGSTOP");
        // Enforce per-signal queue cap. If cap reached, drop oldest entry.
        int qlen = __siginfo_queue_len(p, info->signo);
        if (qlen >= MAX_SIGINFO_PER_SIGNAL) {
            // Drop head (oldest) then append new info to tail.
            sigpending_t *sq = &p->signal.sig_pending[info->signo - 1];
            if (!LIST_IS_EMPTY(&sq->queue)) {
                ksiginfo_t *old =
                    LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
                if (old) {
                    list_entry_detach(&old->list_entry);
                    xv6_sigport_ksiginfo_free(old);
                }
            }
        }
        ksiginfo_t *ksi = ksiginfo_alloc();
        if (!ksi) {
            // Allocation failed: keep non-RT semantics by setting the pending
            // bit below, but skip queuing siginfo payload.
            goto after_enqueue; // fall through to set pending bit & notify
        }
        *ksi = *info; // Copy the signal info
        list_entry_init(&ksi->list_entry);
        // Add to pending queue
        list_node_push_back(&p->signal.sig_pending[info->signo - 1].queue, ksi,
                       list_entry);
    }

after_enqueue:
    // Always record the signal as pending (even for stop signals) to allow
    // later logic (e.g., mask changes) to notice it.
    sigaddset(&p->signal.sig_pending_mask, info->signo);

    // Update xv6_sigport_sigpending flag after adding to pending mask
    xv6_sigport_recalc_sigpending_tsk(p);

    bool is_stop = sigismember(&sa->sa_sigstop, info->signo) &&
                   !sigismember(&p->signal.sig_mask, info->signo);
    bool is_cont = sigismember(&sa->sa_sigcont, info->signo) &&
                   !sigismember(&p->signal.sig_mask, info->signo);
    bool is_term = sigismember(&sa->sa_sigterm, info->signo);
    sigset_t sigmask = p->signal.sig_mask;

    // Release sigacts lock before scheduler operations
    xv6_sigport_sigacts_unlock(sa);

    // For scheduler operations, we need tcb_lock to check/modify state
    if (is_stop) {
        // Stop signals: The thread will enter THREAD_STOPPED voluntarily when
        // it Process signals in xv6_sigport_handle_signal(). If it's currently sleeping in
        // an interruptible state, wake it up so it can process the stop signal.
        tcb_lock(p);
        pstate = __thread_state_get(p);
        if (THREAD_IS_INTERRUPTIBLE(pstate)) {
            // Wake up interruptible sleeper so it can handle the stop signal
            tcb_unlock(p);
            scheduler_wakeup(p);
        } else if (pstate == THREAD_RUNNING) {
            tcb_unlock(p);
            // Thread is running, send IPI so it handles the stop signal
            // promptly
            int target_cpu = smp_load_acquire(&p->sched_entity->cpu_id);
            if (target_cpu != cpuid()) {
                ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
            } else {
                SET_NEEDS_RESCHED();
            }
        } else {
            tcb_unlock(p);
        }
        // If uninterruptible, the thread will handle the stop signal when it
        // wakes up
    }
    if (is_cont) {
        // Continue signal: Wake up the thread from THREAD_STOPPED state.
        scheduler_wakeup_stopped(p);
    }

    // If the action is to terminate the thread, set the xv6_sigport_killed flag
    if (is_term) {
        THREAD_SET_KILLED(p);
        if (THREAD_STOPPED(p)) {
            // If the thread is stopped, we need to wake it up.
            scheduler_wakeup_stopped(p);
        }
    }

    // Check if signal is pending (unmasked) and notify if thread is sleeping
    sigset_t pending_unmasked =
        smp_load_acquire(&p->signal.sig_pending_mask) & ~sigmask;
    if (pending_unmasked != 0) {
        tcb_lock(p);
        xv6_sigport_signal_notify(p);
        tcb_unlock(p);
    }

    return 0; // Signal sent successfully
}

int xv6_sigport_signal_send(int pid, ksiginfo_t *info) {
    struct thread *p = NULL;
    if (pid < 0 || info == NULL || SIGBAD(info->signo)) {
        return -EINVAL; // Invalid PID or signal number
    }
    rcu_read_lock();
    p = get_pid_thread(pid);
    if (IS_ERR(p)) {
        rcu_read_unlock();
        return -ESRCH; // No thread found
    }
    if (p == NULL) {
        rcu_read_unlock();
        return -ESRCH; // No thread found
    }
    assert(p != NULL, "xv6_sigport_signal_send: thread is NULL");

    int ret;
    // If the target has a thread group and is the group leader (i.e., pid ==
    // tgid), deliver as a process-directed signal to the thread group's
    // shared_pending. This matches POSIX xv6_sigport_kill() semantics: xv6_sigport_kill(pid) sends to
    // the process.
    struct thread_group *tg = p->thread_group;
    if (tg != NULL && tg->is_kernel) {
        ret = xv6_sigport___signal_send(p, info);
    } else if (tg != NULL && tg->tgid == pid) {
        ret = tg_signal_send(tg, info);
    } else {
        // Thread-directed signal (pid is a TID, not a TGID)
        ret = xv6_sigport___signal_send(p, info);
    }
    rcu_read_unlock();
    return ret;
}

bool xv6_sigport_signal_pending(struct thread *p) {
    if (!p) {
        return false;
    }
    // Fast path: if the hint bit is clear, there is no pending signal work.
    if (!THREAD_SIGPENDING(p)) {
        return false;
    }

    // Slow-path validation for current only: the hint bit may be stale
    // because xv6_sigport_recalc_sigpending_tsk() only sets and does not clear.
    if (p != current || p->sigacts == NULL) {
        return true;
    }

    sigacts_t *sa = p->sigacts;
    xv6_sigport_sigacts_lock(sa);
    bool has_pending = xv6_sigport_recalc_sigpending_tsk(p);
    if (!has_pending) {
        THREAD_CLEAR_SIGPENDING(p);
    }
    xv6_sigport_sigacts_unlock(sa);
    return has_pending;
}

// Version that checks pending state while caller already holds xv6_sigport_sigacts_lock.
// This function does not acquire xv6_sigport_sigacts_lock.
bool xv6_sigport_signal_pending_locked(struct thread *p, sigacts_t *sa) {
    if (!p || !sa) {
        return false;
    }
    if (!THREAD_SIGPENDING(p)) {
        return false;
    }

    sigset_t pending = smp_load_acquire(&p->signal.sig_pending_mask);
    sigset_t blocked = p->signal.sig_mask;
    if (p->thread_group != NULL) {
        pending |=
            smp_load_acquire(&p->thread_group->shared_pending.sig_pending_mask);
    }
    return (pending & ~blocked) != 0;
}

// Notify parent that child has stopped:
// - always wake parent waiter
// - send SIGCHLD only if parent didn't set SA_NOCLDSTOP
static void __notify_parent_child_stopped(struct thread *p) {
    bool notify_sigchld = true;

    pid_rlock();
    struct thread *parent = p->parent;
    if (parent != NULL && parent->sigacts != NULL) {
        sigacts_t *psa = parent->sigacts;
        xv6_sigport_sigacts_lock(psa);
        if (psa->sa[SIGCHLD].sa_flags & SA_NOCLDSTOP) {
            notify_sigchld = false;
        }
        xv6_sigport_sigacts_unlock(psa);
    }
    pid_runlock();

    if (parent != NULL) {
        scheduler_wakeup_interruptible(parent);
        if (notify_sigchld) {
            xv6_sigport_kill_proc(parent, SIGCHLD);
        }
    }
}

int xv6_sigport_signal_notify(struct thread *p) {
    if (!p) {
        return -EINVAL;
    }
    proc_assert_holding(p);
    if (THREAD_AWOKEN(p)) {
        return 0;
    }
    if (!THREAD_SLEEPING(p)) {
        return -EAGAIN; // Process is not sleeping
    }
    if (__thread_state_get(p) == THREAD_INTERRUPTIBLE) {
        // Must follow wakeup locking protocol:
        // - Release tcb_lock (must NOT be held during wakeup)
        // - Call wakeup (no pi_lock needed - rq_lock serializes)
        // - Reacquire tcb_lock
        tcb_unlock(p);
        scheduler_wakeup_interruptible(p);
        tcb_lock(p);
        return 0; // Success
    }
    return -EAGAIN; // Thread not in interruptible state
}

bool xv6_sigport_signal_terminated(struct thread *p) {
    if (!p) {
        return 0;
    }
    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return false; // No sigacts means no termination signals
    }
    xv6_sigport_sigacts_lock(sa);
    sigset_t masked = p->signal.sig_pending_mask & ~p->signal.sig_mask;
    bool terminated = (masked & sa->sa_sigterm) != 0;
    xv6_sigport_sigacts_unlock(sa);
    return terminated;
}

bool xv6_sigport_signal_test_clear_stopped(struct thread *p) {
    if (!p) {
        return 0;
    }
    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return THREAD_STOPPED(
            p); // No sigacts, just return current stopped state
    }

    xv6_sigport_sigacts_lock(sa);
    sigset_t sigmask = p->signal.sig_mask;
    sigset_t sigstop_mask = sa->sa_sigstop;
    sigset_t sigcont_mask = sa->sa_sigcont;
    sigset_t masked = p->signal.sig_pending_mask & ~sigmask;
    sigset_t pending_stopped = masked & sigstop_mask;
    sigset_t pending_cont = masked & sigcont_mask;

    if (pending_cont) {
        // A continue-category signal is pending. Determine if any of them
        // have user handlers installed. We resume the thread in all cases.
        bool user_handler = false;
        for (int signo = 1; signo <= NSIG; signo++) {
            if (sigismember(&sigcont_mask, signo) > 0 &&
                sigismember(&pending_cont, signo) > 0) {
                sigaction_t *act = &sa->sa[signo];
                if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
                    user_handler = true;
                    break;
                }
            }
        }
        // Clear all pending stop signals (they are canceled by any continue)
        p->signal.sig_pending_mask &= ~sigstop_mask;
        if (!user_handler) {
            // Default action: consume the continue signals here so they are not
            // delivered.
            p->signal.sig_pending_mask &= ~pending_cont;
        }
        // Recalc after modifying pending mask
        xv6_sigport_recalc_sigpending_tsk(p);
        xv6_sigport_sigacts_unlock(sa);
        return 0; // Do not request stop.
    }

    if (pending_stopped) {
        // Consume all pending stop signals (they stop the thread) and request
        // STOPPED state.
        p->signal.sig_pending_mask &= ~pending_stopped;
        // Recalc after modifying pending mask
        xv6_sigport_recalc_sigpending_tsk(p);
        xv6_sigport_sigacts_unlock(sa);
        return 1; // Caller will transition to THREAD_STOPPED.
    }

    xv6_sigport_sigacts_unlock(sa);
    // No new stop/cont signals; indicate whether thread is already stopped.
    return THREAD_STOPPED(p);
}

int xv6_sigport_signal_restore(struct thread *p, ucontext_t *context) {
    if (p == NULL || context == NULL) {
        return -EINVAL; // Invalid thread or context
    }

    sigacts_t *sa = p->sigacts;
    if (!sa) {
        return -EINVAL;
    }
    xv6_sigport_sigacts_lock(sa);

    p->signal.sig_stack = context->uc_stack;
    p->signal.sig_ucontext = (uint64)context->uc_link;

    if (p->signal.sig_ucontext == 0) {
        p->signal.sig_mask = p->signal.sig_saved_mask; // Reset to original mask
    } else {
        p->signal.sig_mask = context->uc_sigmask;
        p->signal.sig_mask |=
            p->signal.sig_saved_mask; // Update the signal mask
    }

    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);
    sigdelset(&sa->sa_sigignore, SIGKILL);
    sigdelset(&sa->sa_sigignore, SIGSTOP);
    // Recalc xv6_sigport_sigpending after changing blocked mask
    xv6_sigport_recalc_sigpending_tsk(p);
    xv6_sigport_sigacts_unlock(sa);

    return 0; // Success
}

int xv6_sigport_sigaction(int signum, struct sigaction *act, struct sigaction *oldact) {
    if (signum < 1 || signum > NSIG) {
        return -EINVAL; // Invalid signal number
    }
    if (signum == SIGKILL || signum == SIGSTOP) {
        return -EINVAL; // SIGKILL and SIGSTOP cannot be caught or ignored
    }

    struct thread *p = current;
    assert(p != NULL, "sys_sigaction: current returned NULL");

    if (!THREAD_USER_SPACE(p)) {
        return -EPERM; // xv6_sigport_sigaction can only be called in user space
    }

    sigacts_t *sa = p->sigacts;

    // Use only xv6_sigport_sigacts_lock for signal state protection
    xv6_sigport_sigacts_lock(sa);

    if (oldact) {
        *oldact = sa->sa[signum];
    }

    if (act) {
        bool clear_pending = false;
        __sig_reset_act_mask(sa, signum);

        if (act->sa_handler == SIG_IGN) {
            sigaddset(&sa->sa_sigignore, signum);
            sa->sa[signum] = *act;
            clear_pending = true;
        } else if (act->sa_handler == SIG_DFL) {
            if (__sig_setdefault(sa, signum) != 0) {
                xv6_sigport_sigacts_unlock(sa);
                return -EINVAL; // Failed to set default action
            }

            // For default-ignored signals, pending instances are discarded.
            if (sigismember(&sa->sa_sigignore, signum) > 0) {
                clear_pending = true;
            }

            // After changing to SIG_DFL, check if any pending signals
            // are now termination signals and update THREAD_KILLED accordingly
            sigset_t pending_term = p->signal.sig_pending_mask &
                                    sa->sa_sigterm & ~p->signal.sig_mask;
            if (pending_term != 0) {
                THREAD_SET_KILLED(p);
            }
        } else {
            // User-installed handler: preserve user-supplied disposition data.
            sa->sa[signum] = *act;
            sigdelset(&sa->sa[signum].sa_mask, SIGKILL); // Cannot be blocked
            sigdelset(&sa->sa[signum].sa_mask, SIGSTOP); // Cannot be blocked
        }

        if (clear_pending) {
            // Only ignored dispositions consume pending signals.
            if (xv6_sigport_sigpending_empty(p, signum) != 0) {
                xv6_sigport_sigacts_unlock(sa);
                return -EINVAL; // Failed to clear pending signals
            }
            if (p->thread_group != NULL) {
                tg_sigpending_empty(p->thread_group, signum);
            }
        }
    }

    xv6_sigport_sigacts_unlock(sa);
    return 0; // Success
}

int xv6_sigport_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (set != NULL && how != SIG_BLOCK && how != SIG_UNBLOCK &&
        how != SIG_SETMASK) {
        return -EINVAL; // Invalid operation
    }
    struct thread *p = current;
    assert(p != NULL, "xv6_sigport_sigprocmask: current returned NULL");

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "xv6_sigport_sigprocmask: sigacts is NULL");

    // Use only xv6_sigport_sigacts_lock for signal state protection
    xv6_sigport_sigacts_lock(sa);
    if (oldset) {
        *oldset = p->signal.sig_mask; // Save the old mask
    }

    // POSIX: if set is NULL, do not change mask (how is ignored).
    if (set != NULL) {
        if (how == SIG_SETMASK) {
            p->signal.sig_saved_mask = *set; // Set the new mask
            p->signal.sig_mask = *set;       // Update the signal mask
        } else if (how == SIG_BLOCK) {
            p->signal.sig_saved_mask |= *set; // Block the signals in the set
            p->signal.sig_mask |= *set;       // Update the signal mask
        } else if (how == SIG_UNBLOCK) {
            p->signal.sig_saved_mask &= ~(*set); // Unblock the signals in set
            p->signal.sig_mask &= ~(*set);       // Update the signal mask
        }
    }

    // Mandatory signals cannot be blocked
    sigdelset(&p->signal.sig_saved_mask, SIGKILL);
    sigdelset(&p->signal.sig_saved_mask, SIGSTOP);
    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);

    // Recalc xv6_sigport_sigpending flag after changing blocked mask
    xv6_sigport_recalc_sigpending_tsk(p);

    // Check if newly unmasked signals are pending (per-thread + shared)
    sigset_t pending = p->signal.sig_pending_mask;
    if (p->thread_group != NULL) {
        pending |= p->thread_group->shared_pending.sig_pending_mask;
    }
    sigset_t pending_unmasked = pending & ~p->signal.sig_mask;

    // If newly unmasked termination signals are pending, set THREAD_KILLED
    sigset_t pending_term = pending_unmasked & sa->sa_sigterm;
    if (pending_term != 0) {
        THREAD_SET_KILLED(p);
    }
    xv6_sigport_sigacts_unlock(sa);

    // If newly unmasked signals are pending and thread is sleeping, wake it.
    // Need tcb_lock for xv6_sigport_signal_notify (which checks thread state)
    if (pending_unmasked != 0) {
        tcb_lock(p);
        (void)xv6_sigport_signal_notify(p);
        tcb_unlock(p);
    }
    return 0; // Success
}

int xv6_sigport_sigpending(struct thread *p, sigset_t *set) {
    if (!set) {
        return -EINVAL; // Invalid set pointer
    }
    assert(p != NULL, "xv6_sigport_sigpending: current returned NULL");

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "xv6_sigport_sigpending: sigacts is NULL");

    // Use only xv6_sigport_sigacts_lock - it protects both masks
    xv6_sigport_sigacts_lock(sa);
    sigset_t mask = p->signal.sig_mask;
    sigset_t pending = p->signal.sig_pending_mask;
    if (p->thread_group != NULL) {
        pending |= p->thread_group->shared_pending.sig_pending_mask;
    }
    *set = mask & pending; // Return blocked-and-pending signals
    xv6_sigport_sigacts_unlock(sa);

    return 0; // Success
}

int xv6_sigport_sigreturn(void) {
    struct thread *p = current;
    assert(p != NULL, "sys_sigreturn: current returned NULL");

    if (!THREAD_USER_SPACE(p)) {
        return -EPERM; // xv6_sigport_sigreturn can only be called in user space
    }

    sigacts_t *sa = p->sigacts;
    xv6_sigport_sigacts_lock(sa);
    if (p->signal.sig_ucontext == 0) {
        xv6_sigport_sigacts_unlock(sa);
        return -EINVAL; // No signal trap frame to restore
    }
    xv6_sigport_sigacts_unlock(sa);

    // Call restore_sigframe without holding xv6_sigport_sigacts_lock since it calls
    // vm_copyin which needs vm_rlock (sleep lock)
    ucontext_t uc = {0};
    if (restore_sigframe(p, &uc) != 0) {
        // @TODO:
        exit(-1); // Restore failed, exit the thread
    }

    // xv6_sigport_signal_restore now acquires xv6_sigport_sigacts_lock internally
    assert(xv6_sigport_signal_restore(p, &uc) == 0, "xv6_sigport_sigreturn: xv6_sigport_signal_restore failed");

    return 0; // Success
}

// Dequeue signal - caller provides the xv6_sigport_sigaction copy.
// Caller must hold sigacts->lock.
static ksiginfo_t *__dequeue_signal_update_pending_nolock(struct thread *p,
                                                          int signo,
                                                          sigaction_t *act) {
    if (p == NULL || act == NULL) {
        return ERR_PTR(-EINVAL);
    }
    sigacts_t *sa = p->sigacts;
    if (sa) {
        sigacts_assert_holding(sa);
    }

    if (SIGBAD(signo)) {
        return ERR_PTR(-EINVAL);
    }

    assert(act->sa_handler != SIG_IGN,
           "__dequeue_signal_update_pending_nolock: signal handler is SIG_IGN");
    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
    if ((act->sa_flags & SA_SIGINFO) == 0) {
        assert(LIST_IS_EMPTY(&sq->queue),
               "sig_pending is not empty for a non-SA_SIGINFO signal");
        sigdelset(&p->signal.sig_pending_mask, signo);
        // Caller should call xv6_sigport_recalc_sigpending while still holding lock
        return NULL; // No signal info to return
    }

    // Pop exactly one ksiginfo (FIFO order: head of list).
    if (LIST_IS_EMPTY(&sq->queue)) {
        // Queue empty but bit set implies inconsistency; clear defensively.
        sigdelset(&p->signal.sig_pending_mask, signo);
        return NULL;
    }
    ksiginfo_t *info = LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
    assert(info->signo == signo,
           "__dequeue_signal_update_pending_nolock: pos->signo != signo");
    list_entry_detach(&info->list_entry);
    // If queue now empty, clear pending bit; else leave it set for further
    // delivery.
    if (LIST_IS_EMPTY(&sq->queue)) {
        sigdelset(&p->signal.sig_pending_mask, signo);
    }
    return info;
}

static int __deliver_signal(struct thread *p, int signo, ksiginfo_t *info,
                            sigaction_t *sa, bool *repeat) {
    // NOTE: This function is called WITHOUT tcb_lock held to allow
    // push_sigframe to acquire vm_wlock (sleep lock). The caller must
    // ensure the signal state (sa, info) was captured while holding the lock.
    if (repeat) {
        *repeat = false; // Default to not repeat
    }

    if (p == NULL || sa == NULL) {
        return -1;
    }

    if (sa->sa_handler == SIG_IGN) {
        return 0; // Signal is ignored
    }

    if (sa->sa_flags & SA_SIGINFO) {
        assert(info != NULL, "__deliver_signal: SA_SIGINFO but info is NULL");
    }

    // Other than SIG_IGN and SIG_CONT, all signal handlers must be placed
    // beyond the first page of the address space.
    if ((uint64)sa->sa_handler < PAGE_SIZE) {
        printf("__deliver_signal: invalid signal handler address %p for signal "
               "%d\n",
               sa->sa_handler, signo);
        tcb_lock(p);
        THREAD_SET_KILLED(p);
        tcb_unlock(p);
        return 0;
    }

    int ret = 0;
    if (THREAD_USER_SPACE(current)) {
        // If the thread has user space, push the signal to its user stack
        // This may call vm_try_growstack which needs vm_wlock (sleep lock)
        ret = push_sigframe(p, signo, sa, info);
    }

    // Acquire xv6_sigport_sigacts_lock to update signal masks
    sigacts_t *sigacts = p->sigacts;
    xv6_sigport_sigacts_lock(sigacts);
    if ((sa->sa_flags & SA_NODEFER) == 0) {
        sigaddset(&p->signal.sig_mask, signo);
    }

    p->signal.sig_mask |= sa->sa_mask; // Block the signal during delivery
    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);

    // Recalc xv6_sigport_sigpending flag after blocking signals
    xv6_sigport_recalc_sigpending_tsk(p);

    if ((sa->sa_flags & SA_RESETHAND) != 0) {
        // Reset the signal handler to default action
        assert(__sig_setdefault(sigacts, signo) == 0,
               "__deliver_signal: __sig_setdefault failed");
    }
    xv6_sigport_sigacts_unlock(sigacts);

    return ret;
}

void xv6_sigport_handle_signal(void) {
    struct thread *p = current;
    assert(p != NULL, "xv6_sigport_handle_signal: current returned NULL");
    if (p->sigacts == NULL) {
        return; // No signal actions defined
    }
    sigacts_t *sa = p->sigacts;
    struct thread_group *tg = p->thread_group;

    for (;;) {
        // Gather all signal info with xv6_sigport_sigacts_lock - this protects all signal
        // state
        xv6_sigport_sigacts_lock(sa);
        sigset_t sigmask = p->signal.sig_mask;
        sigset_t sigterm = sa->sa_sigterm;
        sigset_t sigstop = sa->sa_sigstop;
        sigset_t sigcont = sa->sa_sigcont;
        sigset_t pending = p->signal.sig_pending_mask;

        // Merge in shared pending signals from thread group
        sigset_t shared_pending = 0;
        if (tg != NULL) {
            shared_pending =
                smp_load_acquire(&tg->shared_pending.sig_pending_mask);
            pending |= shared_pending;
        }

        sigset_t masked = pending & ~sigmask;

        // Check termination
        if ((masked & sigterm) || THREAD_KILLED(p)) {
            /* Protect init (PID 1): force-terminate signals that lack
             * a user handler are silently dropped, matching Linux
             * kernel_init_free_pages / do_signal behaviour.  Only
             * explicit SIGKILL from the kernel can xv6_sigport_kill init. */
            if (p == __proctab_get_initproc() && !THREAD_KILLED(p)) {
                /* Consume the termination signals so they don't
                 * re-trigger on the next return to userspace. */
                p->signal.sig_pending_mask &= ~sigterm;
                if (tg != NULL)
                    tg->shared_pending.sig_pending_mask &= ~sigterm;
                xv6_sigport_recalc_sigpending_tsk(p);
                xv6_sigport_sigacts_unlock(sa);
                continue; /* re-check for other deliverable signals */
            }
            THREAD_SET_KILLED(p);
            xv6_sigport_sigacts_unlock(sa);
            break;
        }

        // Check stop/continue
        sigset_t pending_cont = masked & sigcont;
        sigset_t pending_stop = masked & sigstop;

        if (pending_cont) {
            // Continue cancels stop - clear stop signals from both
            // per-thread and shared pending
            p->signal.sig_pending_mask &= ~sigstop;
            if (tg != NULL) {
                tg->shared_pending.sig_pending_mask &= ~sigstop;
            }

            // Check if any pending SIGCONT has a user handler
            bool user_handler = false;
            for (int signo = 1; signo <= NSIG; signo++) {
                if (sigismember(&sigcont, signo) > 0 &&
                    sigismember(&pending_cont, signo) > 0) {
                    sigaction_t *act = &sa->sa[signo];
                    if (act->sa_handler != SIG_DFL &&
                        act->sa_handler != SIG_IGN) {
                        user_handler = true;
                        break;
                    }
                }
            }

            if (!user_handler) {
                // Default action: consume the continue signals here
                // from both per-thread and shared pending
                p->signal.sig_pending_mask &= ~pending_cont;
                if (tg != NULL) {
                    tg->shared_pending.sig_pending_mask &= ~pending_cont;
                }
                // Recalc xv6_sigport_sigpending flag after modifying pending mask
                xv6_sigport_recalc_sigpending_tsk(p);
                xv6_sigport_sigacts_unlock(sa);
                continue; // No handler to call, loop back
            }
            // If user_handler is true, leave pending_cont bits set
            // and fall through to deliver the signal to the user handler
            // (don't call continue - let the delivery code below handle it)
        } else if (pending_stop) {
            // Clear stop signals from both per-thread and shared pending,
            // then enter stopped state
            p->signal.sig_pending_mask &= ~pending_stop;
            if (tg != NULL) {
                tg->shared_pending.sig_pending_mask &= ~pending_stop;
            }
            // Record which signal caused the stop
            p->signal.stop_signal = bits_ffsg(pending_stop);
            // Recalc xv6_sigport_sigpending flag after modifying pending mask
            xv6_sigport_recalc_sigpending_tsk(p);
            xv6_sigport_sigacts_unlock(sa);

            // Use tcb_lock for state transition
            tcb_lock(p);
            __thread_state_set(p, THREAD_STOPPED);
            tcb_unlock(p);

            // Notify parent that child has stopped.
            __notify_parent_child_stopped(p);

            scheduler_yield();
            continue; // Re-check after wakeup
        }

        // Find first deliverable signal
        int signo = (masked != 0) ? bits_ffsg(masked) : 0;
        if (signo == 0 || signo > NSIG) {
            xv6_sigport_sigacts_unlock(sa);
            break; // No pending signals
        }

        // Skip stop signals (they were handled above and consumed)
        // Note: SIGCONT with user handler was NOT consumed above, so don't skip
        // it
        if (sigismember(&sigstop, signo)) {
            xv6_sigport_sigacts_unlock(sa);
            continue;
        }

        // Copy xv6_sigport_sigaction and dequeue while holding xv6_sigport_sigacts_lock
        sigaction_t sa_copy = sa->sa[signo];

        // Determine if the signal is from per-thread pending or shared pending,
        // and dequeue from the appropriate queue.
        bool from_shared = false;
        if (sigismember(&p->signal.sig_pending_mask, signo)) {
            // Per-thread pending — dequeue from per-thread queue
            from_shared = false;
        } else if (tg != NULL &&
                   sigismember(&tg->shared_pending.sig_pending_mask, signo)) {
            // Shared pending from thread group
            from_shared = true;
        } else {
            xv6_sigport_sigacts_unlock(sa);
            continue; // Signal was consumed elsewhere, try again
        }

        bool repeat = false;
        ksiginfo_t *info = NULL;
        if (from_shared && tg != NULL) {
            // Dequeue from thread group's shared pending.
            // sigacts lock is already held (which serializes shared_pending
            // access since all group threads share the same sigacts via
            // CLONE_SIGHAND). pid_rlock is NOT needed here because the
            // shared_pending queues are protected by sigacts->lock.
            info = tg_dequeue_signal(tg, signo);
        } else {
            info = __dequeue_signal_update_pending_nolock(p, signo, &sa_copy);
            assert(!IS_ERR(info),
                   "xv6_sigport_handle_signal: __dequeue_signal_update_pending failed");
        }

        // Recalc xv6_sigport_sigpending after dequeue modified the pending mask
        xv6_sigport_recalc_sigpending_tsk(p);

        // Release xv6_sigport_sigacts_lock before calling __deliver_signal, which may need
        // to acquire vm_wlock (sleep lock) via push_sigframe/vm_try_growstack
        xv6_sigport_sigacts_unlock(sa);

        assert(__deliver_signal(p, signo, info, &sa_copy, &repeat) == 0,
               "xv6_sigport_handle_signal: __deliver_signal failed");

        // Check repeat condition with xv6_sigport_sigacts_lock only
        if (sa_copy.sa_flags & SA_SIGINFO) {
            xv6_sigport_sigacts_lock(sa);
            bool unmasked = sigismember(&p->signal.sig_mask, signo) == 0;
            bool still_pending =
                sigismember(&p->signal.sig_pending_mask, signo) > 0;
            // Also check thread group shared_pending for more queued entries
            if (!still_pending && tg != NULL) {
                still_pending =
                    sigismember(&tg->shared_pending.sig_pending_mask, signo) >
                    0;
            }
            xv6_sigport_sigacts_unlock(sa);

            if (unmasked && still_pending) {
                repeat = true;
            }
        }

        if (info) {
            xv6_sigport_ksiginfo_free(info);
        }

        if (!repeat) {
            break;
        }
    }

    // Recalculate SIGPENDING after delivering/consuming all signals.
    // xv6_sigport_recalc_sigpending_tsk (used inside the loop) can only SET the flag;
    // we need xv6_sigport_recalc_sigpending (which checks shared_pending too) to CLEAR
    // it when no unmasked signals remain.
    xv6_sigport_recalc_sigpending();

    if (THREAD_KILLED(p)) {
        exit(-1);
    }
}

// Kill the threads with the given pid (process-directed signal).
// When the target has a thread group, this sends to the group (POSIX xv6_sigport_kill()).
// When pid < 0, send signal to every process in process group -pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int xv6_sigport_kill(int pid, int signum) {
    /* POSIX: xv6_sigport_kill(-pgid, sig) sends to process group */
    if (pid < -1)
        return pgroup_kill(-pid, signum);
    if (pid == -1)
        return -EINVAL; /* xv6_sigport_kill(-1) (all processes) not supported */

    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);

    return xv6_sigport_signal_send(pid, &info);
}

// Kill the given thread directly (thread-directed signal).
// Instead of looking up by pid, directly send signal to the given thread.
int xv6_sigport_kill_thread(struct thread *p, int signum) {
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);

    rcu_read_lock();
    int ret = xv6_sigport___signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

// Send a signal to a specific thread within a specific thread group (xv6_sigport_tgkill).
// This is the POSIX xv6_sigport_tgkill(tgid, tid, sig) function.
// Returns 0 on success, negative errno on failure.
int xv6_sigport_tgkill(int tgid, int tid, int signum) {
    if (tgid < 0 || tid < 0 || SIGBAD(signum)) {
        return -EINVAL;
    }
    struct thread *p = NULL;
    rcu_read_lock();
    p = get_pid_thread(tid);
    if (IS_ERR(p)) {
        rcu_read_unlock();
        return -ESRCH;
    }
    // Verify the thread belongs to the specified thread group
    if (p->thread_group == NULL || p->thread_group->tgid != tgid) {
        rcu_read_unlock();
        return -ESRCH;
    }
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);
    int ret = xv6_sigport___signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

// Send a signal to a specific thread by TID (xv6_sigport_tkill).
// This is the POSIX xv6_sigport_tkill(tid, sig) function.
int xv6_sigport_tkill(int tid, int signum) {
    if (tid < 0 || SIGBAD(signum)) {
        return -EINVAL;
    }
    struct thread *p = NULL;
    rcu_read_lock();
    p = get_pid_thread(tid);
    if (IS_ERR(p)) {
        rcu_read_unlock();
        return -ESRCH;
    }
    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = thread_tgid(current);
    int ret = xv6_sigport___signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

// Check if thread should be terminated.
// This only checks the THREAD_KILLED flag which is set atomically by
// xv6_sigport___signal_send when a termination signal is delivered. No locks needed.
int xv6_sigport_killed(struct thread *p) {
    if (!p) {
        return 0;
    }
    return THREAD_KILLED(p);
}

/**
 * signal_send_to_tgroup - send a process-directed signal to a thread group
 * @tgid: thread group ID (process ID)
 * @info: signal information
 *
 * Delivers the signal to any suitable thread in the thread group.
 * Used by xv6_sigport_kill_from_kernel() when targeting a process ID.
 *
 * Returns 0 on success, or negative errno on failure.
 */
static int signal_send_to_tgroup(int tgid, ksiginfo_t *info) {
    struct thread *leader = NULL;

    rcu_read_lock();

    leader = get_pid_thread(tgid);
    if (IS_ERR(leader)) {
        rcu_read_unlock();
        return -ESRCH;
    }

    // If pid doesn't refer to a group leader, find the real one
    struct thread_group *tg = leader->thread_group;
    if (tg != NULL && tg->tgid == tgid) {
        // Good — use tg_signal_send which already handles shared_pending
        int ret = tg_signal_send(tg, info);
        rcu_read_unlock();
        return ret;
    }

    // Fallback: tgid matched a non-leader TID, or thread has no group.
    // Navigate to the real leader if possible.
    if (tg != NULL && tg->group_leader != NULL) {
        int ret = tg_signal_send(tg, info);
        rcu_read_unlock();
        return ret;
    }

    // No thread group — send directly to the thread
    int ret = xv6_sigport___signal_send(leader, info);
    rcu_read_unlock();
    return ret;
}

/**
 * xv6_sigport_kill_from_kernel - send a signal from kernel context (no current thread)
 * @pid: target process/thread group ID
 * @signum: signal number to send
 *
 * Used by interrupt handlers (e.g., console ^C) where there is no
 * user-space caller. Sets sender to NULL/pid 0.
 *
 * Returns 0 on success, negative errno on failure.
 */
int xv6_sigport_kill_from_kernel(int pid, int signum) {
    if (SIGBAD(signum) && signum != 0) {
        return -EINVAL;
    }

    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = NULL; // Sent from kernel
    info.info.si_pid = 0;

    // Signal 0 is used to check if the process exists
    if (signum == 0) {
        struct thread *p = NULL;
        rcu_read_lock();
        p = get_pid_thread(pid);
        if (IS_ERR(p)) {
            rcu_read_unlock();
            return -ESRCH;
        }
        rcu_read_unlock();
        return 0;
    }

    return signal_send_to_tgroup(pid, &info);
}

/**
 * xv6_sigport_kill_proc - send a signal directly to a thread/thread group
 * @p: target thread
 * @signum: signal number to send
 *
 * For thread groups, selects a suitable thread to receive the signal.
 * Used internally (e.g., exit.c sending SIGCHLD to parent).
 *
 * Returns 0 on success, negative errno on failure.
 */
int xv6_sigport_kill_proc(struct thread *p, int signum) {
    if (!p || SIGBAD(signum)) {
        return -EINVAL;
    }

    ksiginfo_t info = {0};
    info.signo = signum;
    info.sender = current;
    info.info.si_pid = current ? thread_tgid(current) : 0;

    rcu_read_lock();

    struct thread_group *tg = p->thread_group;
    if (tg != NULL) {
        // Use tg_signal_send for proper shared_pending handling
        int ret = tg_signal_send(tg, &info);
        rcu_read_unlock();
        return ret;
    }

    // No thread group — send directly
    int ret = xv6_sigport___signal_send(p, &info);
    rcu_read_unlock();
    return ret;
}

/**
 * xv6_sigport_sigsuspend - temporarily replace signal mask and wait for a signal
 * @mask: the temporary signal mask to use while waiting
 *
 * Atomically:
 * 1. Saves the current signal mask
 * 2. Sets the signal mask to @mask
 * 3. Suspends until a signal is caught
 * 4. Restores the original signal mask
 *
 * Returns -EINTR when a signal is caught (always returns error).
 * This is POSIX/pthread-compatible behavior.
 */
int xv6_sigport_sigsuspend(const sigset_t *mask) {
    struct thread *p = current;
    if (!p || !mask) {
        return -EINVAL;
    }

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "xv6_sigport_sigsuspend: sigacts is NULL");

    xv6_sigport_sigacts_lock(sa);

    // Save the current mask and set the temporary one
    sigset_t saved = p->signal.sig_mask;
    p->signal.sig_saved_mask = p->signal.sig_mask; // Also save original
    p->signal.sig_mask = *mask;
    // SIGKILL and SIGSTOP cannot be blocked
    sigdelset(&p->signal.sig_mask, SIGKILL);
    sigdelset(&p->signal.sig_mask, SIGSTOP);

    // Check if there are already pending signals now unblocked
    sigset_t pending_unmasked =
        p->signal.sig_pending_mask & ~p->signal.sig_mask;
    // Also check thread group shared pending
    struct thread_group *tg = p->thread_group;
    if (tg != NULL) {
        pending_unmasked |=
            tg->shared_pending.sig_pending_mask & ~p->signal.sig_mask;
    }
    if (pending_unmasked != 0) {
        // Signals already pending and unblocked — restore and return
        p->signal.sig_mask = saved;
        p->signal.sig_saved_mask = saved;
        xv6_sigport_recalc_sigpending_tsk(p);
        xv6_sigport_sigacts_unlock(sa);
        return -EINTR;
    }

    xv6_sigport_recalc_sigpending_tsk(p);
    xv6_sigport_sigacts_unlock(sa);

    // Sleep until a signal arrives
    __thread_state_set(p, THREAD_INTERRUPTIBLE);
    scheduler_yield();

    // Do NOT restore the mask here. The temporary mask stays active so
    // xv6_sigport_handle_signal() (called from usertrapret) can deliver the signal.
    // push_sigframe() saves the current (temporary) mask in uc_sigmask,
    // and xv6_sigport_signal_restore() (xv6_sigport_sigreturn) restores from sig_saved_mask
    // when the outermost frame is popped.

    return -EINTR; // xv6_sigport_sigsuspend always returns -EINTR
}

/**
 * xv6_sigport_sigwait - wait for a signal from a specified set
 * @set: the set of signals to wait for
 * @sig: pointer to store the signal number that was delivered
 *
 * Unlike xv6_sigport_sigsuspend, xv6_sigport_sigwait removes the signal from pending and returns
 * the signal number without invoking the signal handler.
 *
 * Returns 0 on success, or negative errno on failure.
 * This is POSIX/pthread-compatible behavior.
 */
int xv6_sigport_sigwait(const sigset_t *set, int *sig) {
    struct thread *p = current;
    if (!p || !set || !sig) {
        return -EINVAL;
    }

    sigacts_t *sa = p->sigacts;
    assert(sa != NULL, "xv6_sigport_sigwait: sigacts is NULL");

    while (1) {
        xv6_sigport_sigacts_lock(sa);

        // Check for pending signals in the wait set (per-thread)
        sigset_t pending_wanted = p->signal.sig_pending_mask & *set;
        // Also check thread group shared pending
        struct thread_group *tg = p->thread_group;
        if (tg != NULL) {
            pending_wanted |= tg->shared_pending.sig_pending_mask & *set;
        }

        if (pending_wanted != 0) {
            // Find the first pending signal in the set
            for (int signo = 1; signo <= NSIG; signo++) {
                if (!sigismember(&pending_wanted, signo)) {
                    continue;
                }

                // Try per-thread pending first
                if (sigismember(&p->signal.sig_pending_mask, signo)) {
                    sigpending_t *sq = &p->signal.sig_pending[signo - 1];
                    if (!LIST_IS_EMPTY(&sq->queue)) {
                        ksiginfo_t *ksi =
                            LIST_FIRST_NODE(&sq->queue, ksiginfo_t, list_entry);
                        if (ksi) {
                            list_entry_detach(&ksi->list_entry);
                            xv6_sigport_ksiginfo_free(ksi);
                        }
                    }
                    if (LIST_IS_EMPTY(&sq->queue)) {
                        sigdelset(&p->signal.sig_pending_mask, signo);
                    }
                } else if (tg != NULL &&
                           sigismember(&tg->shared_pending.sig_pending_mask,
                                       signo)) {
                    // Dequeue from shared pending
                    ksiginfo_t *ksi = tg_dequeue_signal(tg, signo);
                    if (ksi) {
                        xv6_sigport_ksiginfo_free(ksi);
                    }
                }

                *sig = signo;
                xv6_sigport_recalc_sigpending_tsk(p);
                xv6_sigport_sigacts_unlock(sa);
                return 0;
            }
        }

        // No signal yet — temporarily unblock the waited signals so that
        // xv6_sigport___signal_send can see them as unblocked and call xv6_sigport_signal_notify
        // to wake us. Without this, blocked signals would not trigger
        // a wakeup and we'd sleep forever.
        sigset_t saved_mask = p->signal.sig_mask;
        p->signal.sig_mask &= ~(*set); // Temporarily unblock waited signals
        sigdelset(&p->signal.sig_mask, SIGKILL);
        sigdelset(&p->signal.sig_mask, SIGSTOP);
        xv6_sigport_recalc_sigpending_tsk(p);
        xv6_sigport_sigacts_unlock(sa);

        // Sleep until one arrives
        __thread_state_set(p, THREAD_INTERRUPTIBLE);
        scheduler_yield();

        // Restore the original mask before re-checking
        xv6_sigport_sigacts_lock(sa);
        p->signal.sig_mask = saved_mask;
        xv6_sigport_recalc_sigpending_tsk(p);
        xv6_sigport_sigacts_unlock(sa);

        // Check if we were xv6_sigport_killed
        if (xv6_sigport_killed(p)) {
            return -EINTR;
        }
    }
}
