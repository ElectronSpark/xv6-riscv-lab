//! Rust port of `kernel/proc/clone.c`.
//!
//! Implements `thread_clone` and the post-context-switch `forkret_entry`.
//! All struct field access goes through C shims in `proc_rust_shims.c`.
//!
//! ## Centralized-unsafe convention (matches `pgroup.rs` / `pid.rs` / `exit.rs`)
//!
//! All FFI declarations live in the single `unsafe extern "C" { ... }`
//! block below and are marked `pub safe fn`. That promise extends the
//! safety contract for the entire module: every C entry point listed
//! is callable from safe Rust *given* the precondition that any raw
//! pointers passed in are live kernel objects whose lifetime is
//! covered by an appropriate higher-level lock (`pid_lock`, `tcb_lock`,
//! "I own this thread"). The same preconditions held in the original
//! C and are not weakened here.
//!
//! Function bodies keep raw-pointer conversion at the ABI edge, then use
//! the safe wrapper surface below.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_int, c_void};
use core::ptr;

// ---------------------------------------------------------------------------
// Pointer type aliases.
// P3-D1a: formerly opaque `[u8; 0]` markers; now aliases of the real types
// used by the `proc_shims` accessor signatures (`crate::bindings` structs
// where the shim is typed, `c_void` where the shim traffics in `*mut
// c_void`), so the direct Rust calls type-check with call sites unchanged.
// ---------------------------------------------------------------------------
type Thread       = crate::bindings::thread;
type Session      = crate::bindings::session;
type SchedEntity  = c_void;
type Context      = c_void;
type VfsFdtable   = c_void;
type FsStruct     = c_void;
type Vm           = c_void;
// P3-D2b: `ThreadGroup`/`Pgroup`/`Sigacts`/`ThreadSignal` aliases deleted --
// their only users were the `extern "C"` redeclarations of proc-object
// entry points, now plain crate-path imports typed with the real
// `crate::bindings` structs.

// ---------------------------------------------------------------------------
// Errno + constants (mirrors of C headers)
// ---------------------------------------------------------------------------
const EINVAL: c_int = 22;
const EAGAIN: c_int = 11;
const ENOMEM: c_int = 12;

// Clone flags (must match uabi/clone_flags.h)
const CLONE_FILES:  u64 = 0x00100000;
const CLONE_FS:     u64 = 0x00200000;
const CLONE_PARENT: u64 = 0x80000000;
const CLONE_SIGHAND:u64 = 0x0200000000;
const CLONE_THREAD: u64 = 0x1000000000;
const CLONE_VFORK:  u64 = 0x4000000000;
const CLONE_VM:     u64 = 0x8000000000;

// Thread state (matches enum thread_state)
const THREAD_UNINTERRUPTIBLE: c_int = 6;

const USERSTACK_MINSZ: u64 = 4096 * 4; // PAGE_SIZE << 2
const PAGE_SIZE:       u64 = 4096;

// clone_args layout (matches uabi/clone_flags.h)
#[repr(C)]
pub struct CloneArgs {
    pub flags:      u64,
    pub stack:      u64,
    pub stack_size: u64,
    pub entry:      u64,
    pub esignal:    u64,
    pub tls:        u64,
    pub ctid:       u64,
    pub ptid:       u64,
}

// ---------------------------------------------------------------------------
// External C symbols — centralized unsafe boundary.
// See module-level safety contract above.
// ---------------------------------------------------------------------------
// P3-D1a: the `xv6_*` accessor shims are ordinary Rust fns in
// `proc_shims.rs`; call them via crate paths instead of `extern "C"`
// redeclarations (`t_*` accessors imported under their old local alias
// names). Call sites keep the same bare names.
use crate::proc::proc_shims::{
    t_parent as xv6_t_parent, t_pgroup as xv6_t_pgroup, t_pid as xv6_t_pid,
    t_session as xv6_t_session, t_tgid as xv6_t_tgid, t_thread_group as xv6_t_thread_group,
    t_user_space as xv6_t_user_space, xv6_current_thread, xv6_err_ptr, xv6_forkret_assert_user,
    xv6_intr_on, xv6_is_err, xv6_mycpu_clear_noff, xv6_panic, xv6_pid_wlock, xv6_pid_wunlock,
    xv6_ptr_err, xv6_smp_mb, xv6_t_copy_name, xv6_t_copy_trapframe, xv6_t_fdtable, xv6_t_fs,
    xv6_t_kstack_order, xv6_t_sched_entity, xv6_t_set_clone_flags, xv6_t_set_fdtable,
    xv6_t_set_fs, xv6_t_set_parent, xv6_t_set_sigacts, xv6_t_set_tgid, xv6_t_set_user_space,
    xv6_t_set_vfork_parent, xv6_t_set_vm, xv6_t_sigacts, xv6_t_signal_ptr,
    xv6_t_trapframe_set_a0, xv6_t_trapframe_set_sepc, xv6_t_trapframe_set_sp, xv6_t_vm,
    xv6_tcb_lock, xv6_tcb_unlock, xv6_thread_from_context, xv6_thread_state_set,
};

