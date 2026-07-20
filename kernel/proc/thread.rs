//! Pure-Rust port of `kernel/proc/thread.c`.
//!
//! Owns the canonical public ABI symbols: tcb_lock/unlock,
//! proc_assert_holding, thread_init, attach_child, detach_child,
//! thread_create, kthread_create, idle_thread_init, thread_destroy,
//! userinit, install_user_root. The `xv6_thport_*` C-ABI alias layer that
//! used to front these for sibling Rust/C ports was collapsed in the
//! P3-1B2 sweep -- callers now use these names directly.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

// See `crate::u`'s doc comment (kernel/lib.rs) for the macro's contract.
use crate::u;

use core::ffi::{c_char, c_int, c_void};
use core::mem::offset_of;
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::bindings::{
    context, fs_struct, list_node_t, hlist_entry_t, page_t, pgroup, pid_t,
    rcu_head_t, rq, sched_attr, sched_entity, session, sigacts as sigacts_t, spinlock_t, thread,
    thread_group, thread_signal_t, thread_state, utrapframe, vfs_fdtable, vfs_inode,
    vfs_inode_ref, vm_t, workqueue,
};
use crate::lock::rcu::rcu_check_callbacks;
use crate::machine::{cpu_local_ptr, cpuid, intr_on, read_sp};
use crate::proc::access::{
    is_err, is_err_or_null, ptr_err, list_node_init_raw, CpuLocalRef, PgroupAccess,
    RqRef, SchedEntityRef, SessionAccess, SpinLockRef, ThreadAccess, ThreadSignalAccess,
};
use crate::kstd::{result_to_errptr, Errno, GenKey, GenTable, KResult};
use crate::tty::session::{session_key_of, session_lookup, Sid};
use crate::proc::pgroup::{pgroup_key_of, pgroup_lookup, Pgid};
use core::ptr::NonNull;
use crate::proc::proc_shims::xv6_panic;
use crate::proc::__proctab_init;
use crate::proc::pid_assert_wholding;
use crate::proc::sigacts_init;
use crate::proc::sigstack_init;
use crate::proc::rq_lock_current;
use crate::proc::rq_unlock_current;
use crate::proc::sched_attr_init;
use crate::proc::sched_entity_init;
use crate::proc::sched_setattr;
use crate::proc::sigacts_put;
use crate::proc::sigpending_destroy;
use crate::proc::sigpending_init;
// P3-1B2: these cross into sibling proc submodules and were previously
// bridged via `xv6_sigport_*`/`xv6_schport_*`/`xv6_rqport_*` `extern "C"`
// redeclarations (some inline above, some in the block below); now direct
// crate-path items, same precedent as the `use` imports above.
use crate::proc::{sigpending_empty, context_switch_finish, scheduler_wakeup,
    rq_cpu_activate, get_rq_for_cpu, thread_group_init, thread_group_put};
// P3-D2b: the pid/thread-group/pgroup entry points (proc/{pid,
// thread_group,pgroup}.rs) are ordinary Rust fns now that their
// `#[no_mangle]` exports are gone; reached as plain crate-path items
// instead of the `extern "C"` redeclarations that used to sit in the
// "extern C primitives" block below. `pgroup_alloc`/`pgroup_add_tg`/
// `pgroup_add_thread` used to be redeclared here with `*mut c_void`
// pgroup handles; the call sites now use the real `*mut pgroup` type
// (see `KERNEL_PG`).
use crate::proc::{__proctab_get_initproc, pid_wlock, pid_wunlock,
    thread_group_alloc_kernel, thread_group_add,
    pgroup_alloc, pgroup_add_tg, pgroup_add_thread};
// P3-1B mesh sweep: all three cross top-level-module boundaries (proc's
// submodules are private, so the fully-qualified submodule path is used
// rather than risking an ambiguous glob re-export at `crate::<mod>::`
// level -- see `timer/sched_timer.rs`'s module doc for the `E0659` class
// this sidesteps).
use crate::exec::exec;
use crate::irq::trap::usertrapret;
// P3-D3c: `proc/exit.rs`'s `exit` is a plain (safe) Rust fn now that its
// `#[no_mangle]` export is gone -- imported via its private sibling module
// path.
use super::exit::exit;
use crate::start_kernel::start_kernel_post_init;
use crate::proc::pid::{__alloc_pid, __free_pid};

/// `pid::proctab_proc_add`/`pid::__proctab_set_initproc` take
/// `pid::Thread`, a distinct (but layout-identical) opaque marker from
/// this file's own `bindings::thread` -- reinterpret via thin wrappers,
/// same precedent as `sysproc.rs`'s `thread_clone`.
#[inline(always)]
fn proctab_proc_add(p: *mut thread) {
    crate::proc::pid::proctab_proc_add(p as *mut c_void as *mut crate::proc::pid::Thread);
}
#[inline(always)]
fn __proctab_set_initproc(p: *mut thread) {
    crate::proc::pid::__proctab_set_initproc(p as *mut c_void as *mut crate::proc::pid::Thread);
}

// ===========================================================================
// Native layout — Wave P3-N9 (the thread family hub).
//
// This IS the kernel-wide Rust definition of `struct thread`
// (`kernel/inc/proc/thread_types.h`) now: `build.rs` blocklists the
// bindgen-generated form (plus its four `thread__bindgen_ty_*` shells)
// and injects `pub use crate::proc::thread::Thread as thread;`, so every
// `crate::bindings::thread` path across the crate resolves here.
//
// The C struct separates its hot-field groups with four
// `__STRUCT_CACHELINE_PADDING` markers (`struct {}
// __attribute__((aligned(64)))`, kernel/inc/compiler.h) — bindgen
// emitted those as the four zero-sized align(64) `thread__bindgen_ty_*`
// shells plus two explicit `__bindgen_padding_*` arrays where the
// preceding field group didn't already end on a cacheline. The native
// reproduces the byte layout with explicit private `_pad*` arrays (N7
// `RqPercpu`/`SchedEntity` precedent); the shells themselves vanish
// (nothing ever read them — grep-verified, they are zero-sized).
//
// Mixed-tier fields (P3-5 asm-offset set, stay bindgen until then):
// `trapframe: *mut utrapframe` is a POINTER (transparent);
// `sched_entity` likewise. Nothing here embeds context/trapframe/
// cpu_local by value. `signal: thread_signal_t` re-points to the native
// `ThreadSignal` (same wave, kernel/proc/signal.rs).
//
// ASM-OFFSET FINDING (P3-N9): `thread` IS in the gen_asm_offsets.py set
// (kernel/inc/CMakeLists.txt generates THREAD_* defines into
// build/kernel/inc/asm-offsets.h, THREAD_SIZE 1152) — but NO .S file
// consumes any THREAD_* macro (grep over kernel/**/*.S: kernelvec.S/
// trampoline.S use only TRAPFRAME_*/UTRAPFRAME_*/CPU_LOCAL_*; swtch.S
// uses hardcoded context offsets). The generator reads the C header
// (which remains authoritative and untouched), so the generated defines
// stay correct regardless; the hardcoded asserts below additionally
// pin the native to those exact gcc values field-by-field.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen form + riscv64-unknown-elf-gcc `_Static_assert` probe
// (rv64gc/lp64d — scratchpad p3n9_static_assert_probe.c) + the
// gcc-generated asm-offsets.h THREAD_* values; all three agree on every
// size/align/offset (no pipe-style divergence, N5 precedent checked).
//
// Derive fidelity: bindgen derived Copy/Clone (as it does for every
// non-degraded struct); the native keeps them, and build.rs's
// `NativeTypeCallbacks` answers Copy=Yes for `thread` (nothing
// still-bindgen embeds it by value — the only by-value context is the
// kernel-stack head arranged by `kstack_arrange` below).
// ===========================================================================

// ===========================================================================
// `Tid` / `TID_TAG` — the thread family's generational key  [N-R6d-1]
// ===========================================================================
//
// The internal generational handle for a thread, registered in
// [`THREAD_TABLE`] (see below). `TID_TAG` is a distinct compile-time family
// discriminator from `SID_TAG` (the session pilot, N-R6a), so a `Tid` and a
// `Sid` are non-interchangeable types even though they share the one
// [`GenTable`] implementation. `Tid` is *not* the userspace identity — `pid`
// (the `Thread::pid` field) stays exactly that, unchanged; `Tid` is the
// kernel-internal, generation-checked "is this thread still the same live
// object?" handle that replaces raw `*mut Thread` relationship edges.

/// Family tag for thread keys — an ASCII marker distinct from `SID_TAG`
/// (`b"SESSION\0"`); only its distinctness matters (it makes [`Tid`] a
/// different, non-interchangeable type from `Sid`/any future family key).
const TID_TAG: u64 = u64::from_le_bytes(*b"THREAD\0\0");

