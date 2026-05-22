// Minimal C bridge for Rust-side `xv6_panic` and other tiny macro-wrapped
// kernel helpers that bindgen cannot expose. Kept small intentionally so the
// rest of `kernel/proc/` remains 100% Rust.

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/pgroup.h"
#include "tty/session.h"
#include "signal.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "vfs/fs.h"
#include "vfs/vfs_types.h"
#include "hlist.h"

extern hlist_t *xv6_proctab_hlist_ptr(void);

__attribute__((noreturn))
void xv6_panic(const char *msg) {
    panic("%s", msg);
    __builtin_unreachable();
}

void pgroup_mark_kernel(struct pgroup *pg) {
    if (pg) pg->is_kernel = 1;
}

void session_mark_kernel(struct session *s) {
    if (s) s->is_kernel = 1;
}

void sigstack_init_for_thread(struct thread *p) {
    if (p) sigstack_init(&p->signal.sig_stack);
}

void install_user_root_finish(struct thread *p, void *root_inode) {
    struct vfs_inode_ref cwd_ref;
    int ret = vfs_inode_get_ref((struct vfs_inode *)root_inode, &cwd_ref);
    if (ret < 0) {
        panic("install_user_root: failed to get ref to root inode");
    }
    vfs_struct_lock(p->fs);
    p->fs->cwd = cwd_ref;
    vfs_struct_unlock(p->fs);
    vfs_iput((struct vfs_inode *)root_inode);
}

void xv6_proctab_foreach_inner(void (*cb)(struct thread *, void *), void *arg) {
    hlist_t *h = xv6_proctab_hlist_ptr();
    struct thread *t;
    hlist_foreach_node_rcu(h, t, proctab_entry) {
        cb(t, arg);
    }
}

void xv6_proctab_foreach_rcu(void (*cb)(struct thread *, void *), void *arg) {
    rcu_read_lock();
    xv6_proctab_foreach_inner(cb, arg);
    rcu_read_unlock();
}