// P3-D3b: `thread_create`/`thread_destroy`/`attach_child`
// (kernel/proc/thread.rs) are plain safe Rust fns now that their
// `#[no_mangle]` exports are gone; the old file-private `extern "C"`
// redeclarations (kept non-`pub` to dodge E0659 with the glob reexport
// at `crate::proc`) are replaced by direct crate-path imports of the
// real definitions. This file's `Thread` alias *is*
// `crate::bindings::thread`, so the signatures are identical.
use crate::proc::thread::{attach_child, thread_create, thread_destroy};

// P3-D3b: lock/rcu.rs's `rcu_check_callbacks` is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone; reached by crate path.
use crate::lock::rcu::rcu_check_callbacks;

// P3-D3a: `vm_dup`/`vm_copy` (mm/vm.rs) are ordinary (safe) Rust fns now
// that their `#[no_mangle]` exports are gone. This file's `Vm` is an
// opaque `c_void` stand-in for the real `crate::bindings::vm`
// (layout-identical pointer, same cast-adapter precedent as exit.rs's
// `sigacts_put`). Divergence the old redeclaration papered over: it
// claimed `vm_dup` returns `*mut Vm`, but the real fn returns `()` (the
// C ABI let the caller read a stale return register) — the only call
// site discards the "return value", so the adapter returns `()`.
#[inline]
fn vm_dup(vm: *mut Vm) {
    crate::mm::vm_dup(vm as *mut crate::bindings::vm)
}
#[inline]
fn vm_copy(vm: *mut Vm) -> *mut Vm {
    crate::mm::vm_copy(vm as *mut crate::bindings::vm) as *mut Vm
}

// P3-D2b: the signal/thread-group/pgroup entry points (proc/{signal,
// thread_group,pgroup}.rs) are ordinary Rust fns now that their
// `#[no_mangle]` exports are gone; reached as plain (file-private, so
// no E0659 glob-reexport ambiguity) crate-path items instead of the
// `extern "C"` redeclarations that used to sit in the block above.
// Divergences the old redeclarations papered over, now handled at the
// call sites instead:
//  - `sigacts_dup`/`sigpending_clone` were declared with opaque
//    `c_void` handles; the real fns take `*mut bindings::sigacts` /
//    `*mut bindings::thread_signal_t` (layout-identical pointer casts).
//  - `sigpending_clone`'s `esignal` was declared `u64`; the real param
//    is `c_int` (the C ABI already only ever read the low 32 bits).
//  - `pgroup_add_tg`/`pgroup_add_thread` were declared `-> ()`; the
//    real fns return `c_int`, which the call sites discard exactly as
//    the old ABI did.
use crate::proc::{
    pgroup_add_thread, pgroup_add_tg, sigacts_dup, sigpending_clone,
    thread_group_add, thread_group_alloc,
};

// P3-D2a: `rq_task_fork` (kernel/proc/rq.rs) and `scheduler_wakeup`/
// `scheduler_yield`/`context_switch_finish` (kernel/proc/sched.rs) are
// ordinary Rust fns, reached as plain crate-path items instead of the
// `extern "C"` redeclarations that used to sit in the block above
// (`Thread` is a plain alias of `crate::bindings::thread`, so the
// signatures are identical; `rq_task_fork`'s one call site casts its
// `*mut c_void` sched-entity handle to the real pointee type).
use crate::proc::{context_switch_finish, rq_task_fork, scheduler_wakeup, scheduler_yield};

// P3-1B mesh sweep: same-crate `pub(crate)` items as of this wave,
// referenced via a crate path instead of `extern "C"` redeclarations.
// `usertrap`/`__alloc_pid`/`__free_pid` are the exact same underlying
// type on both sides (zero-arg / `c_int`), no cast needed.
// `proctab_proc_add` takes `pid::Thread`, a distinct (but layout-
// identical) opaque marker from this file's own `Thread` -- reinterpret
// via a thin wrapper, same precedent as `sysproc.rs`'s `thread_clone`.
use crate::irq::trap::usertrapret;
use crate::proc::pid::{__alloc_pid, __free_pid};
#[inline(always)]
fn proctab_proc_add(t: *mut Thread) {
    crate::proc::pid::proctab_proc_add(t as *mut c_void as *mut crate::proc::pid::Thread);
}