/// Typed generational handle to a live thread in [`THREAD_TABLE`], distinct
/// from the userspace numeric `pid`. A stale `Tid` (its thread exited and was
/// removed from the table) resolves to `None`/null. The `NONE` sentinel
/// (`generation == 0`) is the in-struct "no thread" edge — see the `parent`
/// field, which stores a bare `Tid` (8 bytes, generation-0 == "no parent").
pub(super) type Tid = GenKey<TID_TAG>;

/// Native `struct thread` (`kernel/inc/proc/thread_types.h`) — the
/// per-thread control block, laid out at the top of its kernel stack.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct Thread {
    // ===== Cache line 0: lock (isolated against false sharing) =====
    pub lock: spinlock_t,
    // C `__STRUCT_CACHELINE_PADDING` #1 (bindgen `__bindgen_anon_1`,
    // zero-sized align(64) shell): next field starts at 64.
    _pad0: [u8; 40],
    // ===== Cache line 1: scheduler hot path =====
    pub state: thread_state,
    pub flags: u64,
    pub sched_entity: *mut sched_entity,
    pub ksp: u64,
    pub chan: *mut c_void,
    pub sched_entry: list_node_t,
    pub wq: *mut workqueue,
    // C `__STRUCT_CACHELINE_PADDING` #2 (bindgen `__bindgen_anon_2`):
    // the group above already ends exactly at 128 — zero bytes.
    // ===== Cache line 2: trap/syscall hot path + cold init =====
    pub trapframe: *mut utrapframe,
    pub vm: *mut vm_t,
    pub clone_flags: u64,
    pub kentry: u64,
    pub arg: [u64; 2],
    pub name: [c_char; 16],
    pub kstack_order: c_int,
    pub kstack: u64,
    pub trapframe_vbase: u64,
    pub fs: *mut fs_struct,
    pub fdtable: *mut vfs_fdtable,
    pub rcu_read_lock_nesting: c_int,
    // C `__STRUCT_CACHELINE_PADDING` #3 (bindgen `__bindgen_padding_0:
    // [u64; 2]` + the zero-sized `__bindgen_anon_3`): 236 -> 256.
    _pad1: [u64; 2],
    // ===== Cache line 3+: family tree (fork/exit/wait) =====
    pub sigacts: *mut sigacts_t,
    // N-R6d-1: the FIRST relationship edge converted off `*mut Thread` to a
    // generational key. Stored as a bare [`Tid`] (8 bytes, same span the
    // `*mut Thread` occupied — `GenKey{index:u32, generation:u32}` is 8
    // bytes/align 4, so offset 264 and every following offset are unchanged;
    // the layout asserts below prove it). `Tid::NONE` (`generation == 0`) is
    // the "no parent" edge (init and detached children). Resolved to a
    // `*mut thread` through [`THREAD_TABLE`] by the `parent` accessors
    // (`ThreadAccess::parent_ptr`/`set_parent` → `proc_shims::t_parent`/
    // `t_set_parent` → `THREAD_TABLE.parent_resolve`/`parent_store`); a
    // stale `Tid` (parent freed) resolves to null — the existing "parent
    // gone" semantics reparent/wait already handle. `vfork_parent` below
    // stays `*mut Thread` (later sub-step).
    pub parent: Tid,
    pub vfork_parent: *mut Thread,
    pub thread_group: *mut thread_group,
    // N-R6d-2b: the pgroup edge converted off `*mut pgroup` to the process-group
    // family's generational key [`Pgid`] (`GenKey<PGID_TAG>`, 8 bytes/align 4 —
    // the exact span the `*mut pgroup` occupied, so offset 288 (already
    // 8-aligned) and every following offset are unchanged; the layout asserts
    // below prove it). `Pgid::NONE` (`generation == 0`) is the "no pgroup" edge.
    // Resolved to a `*mut pgroup` through [`PGROUP_TABLE`] by the `pgroup`
    // accessors (`ThreadAccess::pgroup_ptr`/`set_pgroup` →
    // `proc_shims::t_pgroup`/`t_set_pgroup` →
    // `thread_pgroup_resolve`/`thread_pgroup_store`); a stale `Pgid` (the pgroup
    // was freed) resolves to null — the existing "pgroup gone" semantics readers
    // already null-check. `thread_group` above stays `*mut` (later sub-step).
    pub pgroup: Pgid,
    // N-R6d-2a: the session back-edge converted off `*mut session` to the
    // session family's generational key [`Sid`] (`GenKey<SID_TAG>`, 8
    // bytes/align 4 — the exact span the `*mut session` occupied, so offset
    // 296 and every following offset are unchanged; the layout asserts below
    // prove it). `Sid::NONE` (`generation == 0`) is the "no session" edge.
    // Resolved to a `*mut session` through the pilot's [`SESSION_TABLE`] by
    // the `session` accessors (`ThreadAccess::session_ptr`/`set_session` →
    // `proc_shims::t_session`/`t_set_session` →
    // `thread_session_resolve`/`thread_session_store`); a stale `Sid` (the
    // session was freed) resolves to null — the existing "session gone"
    // semantics every reader already null-checks. `thread_group`/`pgroup`
    // above stay `*mut` (later sub-steps).
    pub session: Sid,
    pub sid: pid_t,
    pub pgid: pid_t,
    pub tgid: pid_t,
    pub pid: pid_t,
    pub tg_entry: list_node_t,
    pub pg_entry: list_node_t,
    pub sid_entry: list_node_t,
    pub siblings: list_node_t,
    pub children: list_node_t,
    pub children_count: c_int,
    pub xstate: c_int,
    pub wq_entry: list_node_t,
    // C `__STRUCT_CACHELINE_PADDING` #4 (bindgen `__bindgen_padding_1:
    // [u64; 3]` + the zero-sized `__bindgen_anon_4`): 424 -> 448.
    _pad2: [u64; 3],
    // ===== Cold: registration / debug =====
    pub proctab_entry: hlist_entry_t,
    pub dmp_list_entry: list_node_t,
    // ===== Signal (large, pushed to end) =====
    pub signal: thread_signal_t,
    // ===== RCU deferred freeing (must be last) =====
    pub rcu_head: rcu_head_t,
    // struct align(64) tail-pads 1112 -> 1152, exactly as bindgen/gcc.
}

