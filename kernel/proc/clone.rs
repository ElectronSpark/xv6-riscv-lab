//! Fork/clone construction and the child's first context-switch return.
//!
//! `PendingChild` owns the reserved PID capacity and unpublished thread while
//! VM, filesystem, descriptor, signal and thread-group allocations can fail.
//! Each successful acquisition is installed on the child immediately; an
//! error drops the guard and releases those references through the existing
//! thread destructor, with no process-table or child control-block lock held.
//!
//! After construction, publication is allocation-free. Numeric PID assignment,
//! group-ID finalization, hierarchy attachment and registry publication share
//! one uninterrupted PID write lock. A vfork parent is parked only after all
//! fallible work succeeds. Native construction returns `KResult`; the syscall
//! adapter preserves the existing PID-or-negative-errno ABI.
//!
//! Raw object pointers cross existing proc/VM/VFS accessors with their kernel
//! lifetime contracts. No mutable reference to a shared thread is constructed.
//! `forkret_entry` retains its C ABI because assembly restores its address.

#![allow(non_camel_case_types, non_snake_case)]

use crate::kstd::{Errno, KResult};
use core::ffi::{c_int, c_void};
use core::ptr::{self, NonNull};

// ---------------------------------------------------------------------------
// Pointer type aliases.
// P3-D1a: formerly opaque `[u8; 0]` markers; now aliases of the real types
// used by the `proc_shims` accessor signatures (`crate::bindings` structs
// where the shim is typed, `c_void` where the shim traffics in `*mut
// c_void`), so the direct Rust calls type-check with call sites unchanged.
// ---------------------------------------------------------------------------
type Thread = crate::bindings::thread;
type Session = crate::bindings::session;
type SchedEntity = c_void;
type Context = c_void;
type VfsFdtable = c_void;
type FsStruct = c_void;
type Vm = c_void;
// P3-D2b: `ThreadGroup`/`Pgroup`/`Sigacts`/`ThreadSignal` aliases deleted --
// their only users were the `extern "C"` redeclarations of proc-object
// entry points, now plain crate-path imports typed with the real
// `crate::bindings` structs.

// ---------------------------------------------------------------------------
// ABI constants
// ---------------------------------------------------------------------------
// Clone flags (must match uabi/clone_flags.h)
const CLONE_FILES: u64 = 0x00100000;
const CLONE_FS: u64 = 0x00200000;
const CLONE_PARENT: u64 = 0x80000000;
const CLONE_SIGHAND: u64 = 0x0200000000;
const CLONE_THREAD: u64 = 0x1000000000;
const CLONE_VFORK: u64 = 0x4000000000;
const CLONE_VM: u64 = 0x8000000000;

// Thread state (matches enum thread_state)
const THREAD_UNINTERRUPTIBLE: c_int = 6;

const USERSTACK_MINSZ: u64 = 4096 * 4; // PAGE_SIZE << 2
const PAGE_SIZE: u64 = 4096;

// clone_args layout (matches uabi/clone_flags.h)
// RUSTIFY-PROC: the only out-of-file namer is `proc/sysproc.rs` (in
// `crate::proc`, via the `pub use clone::*` glob) -> pub(super).
#[repr(C)]
pub(super) struct CloneArgs {
    pub flags: u64,
    pub stack: u64,
    pub stack_size: u64,
    pub entry: u64,
    pub esignal: u64,
    pub tls: u64,
    pub ctid: u64,
    pub ptid: u64,
}

// ---------------------------------------------------------------------------
// Existing native proc accessors; their raw pointers follow the object
// lifetime and synchronization contracts of the defining modules.
// ---------------------------------------------------------------------------
// P3-D1a: the `xv6_*` accessor shims are ordinary Rust fns in
// `proc_shims.rs`; call them via crate paths instead of `extern "C"`
// redeclarations (`t_*` accessors imported under their old local alias
// names). Call sites keep the same bare names.
use crate::proc::proc_shims::{
    t_parent as xv6_t_parent, t_pgroup as xv6_t_pgroup, t_session as xv6_t_session,
    t_tgid as xv6_t_tgid, t_thread_group as xv6_t_thread_group, t_user_space as xv6_t_user_space,
    xv6_current_thread, xv6_forkret_assert_user, xv6_is_err, xv6_mycpu_clear_noff, xv6_panic,
    xv6_pid_wlock, xv6_pid_wunlock, xv6_ptr_err, xv6_t_copy_name, xv6_t_copy_trapframe,
    xv6_t_fdtable, xv6_t_fs, xv6_t_kstack_order, xv6_t_sched_entity, xv6_t_set_clone_flags,
    xv6_t_set_fdtable, xv6_t_set_fs, xv6_t_set_parent, xv6_t_set_sigacts, xv6_t_set_tgid,
    xv6_t_set_user_space, xv6_t_set_vfork_parent, xv6_t_set_vm, xv6_t_sigacts, xv6_t_signal_ptr,
    xv6_t_trapframe_set_a0, xv6_t_trapframe_set_sepc, xv6_t_trapframe_set_sp, xv6_t_vm,
    xv6_thread_from_context, xv6_thread_state_set,
};