// P3-1C mesh sweep: vfs/{fs,fdtable}.rs and tty/session.rs are in scope
// for this wave, same reinterpret precedent as `proctab_proc_add` above
// (this file's opaque markers are distinct-but-layout-identical stand-ins
// for the real `crate::bindings` structs).
#[inline(always)]
fn vfs_struct_clone(old: *mut FsStruct, flags: u64) -> *mut FsStruct {
    crate::vfs::fs::vfs_struct_clone(old as *mut c_void as *mut crate::bindings::fs_struct, flags)
        as *mut c_void as *mut FsStruct
}
#[inline(always)]
fn vfs_fdtable_clone(src: *mut VfsFdtable, flags: c_int) -> *mut VfsFdtable {
    crate::vfs::fdtable::vfs_fdtable_clone(
        src as *mut c_void as *mut crate::bindings::vfs_fdtable,
        flags,
    ) as *mut c_void as *mut VfsFdtable
}
/// SAFETY: only called from `thread_clone` with a live parent session and
/// the just-created child thread (both non-null, verified by the
/// caller) -- matches the real fn's contract. Its `c_int` return is
/// discarded here exactly as this file's only call site always did.
#[inline(always)]
fn session_add_thread(s: *mut Session, t: *mut Thread) {
    unsafe {
        crate::tty::session::session_add_thread(
            s as *mut c_void as *mut crate::bindings::session,
            t as *mut c_void as *mut crate::bindings::thread,
        );
    }
}

#[cold]
fn panic_clone(msg: &str) -> ! {
    let mut buf = [0u8; 96];
    let n = msg.as_bytes().len().min(buf.len() - 1);
    buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
    // xv6_panic only reads the buffer up to the first NUL; we
    // zero-initialised `buf` and bounded the copy by `buf.len() - 1`,
    // so the final byte is always 0.
    xv6_panic(buf.as_ptr())
}

#[inline]
fn clone_args_mut<'a>(args: *mut CloneArgs) -> Option<&'a mut CloneArgs> {
    if args.is_null() { return None; }
    unsafe { Some(&mut *args) }
}

// ---------------------------------------------------------------------------
// forkret_entry — called from context switch via thread_create entry pointer.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn forkret_entry(prev: *mut Context) {
    let cur = xv6_current_thread();
    xv6_forkret_assert_user(cur);
    if prev.is_null() {
        panic_clone("forkret_entry: prev context is NULL");
    }

    context_switch_finish(xv6_thread_from_context(prev), cur, 0);
    xv6_mycpu_clear_noff();
    xv6_intr_on();
    rcu_check_callbacks();
    xv6_smp_mb();
    usertrapret();
}

#[inline]
fn check_ptr<T>(ptr: *mut T) -> Result<*mut T, c_int> {
    if ptr.is_null() {
        Err(-ENOMEM)
    } else if xv6_is_err(ptr as *const c_void) != 0 {
        Err(xv6_ptr_err(ptr as *const c_void) as c_int)
    } else {
        Ok(ptr)
    }
}