// P3-N9 hardcoded layout proof — every field of the hub, values captured
// from the pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate, independently confirmed by the gcc `_Static_assert`
// probe (scratchpad p3n9_static_assert_probe.c) AND equal to the
// gcc-generated build/kernel/inc/asm-offsets.h THREAD_* defines
// (THREAD_LOCK..THREAD_RCU_HEAD, THREAD_SIZE — no .S consumer exists,
// see the block note above).
const _: () = {
    assert!(core::mem::size_of::<Thread>() == 1152, "thread size (THREAD_SIZE)");
    assert!(core::mem::align_of::<Thread>() == 64, "thread alignment");
    assert!(core::mem::offset_of!(Thread, lock) == 0, "thread.lock offset");
    assert!(core::mem::offset_of!(Thread, state) == 64, "thread.state offset");
    assert!(core::mem::offset_of!(Thread, flags) == 72, "thread.flags offset");
    assert!(core::mem::offset_of!(Thread, sched_entity) == 80, "thread.sched_entity offset");
    assert!(core::mem::offset_of!(Thread, ksp) == 88, "thread.ksp offset");
    assert!(core::mem::offset_of!(Thread, chan) == 96, "thread.chan offset");
    assert!(core::mem::offset_of!(Thread, sched_entry) == 104, "thread.sched_entry offset");
    assert!(core::mem::offset_of!(Thread, wq) == 120, "thread.wq offset");
    assert!(core::mem::offset_of!(Thread, trapframe) == 128, "thread.trapframe offset");
    assert!(core::mem::offset_of!(Thread, vm) == 136, "thread.vm offset");
    assert!(core::mem::offset_of!(Thread, clone_flags) == 144, "thread.clone_flags offset");
    assert!(core::mem::offset_of!(Thread, kentry) == 152, "thread.kentry offset");
    assert!(core::mem::offset_of!(Thread, arg) == 160, "thread.arg offset");
    assert!(core::mem::offset_of!(Thread, name) == 176, "thread.name offset");
    assert!(core::mem::offset_of!(Thread, kstack_order) == 192, "thread.kstack_order offset");
    assert!(core::mem::offset_of!(Thread, kstack) == 200, "thread.kstack offset");
    assert!(
        core::mem::offset_of!(Thread, trapframe_vbase) == 208,
        "thread.trapframe_vbase offset"
    );
    assert!(core::mem::offset_of!(Thread, fs) == 216, "thread.fs offset");
    assert!(core::mem::offset_of!(Thread, fdtable) == 224, "thread.fdtable offset");
    assert!(
        core::mem::offset_of!(Thread, rcu_read_lock_nesting) == 232,
        "thread.rcu_read_lock_nesting offset"
    );
    assert!(core::mem::offset_of!(Thread, sigacts) == 256, "thread.sigacts offset");
    // N-R6d-1: `parent` is now a `Tid` (`GenKey`), not a `*mut Thread`. A
    // `GenKey{index:u32, generation:u32}` is exactly 8 bytes (align 4) — the
    // same span the pointer (8 bytes, align 8) occupied — so this offset and
    // every subsequent field offset are unchanged (the pointer's align-8 was
    // not needed to keep offset 264 8-aligned; 264 is already a multiple of
    // 8). The struct `size == 1152` assert above independently guards the tail.
    assert!(core::mem::size_of::<Tid>() == 8, "Tid must be 8 bytes to preserve thread.parent span");
    assert!(core::mem::offset_of!(Thread, parent) == 264, "thread.parent offset");
    assert!(core::mem::offset_of!(Thread, vfork_parent) == 272, "thread.vfork_parent offset");
    assert!(core::mem::offset_of!(Thread, thread_group) == 280, "thread.thread_group offset");
    // N-R6d-2b: `pgroup` is now a `Pgid` (`GenKey`, 8 bytes/align 4), not a
    // `*mut pgroup` (8 bytes/align 8). Same 8-byte span at an already-8-aligned
    // offset, so offset 288 and every following offset are unchanged; the struct
    // `size == 1152` assert above guards the tail.
    assert!(core::mem::size_of::<Pgid>() == 8, "Pgid must be 8 bytes to preserve thread.pgroup span");
    assert!(core::mem::offset_of!(Thread, pgroup) == 288, "thread.pgroup offset");
    // N-R6d-2a: `session` is now an `Sid` (`GenKey`), not a `*mut session`.
    // Like `Tid`, an `Sid` is 8 bytes (align 4) — the same span the pointer
    // occupied — so offset 296 (already 8-aligned) and every following offset
    // are unchanged; the struct `size == 1152` assert above guards the tail.
    assert!(core::mem::size_of::<Sid>() == 8, "Sid must be 8 bytes to preserve thread.session span");
    assert!(core::mem::offset_of!(Thread, session) == 296, "thread.session offset");
    assert!(core::mem::offset_of!(Thread, sid) == 304, "thread.sid offset");
    assert!(core::mem::offset_of!(Thread, pgid) == 308, "thread.pgid offset");
    assert!(core::mem::offset_of!(Thread, tgid) == 312, "thread.tgid offset");
    assert!(core::mem::offset_of!(Thread, pid) == 316, "thread.pid offset");
    assert!(core::mem::offset_of!(Thread, tg_entry) == 320, "thread.tg_entry offset");
    assert!(core::mem::offset_of!(Thread, pg_entry) == 336, "thread.pg_entry offset");
    assert!(core::mem::offset_of!(Thread, sid_entry) == 352, "thread.sid_entry offset");
    assert!(core::mem::offset_of!(Thread, siblings) == 368, "thread.siblings offset");
    assert!(core::mem::offset_of!(Thread, children) == 384, "thread.children offset");
    assert!(
        core::mem::offset_of!(Thread, children_count) == 400,
        "thread.children_count offset"
    );
    assert!(core::mem::offset_of!(Thread, xstate) == 404, "thread.xstate offset");
    assert!(core::mem::offset_of!(Thread, wq_entry) == 408, "thread.wq_entry offset");
    assert!(core::mem::offset_of!(Thread, proctab_entry) == 448, "thread.proctab_entry offset");
    assert!(
        core::mem::offset_of!(Thread, dmp_list_entry) == 472,
        "thread.dmp_list_entry offset"
    );
    assert!(core::mem::offset_of!(Thread, signal) == 488, "thread.signal offset");
    assert!(core::mem::offset_of!(Thread, rcu_head) == 1072, "thread.rcu_head offset");
};

// ===========================================================================
// `THREAD_TABLE` — the thread generational registry  [N-R6d-1]
// ===========================================================================
//
// A [`GenTable`] holding a stable `NonNull<Thread>` for every live thread,
// registered alongside the proc-table hlist (`proctab_proc_add`) and
// deregistered alongside it (`proctab_proc_remove`) — both already run under
// `pid_wlock`, so the registry stays in lock-step with the hlist without a
// new lock. The thread keeps its slab/kstack allocation (the table stores a
// pointer, never the object by value → nothing moves → the asm-adjacent
// `current`/`cpu_local.proc` raw `*mut Thread` stays valid, untouched here).
//
// Registration point (why `proctab_proc_add`, not the literal
// `thread_create`): the table is a shared registry; every mutation must be
// serialized. `proctab_proc_add`/`proctab_proc_remove` are the exact
// "thread joins / leaves the process registry" lifecycle points and are
// guaranteed under `pid_wlock` (they `panic` otherwise). `thread_create`
// runs *unlocked* and is also invoked on error paths for threads that never
// join the registry (`kthread_create` cleanup) — inserting there would race
// and would leave orphan entries. Registering with the hlist is the
// lock-correct realization of the §5c "register at creation" intent.
//
// Freeze/`noalias` screening (memory `freeze-noalias-hazard`): `GenTable` is
// `Freeze`, but mutators form `&mut` only under `pid_wlock` (exclusive) and
// the resolve/store helpers do a single, non-looping generation-checked
// lookup — never a `&Freeze` field read hoisted out of a spin/wait loop. The
// table hands out only raw `NonNull`/`*mut` (never `&Thread`), so no
// `&Freeze` of a concurrently-mutated pointee is formed either.

/// Registry capacity — `NR_THREAD` (`kernel/inc/param.h`, the kernel's hard
/// cap on live threads), the true upper bound, so [`GenTable::insert`] can
/// never return `None` where a thread was successfully created (no capacity
/// regression). All-`None`/zero at rest → BSS, not data-segment.
const NR_THREAD: usize = 10_000;

/// `UnsafeCell` + `unsafe impl Sync` newtype so the registry can be a
/// file-scope `static`, exactly like `proc_shims`'s `PROC_TABLE` and the
/// session pilot's `SESSION_TABLE`. The `GenTable` is **not** internally
/// synchronized; all access is serialized by `pid_lock` — `pid_wlock` for the
/// `&mut self` mutators (`insert`/`remove_ptr`), `pid_lock` (read, or a held
/// write lock) for the `&self` resolver (`get`/`key_of`).
pub(super) struct ThreadTableCell(core::cell::UnsafeCell<GenTable<Thread, NR_THREAD, TID_TAG>>);
// SAFETY: every access below is taken while holding `pid_lock` at the
// required strength (mutators assert `pid_wlock`); the raw `UnsafeCell`
// access is thus race-free, same discipline as `SESSION_TABLE`.
unsafe impl Sync for ThreadTableCell {}

pub(super) static THREAD_TABLE: ThreadTableCell =
    ThreadTableCell(core::cell::UnsafeCell::new(GenTable::new()));

// N-METH: the four registry operations that genuinely act on `THREAD_TABLE`
// (insert / remove / parent-edge resolve+store) are methods on the table
// newtype — the natural `table.op(thread)` shape the brief calls out for the
// GenTable family. The receiver is the registry; each body forms `&mut self`
// (mutators) or `&self` (resolvers) to the inner `GenTable` under `pid_lock`
// and hands back only raw `NonNull`/`*mut` — never a `&Thread` of a
// concurrently-mutated pointee — so no Freeze-noalias hazard is introduced
// (the `GenTable` doc above). The session/pgroup edge shims below stay free
// fns: they resolve through *foreign* tables (`SESSION_TABLE`/`PGROUP_TABLE`,
// other modules) and only touch the thread's own key field, so this table is
// not their receiver.
impl ThreadTableCell {
    /// Register `p`, returning its fresh [`Tid`] (discarded by callers, which
    /// address threads by their stable pointer for this sub-step). `None` only
    /// if the table is full — impossible given [`NR_THREAD`]'s sizing. Called
    /// from `proctab_proc_add` under `pid_wlock`.
    pub(super) fn insert(&self, p: *mut thread) -> Option<Tid> {
        pid_assert_wholding();
        let nn = NonNull::new(p)?;
        // SAFETY: `pid_wlock` is held (asserted above), so this `&mut` to the
        // registry is exclusive — no other hart holds a reference into the
        // table (resolvers need `pid_lock`, excluded by our write lock).
        let table = unsafe { &mut *self.0.get() };
        table.insert(nn)
    }