// P3-D3b: `thread_create`/`thread_destroy`/`attach_child`
// (kernel/proc/thread.rs) are plain safe Rust fns now that their
// `#[no_mangle]` exports are gone; the old file-private `extern "C"`
// redeclarations (kept non-`pub` to dodge E0659 with the glob reexport
// at `crate::proc`) are replaced by direct crate-path imports of the
// real definitions. This file's `Thread` alias *is*
// `crate::bindings::thread`, so the signatures are identical.
// NO-STANDALONE-FN: `thread_create` is now the associated fn `Thread::create`
// (reached through this file's `type Thread = crate::bindings::thread` alias,
// which *is* `crate::proc::thread::Thread`); `thread_destroy`/`attach_child`
// are handle methods on `ThreadAccess` (the thread being destroyed / the
// parent). Call sites construct the namespaced path / the handle.
use crate::proc::access::ThreadAccess;

// P3-D3b: lock/rcu.rs's `rcu_check_callbacks` is a plain safe associated
// fn (`Rcu::check_callbacks`) now that its `#[no_mangle]` export is gone;
// reached by crate path.
use crate::lock::rcu::Rcu;

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
    crate::mm::vm::Vm::vm_dup(vm as *mut crate::bindings::vm)
}
#[inline]
fn vm_copy(vm: *mut Vm) -> *mut Vm {
    crate::mm::vm::Vm::vm_copy(vm as *mut crate::bindings::vm) as *mut Vm
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
use crate::proc::access::ThreadGroupAccess;
use crate::proc::thread_group::ThreadGroup;
use crate::proc::Pgroup;

// P3-D2a: `rq_task_fork` (kernel/proc/rq.rs) and `scheduler_wakeup`/
// `scheduler_yield`/`context_switch_finish` (kernel/proc/sched.rs) are
// ordinary Rust fns, reached as plain crate-path items instead of the
// `extern "C"` redeclarations that used to sit in the block above
// (`Thread` is a plain alias of `crate::bindings::thread`, so the
// signatures are identical; `rq_task_fork`'s one call site casts its
// `*mut c_void` sched-entity handle to the real pointee type).
use crate::proc::{Rq, Scheduler};

// P3-1B mesh sweep: same-crate `pub(crate)` items as of this wave,
// referenced via a crate path instead of `extern "C"` redeclarations.
// `usertrap`/`__alloc_pid`/`__free_pid` are the exact same underlying
// type on both sides (zero-arg / `c_int`), no cast needed.
// `proctab_proc_add` takes `pid::Thread`, a distinct (but layout-
// identical) opaque marker from this file's own `Thread` -- reinterpret
// via a thin wrapper, same precedent as `sysproc.rs`'s `thread_clone`.
use crate::irq::trap::Trap;
use crate::proc::pid::{Pid, ProcTable};

// P3-1C mesh sweep: vfs/{fs,fdtable}.rs and tty/session.rs are in scope
// for this wave, same reinterpret precedent as `proctab_proc_add` above
// (this file's opaque markers are distinct-but-layout-identical stand-ins
// for the real `crate::bindings` structs).
#[inline(always)]
fn vfs_struct_clone(old: *mut FsStruct, flags: u64) -> *mut FsStruct {
    crate::vfs::fs::FsStruct::vfs_struct_clone(
        old as *mut c_void as *mut crate::bindings::fs_struct,
        flags,
    ) as *mut c_void as *mut FsStruct
}
#[inline(always)]
fn vfs_fdtable_clone(src: *mut VfsFdtable, flags: c_int) -> *mut VfsFdtable {
    crate::vfs::fdtable::VfsFdtable::vfs_fdtable_clone(
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
        crate::tty::session::Session::add_thread(
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
    if args.is_null() {
        return None;
    }
    unsafe { Some(&mut *args) }
}

// ---------------------------------------------------------------------------
// forkret_entry — called from context switch via thread_create entry pointer.
// P3-D3c: `#[no_mangle]` dropped (no caller anywhere resolves it by symbol
// name; `thread_clone` below takes its address from Rust), but it KEEPS
// `extern "C"`: the address is stored as a raw entry pointer and invoked
// through the context-switch return path (`proc/swtch.S`-restored `ra`),
// so the C ABI is load-bearing.
// RUSTIFY-PROC: zero namers anywhere outside this file (its address is taken
// only by `thread_clone` below) -> private; `extern "C"` stays (the stored
// entry pointer is invoked through the context-switch return path).
// ---------------------------------------------------------------------------
extern "C" fn forkret_entry(prev: *mut Context) {
    let cur = xv6_current_thread();
    xv6_forkret_assert_user(cur);
    if prev.is_null() {
        panic_clone("forkret_entry: prev context is NULL");
    }

    Scheduler::context_switch_finish(xv6_thread_from_context(prev), cur, 0);
    xv6_mycpu_clear_noff();
    crate::machine::Riscv::intr_on();
    Rcu::check_callbacks();
    crate::machine::Riscv::smp_mb();
    Trap::usertrapret();
}

#[inline]
fn check_ptr<T>(ptr: *mut T) -> KResult<*mut T> {
    if ptr.is_null() {
        Err(Errno::NoMem)
    } else if xv6_is_err(ptr as *const c_void) != 0 {
        Err(Errno::Raw(xv6_ptr_err(ptr as *const c_void) as c_int))
    } else {
        Ok(ptr)
    }
}

/// Own an unfinished child and its reserved PID capacity. Each acquired
/// resource is installed on the child before the next fallible operation, so
/// its existing destructor can release VM, files, fs and signal references.
/// This guard exists only before any scheduler or process-table publication.
struct PendingChild {
    child: Option<NonNull<Thread>>,
}

impl PendingChild {
    fn new(kstack_order: c_int) -> KResult<Self> {
        if Pid::alloc() < 0 {
            return Err(Errno::Again);
        }
        let mut pending = Self { child: None };
        let child = check_ptr(Thread::create(
            forkret_entry as *mut c_void,
            0,
            0,
            kstack_order,
        ))?;
        pending.child = NonNull::new(child);
        Ok(pending)
    }

    fn as_ptr(&self) -> *mut Thread {
        self.child
            .expect("a constructed PendingChild owns a thread")
            .as_ptr()
    }

    /// Begin the infallible publication phase. No resource allocation or
    /// error return is permitted after this point; normal exit/reap takes
    /// responsibility for the child's resources and PID reservation.
    fn commit(self) {
        core::mem::forget(self);
    }
}

impl Drop for PendingChild {
    fn drop(&mut self) {
        if let Some(child) = self.child {
            // SAFETY: the child is exclusively owned and still THREAD_UNUSED;
            // it has never joined a run queue, family list or process table.
            unsafe { ThreadAccess::assume(child.as_ptr()) }.destroy();
        }
        Pid::free();
    }
}

// ---------------------------------------------------------------------------
// thread_clone — full port of C `thread_clone`.
// ---------------------------------------------------------------------------
// P3-D3c: `#[no_mangle] extern "C"` dropped -- the only caller
// (`proc/sysproc.rs`) already reaches it via `crate::proc::thread_clone`.
// RUSTIFY-PROC: that caller is in `crate::proc` (via the `pub use clone::*`
// glob) -> pub(super).
impl CloneArgs {
    /// Preserve the syscall adapter's null-tolerant raw-argument boundary;
    /// all fallible construction below uses Rust ownership and `KResult`.
    pub(super) fn spawn(args: *mut CloneArgs) -> c_int {
        let Some(args) = clone_args_mut(args) else {
            return Errno::Inval.neg();
        };
        args.spawn_inner().unwrap_or_else(Errno::neg)
    }

    fn spawn_inner(&mut self) -> KResult<c_int> {
        let args = self;
        let p = xv6_current_thread();

        if xv6_t_user_space(p) == 0 {
            return Err(Errno::Inval);
        }

        // CLONE_THREAD implies CLONE_PARENT + requires CLONE_VM | CLONE_SIGHAND
        if args.flags & CLONE_THREAD != 0 {
            if args.flags & (CLONE_VM | CLONE_SIGHAND) != (CLONE_VM | CLONE_SIGHAND) {
                return Err(Errno::Inval);
            }
            args.flags |= CLONE_PARENT;
        }

        // CLONE_VM without CLONE_VFORK requires stack and entry
        if args.flags & CLONE_VM != 0
            && args.flags & CLONE_VFORK == 0
            && (args.stack == 0 || args.entry == 0)
        {
            return Err(Errno::Inval);
        }

        // Stack alignment check
        if args.stack != 0
            && (args.stack_size < USERSTACK_MINSZ || (args.stack_size & (PAGE_SIZE - 1)) != 0)
        {
            return Err(Errno::Inval);
        }

        let stack_top = if args.stack == 0 {
            None
        } else {
            Some(
                args.stack
                    .checked_add(args.stack_size)
                    .ok_or(Errno::Inval)?
                    & !0xFu64,
            )
        };

        let pending = PendingChild::new(xv6_t_kstack_order(p))?;
        let child = pending.as_ptr();

        // Install each acquired reference before another allocation can fail.
        let new_vm = if args.flags & CLONE_VM != 0 {
            let vm = xv6_t_vm(p);
            vm_dup(vm);
            vm
        } else {
            check_ptr(vm_copy(xv6_t_vm(p)))?
        };
        xv6_t_set_vm(child, new_vm);
        xv6_t_set_fs(child, check_ptr(vfs_struct_clone(xv6_t_fs(p), args.flags))?);
        xv6_t_set_fdtable(
            child,
            check_ptr(vfs_fdtable_clone(xv6_t_fdtable(p), args.flags as c_int))?,
        );

        let p_sigacts = xv6_t_sigacts(p);
        if !p_sigacts.is_null() {
            let sigacts = check_ptr(crate::proc::signal::SigActs::dup(
                p_sigacts as *mut crate::bindings::sigacts,
                args.flags,
            ))?;
            xv6_t_set_sigacts(child, sigacts.cast());
        }

        // Reserve the private group before publishing the child or parking a
        // vfork parent. Its temporary -1 IDs are finalized under pid_lock once
        // the numeric PID has been assigned, before either object is visible.
        if args.flags & CLONE_THREAD == 0 {
            ThreadGroup::alloc(child)?;
        }

        // Per-thread signal mask
        crate::proc::signal::ThreadSignal::sigpending_clone(
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
        if let Some(stack_top) = stack_top {
            xv6_t_trapframe_set_sp(child, stack_top);
        }
        xv6_t_trapframe_set_a0(child, 0);
        xv6_t_copy_name(child, p);

        // All remaining operations are allocation-free. Disarm rollback before
        // taking locks or moving the child out of its initial UNUSED state.
        pending.commit();

        // SAFETY: `child` is the freshly-allocated, live `*mut thread` this
        // function is constructing; the handle only takes/releases its own
        // control-block lock (former `xv6_tcb_lock` shim, now the method).
        unsafe { ThreadAccess::assume(child) }.tcb_lock();
        xv6_t_set_user_space(child);
        xv6_thread_state_set(child, THREAD_UNINTERRUPTIBLE);
        Rq::task_fork(xv6_t_sched_entity(child) as *mut crate::bindings::sched_entity);
        if args.flags & CLONE_VFORK != 0 {
            xv6_t_set_vfork_parent(child, p);
            xv6_thread_state_set(p, THREAD_UNINTERRUPTIBLE);
        } else {
            xv6_t_set_vfork_parent(child, ptr::null_mut());
        }
        // SAFETY: as the matching `tcb_lock` above.
        unsafe { ThreadAccess::assume(child) }.tcb_unlock();

        // Assign and publish under one uninterrupted write lock. A PID is never
        // left unregistered across an unlock, including at numeric wraparound.
        xv6_pid_wlock();
        let child_pid = ProcTable::assign_pid(child);
        if args.flags & CLONE_THREAD == 0 {
            // SAFETY: successful group construction installed this private group.
            unsafe { ThreadGroupAccess::assume(xv6_t_thread_group(child)) }.set_tgid(child_pid);
            xv6_t_set_tgid(child, child_pid);
        }
        let real_parent = if args.flags & CLONE_PARENT != 0 {
            xv6_t_parent(p)
        } else {
            p
        };

        if args.flags & CLONE_THREAD != 0 {
            xv6_t_set_parent(child, real_parent);
        } else {
            // SAFETY: `real_parent`/`child` are live threads (handle contract).
            unsafe { ThreadAccess::assume(real_parent) }.attach_child(child);
        }

        if args.flags & CLONE_THREAD != 0 {
            let parent_tg = xv6_t_thread_group(p);
            if parent_tg.is_null() {
                panic_clone("clone: parent has no thread_group for CLONE_THREAD");
            }
            // NO-STANDALONE-FN: former `thread_group_add(parent_tg, child)` delegator.
            // SAFETY: `parent_tg` is checked non-null immediately above.
            unsafe { ThreadGroupAccess::assume(parent_tg) }.add_thread(child);
            xv6_t_set_tgid(child, xv6_t_tgid(p));
        }

        let parent_pg = xv6_t_pgroup(p);
        let parent_sess = xv6_t_session(p);
        if !parent_pg.is_null() {
            if args.flags & CLONE_THREAD == 0 {
                Pgroup::add_tg(parent_pg, xv6_t_thread_group(child));
            }
            Pgroup::add_thread(parent_pg, child);
        }
        if !parent_sess.is_null() {
            session_add_thread(parent_sess, child);
        }
        ProcTable::publish(child);
        xv6_pid_wunlock();

        Scheduler::wakeup_thread(child);

        if args.flags & CLONE_VFORK != 0 {
            Scheduler::yield_now();
        }

        // A CLONE_THREAD child can exit and be reclaimed immediately after wake;
        // the return value was captured before publication, while we owned it.
        Ok(child_pid)
    }
}