// ---------------------------------------------------------------------------
// thread_clone — full port of C `thread_clone`.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn thread_clone(args: *mut CloneArgs) -> c_int {
    let Some(args) = clone_args_mut(args) else { return -EINVAL };
    let p = xv6_current_thread();

    if xv6_t_user_space(p) == 0 {
        return -EINVAL;
    }

    // CLONE_THREAD implies CLONE_PARENT + requires CLONE_VM | CLONE_SIGHAND
    if args.flags & CLONE_THREAD != 0 {
        if args.flags & (CLONE_VM | CLONE_SIGHAND) != (CLONE_VM | CLONE_SIGHAND) {
            return -EINVAL;
        }
        args.flags |= CLONE_PARENT;
    }

    // CLONE_VM without CLONE_VFORK requires stack and entry
    if args.flags & CLONE_VM != 0 && args.flags & CLONE_VFORK == 0
        && (args.stack == 0 || args.entry == 0)
    {
        return -EINVAL;
    }

    // Stack alignment check
    if args.stack != 0
        && (args.stack_size < USERSTACK_MINSZ || (args.stack_size & (PAGE_SIZE - 1)) != 0)
    {
        return -EINVAL;
    }

    // Reserve a PID slot
    if __alloc_pid() < 0 {
        return -EAGAIN;
    }

    // Allocate child thread
    let kstack_order = xv6_t_kstack_order(p);
    let child = match check_ptr(thread_create(forkret_entry as *mut c_void, 0, 0, kstack_order)) {
        Ok(c) => c,
        Err(e) => {
            __free_pid();
            return e;
        }
    };

    // VM: share or copy
    let new_vm = if args.flags & CLONE_VM != 0 {
        let v = xv6_t_vm(p);
        vm_dup(v);
        v
    } else {
        match check_ptr(vm_copy(xv6_t_vm(p))) {
            Ok(v) => v,
            Err(e) => {
                thread_destroy(child);
                __free_pid();
                return e;
            }
        }
    };
    xv6_t_set_vm(child, new_vm);

    // FS
    let fs_clone = match check_ptr(vfs_struct_clone(xv6_t_fs(p), args.flags)) {
        Ok(f) => f,
        Err(e) => {
            thread_destroy(child);
            __free_pid();
            return e;
        }
    };
    xv6_t_set_fs(child, fs_clone);

    // fdtable
    let new_fdt = match check_ptr(vfs_fdtable_clone(xv6_t_fdtable(p), args.flags as c_int)) {
        Ok(f) => f,
        Err(e) => {
            thread_destroy(child);
            __free_pid();
            return e;
        }
    };
    xv6_t_set_fdtable(child, new_fdt);

    // sigacts
    let p_sigacts = xv6_t_sigacts(p);
    if !p_sigacts.is_null() {
        let dup = sigacts_dup(p_sigacts as *mut crate::bindings::sigacts, args.flags);
        if dup.is_null() {
            thread_destroy(child);
            __free_pid();
            return -ENOMEM;
        }
        xv6_t_set_sigacts(child, dup as *mut c_void);
    }

    // Per-thread signal mask
    sigpending_clone(
        xv6_t_signal_ptr(child) as *mut crate::bindings::thread_signal_t,
        xv6_t_signal_ptr(p) as *mut crate::bindings::thread_signal_t,
        args.flags,
        args.esignal as c_int,
    );
    xv6_t_set_clone_flags(child, args.flags);

    // Copy + adjust trapframe
    xv6_t_copy_trapframe(child, p);
    if args.entry != 0 {
        xv6_t_trapframe_set_sepc(child, args.entry);
    }
    if args.stack != 0 {
        let stack_top = (args.stack + args.stack_size) & !0xFu64;
        xv6_t_trapframe_set_sp(child, stack_top);
    }
    xv6_t_trapframe_set_a0(child, 0);
    xv6_t_copy_name(child, p);

    xv6_tcb_lock(child);
    xv6_t_set_user_space(child);
    xv6_thread_state_set(child, THREAD_UNINTERRUPTIBLE);
    rq_task_fork(xv6_t_sched_entity(child) as *mut crate::bindings::sched_entity);
    if args.flags & CLONE_VFORK != 0 {
        xv6_t_set_vfork_parent(child, p);
        xv6_thread_state_set(p, THREAD_UNINTERRUPTIBLE);
    } else {
        xv6_t_set_vfork_parent(child, ptr::null_mut());
    }
    xv6_tcb_unlock(child);

    // Attach to parent + add to pid table
    xv6_pid_wlock();
    let real_parent = if args.flags & CLONE_PARENT != 0 {
        xv6_t_parent(p)
    } else {
        p
    };

    if args.flags & CLONE_THREAD != 0 {
        xv6_t_set_parent(child, real_parent);
    } else {
        attach_child(real_parent, child);
    }
    proctab_proc_add(child);

    if args.flags & CLONE_THREAD != 0 {
        let parent_tg = xv6_t_thread_group(p);
        if parent_tg.is_null() {
            panic_clone("clone: parent has no thread_group for CLONE_THREAD");
        }
        thread_group_add(parent_tg, child);
        xv6_t_set_tgid(child, xv6_t_tgid(p));
    } else {
        let rc = thread_group_alloc(child);
        if rc != 0 {
            panic_clone("clone: thread_group_alloc failed");
        }
    }

    let parent_pg = xv6_t_pgroup(p);
    let parent_sess = xv6_t_session(p);
    if !parent_pg.is_null() {
        if args.flags & CLONE_THREAD == 0 {
            pgroup_add_tg(parent_pg, xv6_t_thread_group(child));
        }
        pgroup_add_thread(parent_pg, child);
    }
    if !parent_sess.is_null() {
        session_add_thread(parent_sess, child);
    }
    xv6_pid_wunlock();

    scheduler_wakeup(child);

    if args.flags & CLONE_VFORK != 0 {
        scheduler_yield();
    }

    xv6_t_pid(child)
}