    /// Deregister `p` (by its stable pointer — the thread caches no key),
    /// bumping the slot generation so any outstanding [`Tid`] to it (e.g. a
    /// child's `parent` edge) goes stale → resolves to null. A no-op if `p` was
    /// never registered. Called from `proctab_proc_remove` under `pid_wlock`.
    pub(super) fn remove(&self, p: *mut thread) {
        pid_assert_wholding();
        let Some(nn) = NonNull::new(p) else { return };
        // SAFETY: `pid_wlock` is held (asserted above) — exclusive `&mut`.
        let table = unsafe { &mut *self.0.get() };
        table.remove_ptr(nn);
    }

    /// Resolve thread `p`'s stored `parent` [`Tid`] to a live `*mut thread`, or
    /// null if it is [`Tid::NONE`] ("no parent" — init / a detached child) or
    /// stale (the parent exited and was removed → the correct "parent gone"
    /// answer reparent/wait already handle). The backing read for
    /// `ThreadAccess::parent_ptr` / `proc_shims::t_parent`.
    ///
    /// Caller holds `pid_lock` where the edge is walked under it (attach/detach/
    /// reparent); the few pre-existing lock-free readers (e.g. `sys_getppid`)
    /// inherit exactly their prior best-effort profile — a single, non-looping
    /// generation-checked lookup (no freeze-hoist loop), and a *live* thread's
    /// parent slot is never concurrently removed (a parent with a live child is
    /// never reaped), so the resolved pointer is stable for them.
    pub(super) fn parent_resolve(&self, p: *mut thread) -> *mut thread {
        // SAFETY: `p` is a live `*mut thread` (accessor contract); reading its
        // own `parent` `Tid` field is a plain aligned word read.
        let tid: Tid = unsafe { (*p).parent };
        if tid.is_none() {
            return ptr::null_mut();
        }
        // SAFETY: shared `&self` into the registry; excludes every `&mut self`
        // mutator (they hold `pid_wlock`). No `&Thread` is formed — the raw
        // pointer is handed straight back (freeze note above).
        let table = unsafe { &*self.0.get() };
        match table.get(tid) {
            Some(nn) => nn.as_ptr(),
            None => ptr::null_mut(),
        }
    }

    /// Store `par` as thread `p`'s `parent` edge: `par == null` → [`Tid::NONE`]
    /// ("no parent"), otherwise `par`'s current [`Tid`] (reverse-looked-up in
    /// the registry). The backing write for `ThreadAccess::set_parent` /
    /// `proc_shims::t_set_parent`. If `par` is somehow unregistered it stores
    /// `NONE` (resolves to null) — the same "no reachable parent" outcome a
    /// dangling raw pointer would have been null-checked into.
    ///
    /// Caller holds `pid_wlock` (every `set_parent` site — attach/detach/clone
    /// — does; the registry read is race-free under it).
    pub(super) fn parent_store(&self, p: *mut thread, par: *mut thread) {
        let tid = match NonNull::new(par) {
            None => Tid::NONE,
            Some(nn) => {
                // SAFETY: shared `&self` into the registry under the caller's
                // `pid_wlock` (exclusive vs other mutators). Reverse lookup only.
                let table = unsafe { &*self.0.get() };
                table.key_of(nn).unwrap_or(Tid::NONE)
            }
        };
        // SAFETY: `p` is a live `*mut thread`; writing its own `parent` field
        // under the caller's `pid_wlock` (exclusive) is race-free.
        unsafe {
            (*p).parent = tid;
        }
    }
}

/// Resolve thread `p`'s stored `session` [`Sid`] to a live `*mut session`, or
/// null if it is [`Sid::NONE`] ("no session") or stale (the session exited and
/// was freed → the correct "session gone" answer every reader already
/// null-checks). The backing read for `ThreadAccess::session_ptr` /
/// `proc_shims::t_session`.
///
/// Caller holds `pid_lock` (reader or writer) — the resolution goes through
/// [`session_lookup`], which requires it and does a single non-looping,
/// generation-checked table read (no `&Session` formed — freeze-hazard note in
/// the `GenTable` doc).
pub(super) fn thread_session_resolve(p: *mut thread) -> *mut session {
    // SAFETY: `p` is a live `*mut thread` (accessor contract); reading its own
    // `session` `Sid` field is a plain aligned word read.
    let sid: Sid = unsafe { (*p).session };
    session_lookup(sid).unwrap_or(ptr::null_mut())
}

/// Store `s` as thread `p`'s `session` edge: `s == null` → [`Sid::NONE`] ("no
/// session"), otherwise `s`'s own cached [`Sid`] (`session.self_sid`, stamped
/// at registry insert). The backing write for `ThreadAccess::set_session` /
/// `proc_shims::t_set_session`.
///
/// Caller holds `pid_wlock` (every `set_session` site does).
pub(super) fn thread_session_store(p: *mut thread, s: *mut session) {
    let sid = session_key_of(s);
    // SAFETY: `p` is a live `*mut thread`; writing its own `session` field
    // under the caller's `pid_wlock` (exclusive) is race-free.
    unsafe {
        (*p).session = sid;
    }
}

/// Resolve thread `p`'s stored `pgroup` [`Pgid`] to a live `*mut pgroup`, or
/// null if it is [`Pgid::NONE`] ("no pgroup") or stale (the pgroup emptied and
/// was freed → the correct "pgroup gone" answer every reader already
/// null-checks). The backing read for `ThreadAccess::pgroup_ptr` /
/// `proc_shims::t_pgroup`.
///
/// Caller holds `pid_lock` (reader or writer) — the resolution goes through
/// [`pgroup_lookup`], which requires it and does a single non-looping,
/// generation-checked table read (no `&Pgroup` formed — freeze-hazard note in
/// the `GenTable` doc).
pub(super) fn thread_pgroup_resolve(p: *mut thread) -> *mut pgroup {
    // SAFETY: `p` is a live `*mut thread` (accessor contract); reading its own
    // `pgroup` `Pgid` field is a plain aligned word read.
    let pgid: Pgid = unsafe { (*p).pgroup };
    pgroup_lookup(pgid).unwrap_or(ptr::null_mut())
}

/// Store `pg` as thread `p`'s `pgroup` edge: `pg == null` → [`Pgid::NONE`] ("no
/// pgroup"), otherwise `pg`'s own cached [`Pgid`] (`pgroup.self_pgid`, stamped
/// at registry insert). The backing write for `ThreadAccess::set_pgroup` /
/// `proc_shims::t_set_pgroup`.
///
/// Caller holds `pid_wlock` (every `set_pgroup` site does).
pub(super) fn thread_pgroup_store(p: *mut thread, pg: *mut pgroup) {
    let pgid = pgroup_key_of(pg);
    // SAFETY: `p` is a live `*mut thread`; writing its own `pgroup` field under
    // the caller's `pid_wlock` (exclusive) is race-free.
    unsafe {
        (*p).pgroup = pgid;
    }
}

// ---------------- constants ----------------------------------------------
const PAGE_SHIFT: u32 = 12;
const PAGE_SIZE: u64 = 1 << PAGE_SHIFT;
const PAGE_MASK: u64 = PAGE_SIZE - 1;
const PAGE_BUDDY_MAX_ORDER: c_int = 10;
const CACHELINE_SIZE: u64 = 64;
const CACHELINE_MASK: u64 = CACHELINE_SIZE - 1;
const KERNEL_STACK_ORDER: c_int = 2;
const KERNEL_STACK_SIZE: u64 = 1u64 << (PAGE_SHIFT + KERNEL_STACK_ORDER as u32);

const IDLE_MAJOR_PRIORITY: c_int = 63;
// MAKE_PRIORITY(63, default_minor) — default minor is 0 in xv6, so IDLE_PRIORITY = 63<<2 = 252.
// But we just pass IDLE_MAJOR_PRIORITY into attr.priority via the rqport helper.
// Looking at the C: attr.priority = IDLE_PRIORITY. We must compute the same value.
// MAKE_PRIORITY(major, minor) = (major << 2) | (minor & 0x3); DEFAULT_MINOR_PRIORITY = 0.
const DEFAULT_MINOR_PRIORITY: c_int = 0;
const IDLE_PRIORITY: c_int = (IDLE_MAJOR_PRIORITY << 2) | (DEFAULT_MINOR_PRIORITY & 0x3);

const PAGE_TYPE_ANON: u64 = 1; // matches mm/page_type.h enum value

const KSTACK_ARRANGE_FLAGS_TF: u64 = 0x1;
const KSTACK_ARRANGE_FLAGS_ALL: u64 = KSTACK_ARRANGE_FLAGS_TF;

const KERNEL_HIERARCHY_ID: c_int = 0;

const EINVAL: c_int = 22;
const ENOMEM: c_int = 12;
const EAGAIN: c_int = 11;

// Thread states (matches enum thread_state in thread_types.h)
const THREAD_UNUSED: thread_state = 0;
const THREAD_USED: thread_state = 1;
const THREAD_INTERRUPTIBLE: thread_state = 2;
const THREAD_KIILABLE: thread_state = 3;
const THREAD_TIMER: thread_state = 4;
const THREAD_KIILABLE_TIMER: thread_state = 5;
const THREAD_UNINTERRUPTIBLE: thread_state = 6;
const THREAD_WAKENING: thread_state = 7;
const THREAD_RUNNING: thread_state = 8;
const THREAD_STOPPED: thread_state = 9;
const THREAD_EXITING: thread_state = 10;
const THREAD_ZOMBIE: thread_state = 11;

const THREAD_FLAG_USER_SPACE: u64 = 5;

// ---------------- extern C primitives -----------------------------------
unsafe extern "C" {
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn strncpy(d: *mut c_char, s: *const c_char, n: usize) -> *mut c_char;
    fn safestrcpy(d: *mut c_char, s: *const c_char, n: usize) -> *mut c_char;

    // NOTE: `vfs_struct_lock`/`vfs_struct_unlock` (kernel/inc/vfs/fs.h) are
    // `static inline` wrappers around `spin_lock`/`spin_unlock` — there is
    // no linker symbol to bind to, so `install_user_root_finish` below
    // reimplements them natively via `crate::sync::KSpinlock` instead of
    // declaring them here.

    // CPU / interrupts (only the cpuid trampoline exists as a real symbol)

}

// P3-D3c: the spinlock primitives are genuinely `unsafe fn`s in
// `crate::lock::spinlock` now that their `#[no_mangle]` exports are gone;
// this file's original extern declarations were plain (non-`safe`) `fn`s,
// so every call site already sits in an `unsafe` context -- the plain
// `use` keeps them unchanged.
use crate::lock::spinlock::{spin_holding, spin_init, spin_lock, spin_unlock};

// P3-D3b: lock/rcu.rs's `call_rcu` is a plain `pub(crate) unsafe fn` now
// that its `#[no_mangle]` export is gone; reached by crate path (the call
// site in `thread_destroy` already sits inside `thread_raw_layout!`'s
// `unsafe` block).
use crate::lock::rcu::call_rcu;

// P3-D3a: `vm_init`/`vm_put` (mm/vm.rs, ordinary safe Rust fns now that
// their `#[no_mangle]` exports are gone) and `page_alloc` (genuinely
// `unsafe fn` in `crate::mm::page`; its call site below already sits in
// an `unsafe` context) are reached as crate-path items instead of the
// `extern "C"` redeclarations that used to sit in the block above
// (the old decls' `vm_t` was this file's alias for `crate::bindings::vm`).
use crate::mm::{page_alloc, vm_init, vm_put};

// `page_free` is genuinely `unsafe fn` in `crate::mm::page`; this file's
// original extern declaration asserted `safe fn` (usual FFI facade) and
// typed `order` as `c_int` rather than the real `u64` (the C ABI only
// ever read a non-negative small value). The thin wrapper preserves both.
/// SAFETY: `ptr` must originate from `page_alloc` (see
/// `crate::mm::page::page_free`'s contract).
#[inline]
fn page_free(ptr: *mut c_void, order: c_int) {
    unsafe { crate::mm::page_free(ptr, order as u64) };
}

// P3-1C mesh sweep: tty/session.rs and vfs/{fs,inode,fdtable}.rs are in
// scope for this wave. `vfs_struct_clone`/`vfs_struct_put`/
// `vfs_inode_get_ref`/`vfs_iput`/`vfs_fdtable_put` have identical
// signatures to their real definitions (same `crate::bindings::*` types
// already imported above), so they become plain crate-path imports.
use crate::vfs::fdtable::vfs_fdtable_put;
use crate::vfs::fs::{vfs_inode_get_ref, vfs_struct_clone, vfs_struct_put};
use crate::vfs::inode::vfs_iput;

/// Thin opaque-pointer wrapper: this file treats the session/pgroup
/// handle as `*mut c_void` uniformly with `pgroup_alloc` (proc-owned,
/// genuinely opaque), while `tty::session::session_alloc`'s real
/// signature uses the concrete `*mut session` type (same
/// opaque-mirror-mismatch pattern the P3-1A/B mesh sweeps used
/// elsewhere in this crate).
fn session_alloc(sid: c_int) -> *mut c_void {
    crate::tty::session::session_alloc(sid) as *mut c_void
}

/// SAFETY: only called from `kthread_hierarchy_init_locked` with `s`/`pg`
/// just returned non-null by `session_alloc`/`pgroup_alloc` above (proven
/// by the preceding `kassert!`s) -- matches the real fn's contract.
fn session_add_pg(s: *mut c_void, pg: *mut c_void) -> c_int {
    unsafe { crate::tty::session::session_add_pg(s as *mut session, pg as *mut pgroup) }
}

/// SAFETY: only called from `kthread_join_hierarchy_locked` with a live
/// `s` (the kernel hierarchy's session, initialized once at first use)
/// and a live `p` (the caller's own thread) -- matches the real fn's
/// contract.
fn session_add_thread(s: *mut c_void, p: *mut thread) -> c_int {
    unsafe { crate::tty::session::session_add_thread(s as *mut session, p) }
}

/// SAFETY: only called from `userinit` while holding `pid_wlock`, with the
/// just-created init thread -- matches the real fn's contract.
fn session_init(p: *mut thread) {
    unsafe { crate::tty::session::session_init(p) }
}

/// Thin opaque-pointer wrapper: `install_user_root`/`install_user_root_finish`
/// deliberately keep the looked-up inode as `*mut c_void` until the callee
/// casts it back (see `install_user_root_finish`'s doc); `vfs::inode`'s real
/// `vfs_namei` returns the concrete `*mut vfs_inode`.
fn vfs_namei(path: *const c_char, path_len: usize) -> *mut c_void {
    crate::vfs::inode::vfs_namei(path, path_len) as *mut c_void
}

// ---------------- inline helpers ----------------------------------------
#[inline]
fn hlist_entry_init_rs(e: *mut hlist_entry_t) {
    // hlist_entry_init zeroes the inner list_entry; struct hlist_entry has
    // a list_node_t list_entry plus other fields. The C inline does
    // list_entry_init(&entry->list_entry). list_entry is at offset 0.
    // `list_node_init_raw` is a safe fn (encapsulates its own unsafe), and
    // the cast is a pointer reinterpretation with no dereference — no
    // unsafe context is actually needed here.
    list_node_init_raw(e as *mut list_node_t);
}

#[inline]
fn ta_of<'a>(p: *mut thread) -> Option<ThreadAccess<'a>> {
    ThreadAccess::from_ptr(p)
}

#[inline]
fn current_thread() -> *mut thread {
    // SAFETY: `cpu_local_ptr()` indexes into statically-allocated, always-initialized
    // per-CPU storage; never null.
    unsafe { CpuLocalRef::assume(cpu_local_ptr()) }.proc_ptr()
}

#[inline]
fn thread_state_set(p: *mut thread, s: thread_state) {
    if let Some(t) = ta_of(p) { t.state_store(s as u32); }
}

// N-METH: the derived thread-state predicates and the user-space-flag action
// that used to be the free fns `thread_is_awoken`/`thread_is_sleeping`/
// `thread_set_user_space` are inherent methods on the `ThreadAccess` handle
// (defined in `proc::access`, extended here in-crate). They read only through
// the handle's existing `pub` sound accessors (`state_load`/`flags_set_bit`) —
// never forming a `&Thread` — so they carry the same no-Freeze-hazard profile
// as every other `ThreadAccess` accessor (the brief's "route through
// ThreadAccess" sound form), while living beside the `THREAD_*` state
// constants that are their single source of truth. Kept module-private (used
// only by this file's create/destroy paths).
impl<'a> ThreadAccess<'a> {
    #[inline]
    fn is_awoken(&self) -> bool {
        let s = self.state_load() as thread_state;
        s == THREAD_RUNNING || s == THREAD_WAKENING
    }
    #[inline]
    fn is_sleeping(&self) -> bool {
        let s = self.state_load() as thread_state;
        s == THREAD_INTERRUPTIBLE || s == THREAD_UNINTERRUPTIBLE
            || s == THREAD_KIILABLE || s == THREAD_TIMER || s == THREAD_KIILABLE_TIMER
    }
    #[inline]
    fn set_user_space(&self) {
        self.flags_set_bit(THREAD_FLAG_USER_SPACE);
        core::sync::atomic::fence(Ordering::SeqCst);
    }
}

#[inline]
fn thread_from_context(ctx: *mut context)-> *mut thread  {
    // `wrapping_sub`/`as` are pointer arithmetic with no dereference;
    // `SchedEntityRef::assume` is `unsafe fn` (see its `# Safety` doc in
    // access.rs) and is wrapped below with the site-specific justification.
    let off = offset_of!(sched_entity, context);
    let se = (ctx as *mut u8).wrapping_sub(off) as *mut sched_entity;
    // SAFETY: `se` is derived by pointer arithmetic assuming `ctx` points to the
    // `context` field of a live `sched_entity` -- the documented contract of
    // `thread_from_context`, a kernel-internal context-switch helper always
    // invoked with such a pointer.
    unsafe { SchedEntityRef::assume(se) }.thread_ptr()
}

// ---------------- panic macro -------------------------------------------
macro_rules! kpanic {
    ($msg:expr) => {{ xv6_panic(concat!($msg, "\0").as_ptr() as *const c_char) }};
}
macro_rules! kassert {
    ($cond:expr, $msg:expr) => {{ if !($cond) { kpanic!($msg); } }};
}
/// SAFETY: wraps raw field/layout manipulation of a `thread` (and its
/// embedded `sched_entity`/`utrapframe`) at a stack address computed by
/// the specific call site — e.g. `kstack_arrange`'s caller-supplied
/// `kstack`/`kstack_size`, or `idle_thread_init`'s `read_sp()`-derived
/// boot stack. Validity of every raw pointer touched inside a
/// `thread_raw_layout! { ... }` body comes from that call site's own
/// arrangement/ownership logic, not from any single blanket invariant
/// here; see the individual call sites for the actual argument.
macro_rules! thread_raw_layout {
    ($($body:tt)*) => {{
        #[allow(unused_unsafe)]
        u! { $($body)* }
    }};
}

// ---------------- kernel hierarchy statics ------------------------------
static KERNEL_TG: AtomicPtr<thread_group> = AtomicPtr::new(ptr::null_mut());
static KERNEL_PG: AtomicPtr<pgroup> = AtomicPtr::new(ptr::null_mut());
static KERNEL_SESSION: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());

fn kthread_hierarchy_init_locked() {
    // Every call in this function is a safe `pub extern "C" fn`
    // (`pid_assert_wholding`, `thread_group_alloc_kernel`, `pgroup_alloc`,
    // `session_alloc`, `session_add_pg`, `pgroup_add_tg`, `xv6_panic` via
    // `kassert!`), an atomic load/store, or a small `unsafe { }` block
    // (`PgroupAccess::assume`/`SessionAccess::assume`, each justified
    // in-line below) — no function-wide unsafe context is needed.
    pid_assert_wholding();
    if !KERNEL_TG.load(Ordering::Acquire).is_null() {
        return;
    }

    let mut tg: *mut thread_group = ptr::null_mut();
    let ret = thread_group_alloc_kernel(&mut tg as *mut _, KERNEL_HIERARCHY_ID);
    kassert!(ret == 0 && !tg.is_null(), "kthread hierarchy: thread_group_alloc_kernel failed");

    let pg = pgroup_alloc(KERNEL_HIERARCHY_ID, ptr::null_mut());
    kassert!(!pg.is_null(), "kthread hierarchy: pgroup_alloc failed");
    // pgroup->is_kernel = 1 (was the C `pgroup_mark_kernel` bridge helper;
    // now a direct bitfield write through PgroupAccess — no FFI needed).
    // SAFETY: `pg` is proven non-null by the diverging `kassert!` immediately above.
    unsafe { PgroupAccess::assume(pg) }.mark_kernel();

    let s = session_alloc(KERNEL_HIERARCHY_ID);
    kassert!(!s.is_null(), "kthread hierarchy: session_alloc failed");
    // SAFETY: `s` is proven non-null by the diverging `kassert!` immediately above.
    unsafe { SessionAccess::assume(s as *mut session) }.mark_kernel();

    let ret = session_add_pg(s, pg as *mut c_void);
    kassert!(ret == 0, "kthread hierarchy: session_add_pg failed");
    let ret = pgroup_add_tg(pg, tg);
    kassert!(ret == 0, "kthread hierarchy: pgroup_add_tg failed");

    KERNEL_TG.store(tg, Ordering::Release);
    KERNEL_PG.store(pg, Ordering::Release);
    KERNEL_SESSION.store(s, Ordering::Release);
}

fn kthread_join_hierarchy_locked(p: *mut thread) {
    // Same rationale as `kthread_hierarchy_init_locked`: every call here
    // is a safe `pub extern "C" fn` or `xv6_panic` via `kassert!` — no
    // unsafe context is actually needed here.
    pid_assert_wholding();
    kassert!(!p.is_null(), "kthread hierarchy: thread is NULL");
    kthread_hierarchy_init_locked();

    thread_group_add(KERNEL_TG.load(Ordering::Acquire), p);
    let ret = pgroup_add_thread(KERNEL_PG.load(Ordering::Acquire), p);
    kassert!(ret == 0, "kthread hierarchy: pgroup_add_thread failed");
    let ret = session_add_thread(KERNEL_SESSION.load(Ordering::Acquire), p);
    kassert!(ret == 0, "kthread hierarchy: session_add_thread failed");
}

// ---------------- pcb init / kstack arrange -----------------------------
// NOTE: these two functions' bodies are entirely a single
// `thread_raw_layout! { ... }` invocation, which already supplies its own
// `unsafe` block (see the macro's doc comment above) — an extra outer
// `u! { ... }` around the whole function body would just be a redundant,
// vacuous wrap, so it has been removed.
fn pcb_init(p: *mut thread, fdtable: *mut vfs_fdtable) {
    thread_raw_layout! {
        let ta = ThreadAccess::assume(p);
        thread_state_set(p, THREAD_UNUSED);
        sigpending_init(p);
        // Was the C `sigstack_init_for_thread` bridge helper
        // (`sigstack_init(&p->signal.sig_stack)`); now a direct call through
        // the typed field pointer.
        sigstack_init(ThreadSignalAccess::assume_thread(p).sig_stack_ptr());
        ta.init_embedded_lists_and_lock(c"thread".as_ptr() as *mut c_char);
        ta.set_thread_group(ptr::null_mut());
        ta.set_fs(ptr::null_mut());
        ta.set_fdtable(fdtable);
        if !ta.sched_entity_ptr().is_null() {
            ta.zero_sched_entity_storage();
            sched_entity_init(ta.sched_entity_ptr(), p);
        }
    }
}

fn kstack_arrange(kstack: *mut c_void, kstack_size: u64, flags: u64)-> *mut thread  {
    thread_raw_layout! {
        let p = (kstack as *mut u8).add((kstack_size as usize) - core::mem::size_of::<thread>()) as *mut thread;
        let mut next_addr = p as u64;

        let mut trapframe: *mut utrapframe = ptr::null_mut();
        let fdtable: *mut vfs_fdtable = ptr::null_mut();

        if (flags & KSTACK_ARRANGE_FLAGS_TF) != 0 {
            next_addr = (p as u64) - (core::mem::size_of::<utrapframe>() as u64) - 16;
            next_addr &= !0x7u64;
            trapframe = next_addr as *mut utrapframe;
        }

        next_addr = next_addr - (core::mem::size_of::<sched_entity>() as u64);
        next_addr &= !CACHELINE_MASK;
        let ta = ThreadAccess::assume(p);
        ta.set_sched_entity_ptr(next_addr as *mut sched_entity);

        pcb_init(p, fdtable);

        ta.set_trapframe(trapframe);

        let mut ksp = next_addr - 16;
        ksp &= !0x7u64;
        ta.set_ksp(ksp);

        p
    }
}

// ---------------- public canonical ABI ----------------------------------
fn thread_lock_ref<'a>(p: *mut thread) -> Option<SpinLockRef<'a>> {
    ta_of(p).map(|t| t.lock_ref())
}

fn tcb_lock_impl(p: *mut thread) {
    kassert!(!p.is_null(), "tcb_lock: thread is NULL");
    thread_lock_ref(p).expect("BUG: tcb_lock called with null/invalid thread pointer").lock();
}
fn tcb_unlock_impl(p: *mut thread) {
    kassert!(!p.is_null(), "tcb_unlock: thread is NULL");
    thread_lock_ref(p).expect("BUG: tcb_unlock called with null/invalid thread pointer").unlock();
}
fn proc_assert_holding_impl(p: *mut thread) {
    kassert!(!p.is_null(), "proc_assert_holding: thread is NULL");
    kassert!(
        thread_lock_ref(p).expect("BUG: proc_assert_holding called with null/invalid thread pointer").holding(),
        "proc_assert_holding: thread lock not held"
    );
}

pub(super) fn tcb_lock(p: *mut thread) { tcb_lock_impl(p) }
pub(super) fn tcb_unlock(p: *mut thread) { tcb_unlock_impl(p) }
pub(super) fn proc_assert_holding(p: *mut thread) { proc_assert_holding_impl(p) }

// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn thread_init() { __proctab_init(); }

pub(super) fn attach_child(parent: *mut thread, child: *mut thread) {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        kassert!(!parent.is_null(), "attach_child: parent is NULL");
        kassert!(!child.is_null(), "attach_child: child is NULL");
        let pta = ThreadAccess::assume(parent);
        let cta = ThreadAccess::assume(child);
        kassert!(child != __proctab_get_initproc(), "attach_child: child is init process");
        pid_assert_wholding();
        kassert!(cta.siblings_ref().is_detached(), "attach_child: child is attached to a parent");
        kassert!(cta.parent_ptr().is_null(), "attach_child: child has a parent");

        cta.set_parent(parent);
        cta.siblings_ref().push_back(pta.children_ptr());
        pta.inc_children_count();
    }
}

pub(super) fn detach_child(parent: *mut thread, child: *mut thread) {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        kassert!(!parent.is_null(), "detach_child: parent is NULL");
        kassert!(!child.is_null(), "detach_child: child is NULL");
        let pta = ThreadAccess::assume(parent);
        let cta = ThreadAccess::assume(child);
        pid_assert_wholding();
        kassert!(pta.children_count() > 0, "detach_child: parent has no children");
        kassert!(!cta.siblings_ref().is_empty(), "detach_child: child is not a sibling of parent");
        kassert!(!cta.siblings_ref().is_detached(), "detach_child: child is already detached");
        kassert!(cta.parent_ptr() == parent, "detach_child: child is not a child of parent");

        cta.siblings_ref().detach();
        pta.dec_children_count();
        cta.set_parent(ptr::null_mut());

        kassert!(pta.children_count() > 0 || pta.children_ref().is_empty(),
                 "detach_child: parent has no children after detaching child");
    }
}

fn thread_create_inner(entry: *mut c_void, arg1: u64, arg2: u64, kstack_order: c_int) -> KResult<*mut thread> {
    // The guard check and shift below are plain safe arithmetic/calls;
    // the rest of the body is a `thread_raw_layout! { ... }` invocation,
    // which already supplies its own `unsafe` block — so the outer wrap
    // has been removed as redundant rather than genuinely needed.
    if kstack_order < 0 || kstack_order > PAGE_BUDDY_MAX_ORDER {
        return Err(Errno::Inval);
    }
    let kstack_size = 1u64 << (PAGE_SHIFT + kstack_order as u32);
    thread_raw_layout! {
        let kstack = page_alloc(kstack_order as u64, PAGE_TYPE_ANON);
        if kstack.is_null() {
            return Err(Errno::NoMem);
        }
        memset((kstack as *mut u8).add((kstack_size - PAGE_SIZE) as usize) as *mut c_void,
               0, PAGE_SIZE as usize);

        let p = kstack_arrange(kstack, kstack_size, KSTACK_ARRANGE_FLAGS_ALL);

         let ta = ThreadAccess::assume(p);
         ta.set_kstack_order(kstack_order);
         ta.set_kstack_addr(kstack as u64);
         let se = ta.sched_entity_ptr();
         let se_ref = SchedEntityRef::assume(se);
         se_ref.zero_context();
         se_ref.init_context(entry as u64, ta.ksp());
         ta.set_kentry(entry as u64);
         ta.set_arg(0, arg1);
         ta.set_arg(1, arg2);
        ta.set_pid(-1);
        ta.set_tgid(-1);
        ta.set_pgid(-1);
        ta.set_sid(-1);

        sched_entity_init(se, p);
        Ok(p)
    }
}

pub(super) fn thread_create(entry: *mut c_void, arg1: u64, arg2: u64, kstack_order: c_int)-> *mut thread  {
    result_to_errptr(thread_create_inner(entry, arg1, arg2, kstack_order))
}

extern "C" fn kthread_entry(prev: *mut context) {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        kassert!(!prev.is_null(), "kthread_entry: prev context is NULL");
        let cur = current_thread();
        context_switch_finish(thread_from_context(prev), cur, 0);
        CpuLocalRef::assume(cpu_local_ptr()).set_noff(0);
        intr_on();
        // `rcu_check_callbacks` (kernel/lock/rcu.rs) is a plain safe fn
        // as of P3-D3b; sound to call here because we're on the boot
        // path of a freshly-scheduled kernel thread right after
        // `context_switch_finish`, with interrupts just turned on above
        // and no RCU read-side critical section held — matching every
        // other call site of this function.
        rcu_check_callbacks();

        let cur = current_thread();
        let cur_ref = ThreadAccess::assume(cur);
        let entry_fn: extern "C" fn(u64, u64) -> c_int = core::mem::transmute(cur_ref.kentry() as *const ());
        let ret = entry_fn(cur_ref.arg(0), cur_ref.arg(1));
        exit(ret);
    }
}

fn kthread_create_inner(name: *const c_char, entry: *mut c_void,
                                         arg1: u64, arg2: u64, stack_order: c_int) -> KResult<*mut thread> {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let initproc = __proctab_get_initproc();
        kassert!(!initproc.is_null(), "kthread_create: initproc is NULL");

        if __alloc_pid() < 0 {
            return Err(Errno::Again);
        }

        let p = thread_create(entry, arg1, arg2, stack_order);
        if is_err_or_null(p) {
            __free_pid();
            return Err(if is_err(p) { Errno::Raw(ptr_err(p)) } else { Errno::NoMem });
        }

        let mut fs_clone: *mut fs_struct = ptr::null_mut();
        if !(*initproc).fs.is_null() {
            fs_clone = vfs_struct_clone((*initproc).fs, 0);
            if is_err_or_null(fs_clone) {
                __free_pid();
                thread_destroy(p);
                return Err(if is_err(fs_clone) { Errno::Raw(ptr_err(fs_clone)) } else { Errno::NoMem });
            }
        }

        let se = (*p).sched_entity;
        (*se).context.ra = kthread_entry as usize as u64;
        (*p).kentry = entry as u64;
        (*p).arg[0] = arg1;
        (*p).arg[1] = arg2;
        (*p).fs = fs_clone;
        let name_use = if name.is_null() { c"kthread".as_ptr() } else { name };
        safestrcpy((*p).name.as_mut_ptr(), name_use, (*p).name.len());
        thread_state_set(p, THREAD_UNINTERRUPTIBLE);

        pid_wlock();
        attach_child(initproc, p);
        proctab_proc_add(p);
        kthread_join_hierarchy_locked(p);
        pid_wunlock();

        Ok(p)
    }
}

pub(crate) fn kthread_create(name: *const c_char, entry: *mut c_void,
                                         arg1: u64, arg2: u64, stack_order: c_int)-> *mut thread  {
    result_to_errptr(kthread_create_inner(name, entry, arg1, arg2, stack_order))
}

pub(crate) fn idle_thread_init() {
    // The guard check and stack-pointer arithmetic below are plain safe
    // calls/bit ops (`read_sp`/`cpuid`/etc. are all safe `pub fn`s); the
    // rest of the body is a `thread_raw_layout! { ... }` invocation,
    // which already supplies its own `unsafe` block — so the outer wrap
    // has been removed as redundant rather than genuinely needed.
    let kstack_size = KERNEL_STACK_SIZE;
    let kstack = (read_sp() & !(kstack_size - 1)) as *mut c_void;
    kassert!((PAGE_SIZE << KERNEL_STACK_ORDER) == kstack_size, "idle_thread_init: invalid KERNEL_STACK_ORDER");
    thread_raw_layout! {
        let p = kstack_arrange(kstack, kstack_size, 0);
        kassert!(!p.is_null(), "idle_thread_init: failed to arrange kstack");

        (*p).kstack_order = KERNEL_STACK_ORDER;
        (*p).kstack = kstack as u64;
        strncpy((*p).name.as_mut_ptr(), c"idle".as_ptr(), (*p).name.len());
        thread_state_set(p, THREAD_RUNNING);
        let cpu = CpuLocalRef::assume(cpu_local_ptr());
        cpu.set_proc(p);
        cpu.set_idle_thread(p);

        rq_cpu_activate(cpuid());

        let mut attr: sched_attr = core::mem::zeroed();
        sched_attr_init(&mut attr);
        attr.priority = IDLE_PRIORITY;
        attr.affinity_mask = 1u64 << cpuid();
        sched_setattr((*p).sched_entity, &attr);

        rq_lock_current();
        let idle_rq = get_rq_for_cpu(IDLE_MAJOR_PRIORITY, cpuid());
        // SAFETY: `idle_rq` is the current CPU's live idle run queue (valid
        // cpu/priority indices); the former free `rq_enqueue_task` likewise
        // `assume`d it non-null.
        RqRef::assume(idle_rq).enqueue((*p).sched_entity);
        rq_unlock_current();
        // smp_store_release on on_cpu
        let se = (*p).sched_entity;
        core::sync::atomic::fence(Ordering::Release);
        core::ptr::write_volatile(&mut (*se).on_cpu as *mut c_int, 1);

        crate::kprintln!("CPU {} idle process initialized at kstack 0x{:x}", cpuid() as u64, kstack as u64);
    }
}

extern "C" fn thread_destroy_rcu_callback(data: *mut c_void) {
    // `.kstack_addr()`/`.kstack_order()` and `page_free` (declared `safe
    // fn` in the `unsafe extern "C"` block above) are safe calls; the one
    // `unsafe { }` block below (`ThreadAccess::assume`) is justified
    // in-line.
    // SAFETY: `thread_destroy_rcu_callback` is only ever invoked by the RCU subsystem
    // with the `data` pointer registered for this specific thread's deferred
    // destruction, so `data` is always a live `*mut thread` being destroyed,
    // never null.
    let t = unsafe { ThreadAccess::assume(data as *mut thread) };
    page_free(t.kstack_addr() as *mut c_void, t.kstack_order());
}

pub(super) fn thread_destroy(p: *mut thread) {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        kassert!(!p.is_null(), "thread_destroy called with NULL thread");
        kassert!(!ta_of(p).is_some_and(|t| t.is_awoken()), "thread_destroy called with a runnable thread");
        kassert!(!ta_of(p).is_some_and(|t| t.is_sleeping()), "thread_destroy called with a sleeping thread");
        kassert!((*p).kstack_order >= 0 && (*p).kstack_order <= PAGE_BUDDY_MAX_ORDER,
                 "thread_destroy: invalid kstack_order");

        let ta_d = crate::proc::access::ThreadAccess::from_raw(p).unwrap_unchecked();
        if !ta_d.sigacts_ptr().is_null() {
            sigacts_put(ta_d.sigacts_ptr());
            ta_d.set_sigacts(ptr::null_mut());
        }
        if !ta_d.vm_ptr().is_null() {
            vm_put(ta_d.vm_ptr());
            ta_d.set_vm(ptr::null_mut());
        }
        if !ta_d.fdtable_ptr().is_null() {
            vfs_fdtable_put(ta_d.fdtable_ptr());
            ta_d.set_fdtable(ptr::null_mut());
        }
        if !(*p).fs.is_null() {
            vfs_struct_put((*p).fs);
            (*p).fs = ptr::null_mut();
        }

        sigpending_empty(p, 0);
        sigpending_destroy(p);

        let ta = crate::proc::access::ThreadAccess::from_raw(p).unwrap_unchecked();
        if !ta.thread_group_ptr().is_null() {
            thread_group_put(ta.thread_group_ptr());
            ta.set_thread_group(ptr::null_mut());
        }

        call_rcu(&mut (*p).rcu_head, Some(thread_destroy_rcu_callback), p as *mut c_void);
    }
}

extern "C" fn init_entry(prev: *mut context) {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        let cur = current_thread();
        context_switch_finish(thread_from_context(prev), cur, 0);
        CpuLocalRef::assume(cpu_local_ptr()).set_noff(0);
        intr_on();

        start_kernel_post_init();

        let init_path = c"/bin/init".as_ptr() as *mut c_char;
        let argv0 = c"init".as_ptr() as *mut c_char;
        let mut argv: [*mut c_char; 2] = [argv0, ptr::null_mut()];
        let ret = exec(init_path, argv.as_mut_ptr(), ptr::null_mut());
        kassert!(ret >= 0, "init_entry: exec /bin/init failed");

        core::sync::atomic::fence(Ordering::SeqCst);
        usertrapret();
    }
}

// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn userinit() {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        kassert!(__alloc_pid() == 0, "userinit: __alloc_pid failed");

        let p = thread_create(init_entry as *mut c_void, 0, 0, KERNEL_STACK_ORDER);
        kassert!(!is_err_or_null(p), "userinit: thread_create failed");

        pid_wlock();
        proctab_proc_add(p);
        thread_group_init(p);
        // SAFETY: `p` is a live `*mut thread` (just created by
        // `thread_create` above); `crate::proc::pgroup::Thread` is an
        // opaque `#[repr(C)] struct { _p: [u8; 0] }` marker type standing
        // in for the exact same underlying object -- reinterpreting the
        // pointer is sound (same precedent as `sysproc.rs`'s `thread_clone`
        // wrapper, P3-1B mesh sweep).
        crate::proc::pgroup_init(p as *mut c_void as *mut crate::proc::pgroup::Thread);
        session_init(p);
        pid_wunlock();

        crate::kprintln!("Init process kernel stack size order: {}", (*p).kstack_order);

        let vm = vm_init();
        kassert!(!is_err_or_null(vm), "userinit: vm_init failed");
        let ta_u = crate::proc::access::ThreadAccess::from_raw(p).unwrap_unchecked();
        ta_u.set_vm(vm);

        __proctab_set_initproc(p);

        ta_u.set_sigacts(sigacts_init());
        kassert!(!ta_u.sigacts_ptr().is_null(), "userinit: sigacts_init failed");

        safestrcpy((*p).name.as_mut_ptr(), c"initcode".as_ptr(), (*p).name.len());

        ta_u.set_user_space();

        let mut attr: sched_attr = core::mem::zeroed();
        sched_attr_init(&mut attr);
        sched_setattr((*p).sched_entity, &attr);

        thread_state_set(p, THREAD_UNINTERRUPTIBLE);
        scheduler_wakeup(p);
    }
}

// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn install_user_root() {
    // See `thread_raw_layout!`'s doc comment: the body below is entirely
    // the macro invocation, which already supplies its own `unsafe`
    // block, so the outer wrap has been removed as redundant.
    thread_raw_layout! {
        let p = current_thread();

        let root_inode = vfs_namei(c"/".as_ptr(), 1);
        if root_inode.is_null() {
            kpanic!("install_user_root: cannot find root directory");
        }

        kassert!(!(*p).fs.is_null(), "install_user_root: thread fs_struct is NULL");

        tcb_lock(p);
        if let Some(t) = ta_of(p) { t.set_user_space(); }
        tcb_unlock(p);

        install_user_root_finish(p, root_inode);
    }
}

/// Native port of the (deleted) C `install_user_root_finish` bridge
/// helper: takes a ref on `root_inode`, installs it as the thread's
/// `fs->cwd` under the fs_struct spinlock (replicating the `static
/// inline vfs_struct_lock`/`vfs_struct_unlock` bodies from
/// kernel/inc/vfs/fs.h via `KSpinlock`), then drops the caller's
/// original `vfs_namei` reference.
fn install_user_root_finish(p: *mut thread, root_inode: *mut c_void) {
    // SAFETY: `p` is the current thread (`current_thread()` in
    // `install_user_root`, always a live pointer for the running
    // thread) whose `fs` field was just null-checked by the caller
    // right before this call, so `(*p).fs` and the subsequent
    // `(*fs).lock`/`(*fs).cwd` accesses are on a valid, non-null
    // `fs_struct`. `root_inode` is a just-returned, non-null
    // `vfs_namei()` reference (also null-checked by the caller) that
    // this function fully consumes: `vfs_inode_get_ref` takes a
    // reference into `cwd_ref` (written back through the `MaybeUninit`
    // out-pointer, then read via `assume_init` only after `ret >= 0`
    // confirms it was initialized), and `vfs_iput` drops the caller's
    // original reference once the new one has been installed. The
    // `KSpinlock` over `fs->lock` mirrors the deleted C
    // `vfs_struct_lock`/`vfs_struct_unlock` static-inline pair, so
    // `(*fs).cwd` is written while that lock is held, as the C original
    // required.
    u! {
    let root_inode = root_inode as *mut vfs_inode;
    let mut cwd_ref: core::mem::MaybeUninit<vfs_inode_ref> = core::mem::MaybeUninit::uninit();
    let ret = vfs_inode_get_ref(root_inode, cwd_ref.as_mut_ptr());
    if ret < 0 {
        kpanic!("install_user_root: failed to get ref to root inode");
    }
    let fs = (*p).fs;
    {
        let _g = crate::sync::KSpinlock::from_bindings(&raw mut (*fs).lock).lock();
        (*fs).cwd = cwd_ref.assume_init();
    }
    vfs_iput(root_inode);
    }
}

// The `xv6_thport_*` C-ABI alias layer that used to front tcb_lock/
// tcb_unlock/proc_assert_holding/thread_init/attach_child/detach_child/
// thread_create/kthread_create/idle_thread_init/thread_destroy/userinit/
// install_user_root was collapsed in the P3-1B2 sweep: every caller now
