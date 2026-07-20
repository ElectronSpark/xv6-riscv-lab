//! Pure-Rust port of `kernel/proc/thread_group.c`.
//!
//! Owns the canonical thread-group entry points `thread_group_init`,
//! `thread_group_put`, `tg_shared_pending_init/destroy`,
//! `thread_group_alloc[/_kernel]`, `thread_group_add/remove`,
//! `thread_is_group_leader`, `thread_tgid`, `thread_group_exit`,
//! `tg_signal_send`, `tg_dequeue_signal`, `tg_sigpending_empty`.
//! The `xv6_tgport_*` C-ABI alias layer that used to front these for
//! sibling Rust/C ports was collapsed in the P3-1B2 sweep; the
//! `#[no_mangle]` C-ABI exports themselves were dismantled in P3-D2b
//! (callers are plain crate-path Rust calls now), which also deleted the
//! caller-less `thread_group_get`, `thread_group_live_dec`,
//! `get_thread_group`, `tg_signal_pending`, `tg_recalc_sigpending`.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

// See `crate::u`'s doc comment (kernel/lib.rs) for the macro's contract.
use crate::u;

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::Ordering;

use crate::bindings::{
    ksiginfo as ksiginfo_t, list_node_t, pid_t, sigacts as sigacts_t,
    sigpending as sigpending_t, slab_cache_t, thread,
    thread_group, thread_state,
};

// NO-STANDALONE-FN: the thin `slab_cache_init`/`slab_alloc`/`slab_free` cast
// adapters (each a single-expression forward to the real `crate::mm::slab`
// `unsafe fn`, reinterpreting this file's bindgen `slab_cache_t` as the
// layout-identical `crate::mm::slab::SlabCache`) had 1-2 call sites apiece and
// have been inlined at those sites (`ThreadGroup::{init,alloc,alloc_kernel}` and
// `ThreadGroupAccess::put`) and deleted. `crate::mm::slab_*` are `unsafe fn`,
// so every inlined site keeps its existing `unsafe`/`u!{}` context.
use crate::machine::{cpuid, CpuLocal};
use crate::proc::access::{
    KsigInfoAccess, SchedEntityRef, SigPendingRef, ThreadAccess, ThreadGroupAccess,
    ThreadSignalAccess, is_err, list_container_of as container_of,
    list_node_detach_raw, list_node_init_raw, list_node_is_detached_raw, list_node_is_empty_raw,
    list_node_next_raw, list_node_push_back_raw,
};
use crate::proc::proc_shims::{xv6_panic, xv6_pid_rlock, xv6_pid_runlock, xv6_pid_wholding};
use crate::proc::ksiginfo_alloc;
use crate::proc::ksiginfo_free;
use crate::proc::sigacts_lock;
use crate::proc::sigacts_unlock;
// P3-1B2: previously bridged via the `xv6_schport_*` C-ABI alias layer
// (`extern "C"` redeclarations above); now direct crate-path calls to the
// real, already-Rust definitions in `sched.rs`.
use crate::proc::{scheduler_wakeup_interruptible, scheduler_wakeup_stopped};
use crate::ipi::ipi_send_single;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N3.
//
// `ThreadGroup` IS the kernel-wide Rust definition of
// `kernel/inc/proc/thread_group_types.h`'s `struct thread_group` now:
// `build.rs` blocklists the bindgen-generated form and injects
// `pub use crate::proc::thread_group::ThreadGroup as thread_group;`, so
// every `crate::bindings::thread_group` path across the crate resolves
// here. `shared_pending` embeds the still-bindgen
// `crate::bindings::tg_shared_pending` *by value* — the sanctioned
// mixed-tier pattern (P3-N2): when the signal family later nativizes,
// the alias re-points transparently. The C `_Atomic int` members
// (`live_threads`/`refcount`/`group_exit`) stay plain `c_int` fields,
// exactly as bindgen lowered them; all atomic access goes through
// `access.rs`'s `AtomicI32` pointer views, unchanged.
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 is_kernel : 1; }` inside `struct thread_group`
/// (bindgen's `thread_group__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `is_kernel` at bit 0 of the
/// (8-byte) unit's byte 0 — identical to bindgen's `get(0,1)` accessor
/// reproduced below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct ThreadGroupFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl ThreadGroupFlagBits {
    #[inline]
    pub(crate) fn is_kernel(&self) -> u64 {
        (self.bits & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_is_kernel(&mut self, val: u64) {
        self.bits = (self.bits & !0b01) | ((val as u8) & 0b01);
    }
}

const _: () = {
    assert!(core::mem::size_of::<ThreadGroupFlagBits>() == 8, "thread_group anon bitfield size");
    assert!(core::mem::align_of::<ThreadGroupFlagBits>() == 8, "thread_group anon bitfield alignment");
};

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ThreadGroup {
    pub(crate) list_entry: crate::list::ListNode,
    pub(crate) pgroup: *mut crate::bindings::pgroup,
    pub(crate) tgid: pid_t,
    pub(crate) group_leader: *mut thread,
    pub(crate) thread_list: crate::list::ListNode,
    pub(crate) live_threads: c_int,
    pub(crate) refcount: c_int,
    pub(crate) shared_pending: crate::bindings::tg_shared_pending,
    pub(crate) group_exit: c_int,
    pub(crate) group_exit_code: c_int,
    pub(crate) group_exit_task: *mut thread,
    pub(crate) group_stop_count: c_int,
    pub(crate) group_stop_signo: c_int,
    pub(crate) flags: ThreadGroupFlagBits,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// thread_group { list_entry: list_node_t, pgroup: *mut pgroup, tgid:
// pid_t, group_leader: *mut thread, thread_list: list_node_t,
// live_threads: c_int, refcount: c_int, shared_pending:
// tg_shared_pending, group_exit: c_int, group_exit_code: c_int,
// group_exit_task: *mut thread, group_stop_count: c_int,
// group_stop_signo: c_int, __bindgen_anon_1:
// thread_group__bindgen_ty_1 }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe (rv64gc/lp64d)
// against `kernel/inc/proc/thread_group_types.h`: size 616, align 8,
// offsets 0/16/24/32/40/56/60/64/584/588/592/600/604/[608]
// (tg_shared_pending itself proven 520/8 by the same probe; the
// anonymous bitfield struct occupies [608,616), pinned in the probe by
// its predecessor plus the struct size).
const _: () = {
    assert!(core::mem::size_of::<crate::bindings::tg_shared_pending>() == 520,
        "tg_shared_pending size");
    assert!(core::mem::align_of::<crate::bindings::tg_shared_pending>() == 8,
        "tg_shared_pending alignment");
    assert!(core::mem::size_of::<ThreadGroup>() == 616, "thread_group size");
    assert!(core::mem::align_of::<ThreadGroup>() == 8, "thread_group alignment");
    assert!(core::mem::offset_of!(ThreadGroup, list_entry) == 0, "thread_group.list_entry offset");
    assert!(core::mem::offset_of!(ThreadGroup, pgroup) == 16, "thread_group.pgroup offset");
    assert!(core::mem::offset_of!(ThreadGroup, tgid) == 24, "thread_group.tgid offset");
    assert!(core::mem::offset_of!(ThreadGroup, group_leader) == 32, "thread_group.group_leader offset");
    assert!(core::mem::offset_of!(ThreadGroup, thread_list) == 40, "thread_group.thread_list offset");
    assert!(core::mem::offset_of!(ThreadGroup, live_threads) == 56, "thread_group.live_threads offset");
    assert!(core::mem::offset_of!(ThreadGroup, refcount) == 60, "thread_group.refcount offset");
    assert!(core::mem::offset_of!(ThreadGroup, shared_pending) == 64, "thread_group.shared_pending offset");
    assert!(core::mem::offset_of!(ThreadGroup, group_exit) == 584, "thread_group.group_exit offset");
    assert!(core::mem::offset_of!(ThreadGroup, group_exit_code) == 588, "thread_group.group_exit_code offset");
    assert!(core::mem::offset_of!(ThreadGroup, group_exit_task) == 592, "thread_group.group_exit_task offset");
    assert!(core::mem::offset_of!(ThreadGroup, group_stop_count) == 600, "thread_group.group_stop_count offset");
    assert!(core::mem::offset_of!(ThreadGroup, group_stop_signo) == 604, "thread_group.group_stop_signo offset");
    assert!(core::mem::offset_of!(ThreadGroup, flags) == 608, "thread_group anon bitfield offset");
};

// ---------------- constants ----------------------------------------------
const SLAB_FLAG_STATIC: u64 = 1 << 1;
const TG_MAX_SIGINFO_PER_SIGNAL: c_int = 8;
const NSIG: usize = 32;
const SIGKILL: c_int = 9;
const SA_SIGINFO: c_int = 0x00000004;
const CPU_FLAG_NEEDS_RESCHED: u64 = 1;
const IPI_REASON_RESCHEDULE: c_int = 2;

const EINVAL: c_int = 22;
const ENOMEM: c_int = 12;
const EPERM: c_int = 1;
const ESRCH: c_int = 3;

const THREAD_FLAG_KILLED: u64 = 2;
const THREAD_FLAG_SIGPENDING: u64 = 4;

const THREAD_UNUSED: thread_state = 0;
const THREAD_INTERRUPTIBLE: thread_state = 2;
const THREAD_KIILABLE: thread_state = 3;
const THREAD_TIMER: thread_state = 4;
const THREAD_KIILABLE_TIMER: thread_state = 5;
const THREAD_UNINTERRUPTIBLE: thread_state = 6;
const THREAD_RUNNING: thread_state = 8;
const THREAD_STOPPED: thread_state = 9;
const THREAD_ZOMBIE: thread_state = 11;

// P3-D2b: `get_pid_thread` (proc/pid.rs) and `pgroup_remove_tg`
// (proc/pgroup.rs) are plain crate-path items now that their
// `#[no_mangle]` exports are gone (identical signatures) -- they used
// to be `extern "C"` redeclarations in the block below.
use crate::proc::{get_pid_thread, pgroup_remove_tg};

// P3-D3c: `proc/exit.rs`'s `exit` is a plain (safe) Rust fn now that its
// `#[no_mangle]` export is gone -- imported via its private sibling module
// path.
use super::exit::exit;
// ---------------- extern C primitives -----------------------------------
unsafe extern "C" {
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // signal helpers

    // cpu id (trampoline from slab_shims.rs)
}

// Field offsets for `container_of` / list iteration — compile-time constants
// (NO-STANDALONE-FN: the former `tg_entry_offset_in_thread` /
// `list_entry_offset_in_ksiginfo` free fns are now `const`s; `offset_of!` is
// const-stable, so the two iterator methods / `container_of` sites read them
// directly).
const TG_ENTRY_OFF: usize = core::mem::offset_of!(thread, tg_entry);
const KSI_LIST_ENTRY_OFF: usize = core::mem::offset_of!(ksiginfo_t, list_entry);

// NO-STANDALONE-FN: the module-private `ta_of` alias for
// `ThreadAccess::from_ptr`, the thread-pointer flag setters
// (`thread_set_killed`/`thread_set_sigpending`), the state predicates
// (`thread_state_load`/`THREAD_SLEEPING`/`THREAD_STOPPED_p`/`THREAD_RUNNING_p`/
// `THREAD_INTERRUPTIBLE_p`/`THREAD_ZOMBIE_p`), and the thread->tg navigators
// (`thread_group_remove`/`thread_is_group_leader`/`thread_tgid`/
// `thread_group_exit`) are now inherent methods on the `ThreadAccess` handle
// (the `impl<'a> ThreadAccess<'a>` block below) — each reads/writes through the
// handle's own pointer, never forming a `&thread`, keeping the same
// no-Freeze-hazard profile as every other accessor. The former free fns'
// leading `p.is_null()` guards move to the call sites, which construct the
// handle via `from_ptr`. Dead helpers (`thread_clear_sigpending`,
// `thread_is_zombie_state`, `sigaddset`, `sigdelset`) had no callers and are
// deleted.

// sigset helpers (GENUINE FLOOR): `sigbad`/`sigismember` operate on a bare
// `sigset_t = u64` (a foreign scalar with no sound domain type to host them as
// methods), so per the wave rule they stay free.
type sigset_t = u64;
#[inline]
fn sigbad(signo: c_int) -> bool { signo < 1 || signo > NSIG as c_int }
#[inline]
fn sigismember(set: sigset_t, signo: c_int) -> bool {
    if sigbad(signo) { return false; }
    (set & (1u64 << (signo - 1))) != 0
}

// NO-STANDALONE-FN: derived thread-pointer/state operations and the thread->tg
// navigators as inherent methods on the `ThreadAccess` handle (extended
// in-crate, same precedent as `thread.rs`). Kept module-private except where an
// out-of-`crate::proc` caller forces a wider visibility (`resolve_tgid` reaches
// `tty::session`).
impl<'a> ThreadAccess<'a> {
    /// Mark this thread killed (former free fn `thread_set_killed`).
    #[inline]
    fn set_killed(&self) { self.flags_set_bit(THREAD_FLAG_KILLED); }
    /// Flag a pending signal on this thread (former `thread_set_sigpending`).
    #[inline]
    fn set_sigpending(&self) { self.flags_set_bit(THREAD_FLAG_SIGPENDING); }
    /// This thread's scheduler state (former `thread_state_load`, non-null self).
    #[inline]
    fn state_of(&self) -> thread_state { self.state_load() as thread_state }
    // The former `THREAD_SLEEPING` predicate is the `ThreadAccess::is_sleeping`
    // method defined (identically) in `thread.rs` and reused here.
    /// Former `THREAD_STOPPED_p` predicate (self non-null).
    #[inline]
    fn is_stopped(&self) -> bool { self.state_of() == THREAD_STOPPED }
    /// Former `THREAD_RUNNING_p` predicate (self non-null).
    #[inline]
    fn is_running(&self) -> bool { self.state_of() == THREAD_RUNNING }
    /// Former `THREAD_INTERRUPTIBLE_p` predicate (self non-null).
    #[inline]
    fn is_interruptible(&self) -> bool { self.state_of() == THREAD_INTERRUPTIBLE }
    /// Former `THREAD_ZOMBIE_p` predicate (self non-null).
    #[inline]
    fn is_zombie_state(&self) -> bool { self.state_of() == THREAD_ZOMBIE }

    /// Detach this thread from its group; return `true` if it was the last live
    /// member (former free fn `thread_group_remove`; caller must hold pid_wlock).
    /// The former `p.is_null()` early-`true` guard now lives at the call site.
    pub(super) fn group_remove(&self) -> bool {
        let tg = self.thread_group_ptr();
        if tg.is_null() { return true; }
        if xv6_pid_wholding() == 0 {
            xv6_panic(c"thread_group_remove: caller must hold pid_wlock".as_ptr());
        }
        let mut last = false;
        if !self.tg_entry_ref().is_detached() {
            self.tg_entry_ref().detach();
        }
        // atomic_sub returns the *previous* value
        // SAFETY: `tg` is checked non-null immediately above.
        let prev = unsafe { ThreadGroupAccess::assume(tg) }.live_threads_fetch_sub(1);
        if prev <= 1 { last = true; }
        if last {
            pgroup_remove_tg(tg);
        }
        last
    }

    /// Whether this thread is its group's leader (former free fn
    /// `thread_is_group_leader`); a thread with no group is trivially its own
    /// leader. The former `p.is_null()` early-`true` guard is at the call site.
    pub(super) fn is_group_leader(&self) -> bool {
        let tg = self.thread_group_ptr();
        if tg.is_null() { return true; }
        // SAFETY: `tg` is checked non-null immediately above.
        unsafe { ThreadGroupAccess::assume(tg) }.group_leader_ptr() == self.as_ptr()
    }

    /// Resolve this thread's thread-group id (former free fn `thread_tgid`);
    /// falls back to the thread's own pid when it has no group or an unset tgid.
    /// The former `p.is_null()` early-`-1` guard is at the call site.
    pub(crate) fn resolve_tgid(&self) -> c_int {
        let tg = self.thread_group_ptr();
        if tg.is_null() { return self.pid(); }
        // SAFETY: `tg` is checked non-null immediately above.
        let tga = unsafe { ThreadGroupAccess::assume(tg) };
        let tgid = tga.tgid();
        if tgid > 0 { tgid } else { self.pid() }
    }

    /// Initiate a group-wide exit with `code` (former free fn
    /// `thread_group_exit`): CAS this thread as the exit task, broadcast SIGKILL
    /// to the siblings, then `exit`. The former `p.is_null()` early-return guard
    /// is at the call site.
    pub(super) fn group_exit(&self, code: c_int) {
        let tg = self.thread_group_ptr();
        if tg.is_null() { exit(code); }
        // SAFETY: `tg` is checked non-null immediately above.
        let tga = unsafe { ThreadGroupAccess::assume(tg) };
        // CAS group_exit_task from NULL to self; only the first wins.
        let cas_ok = tga.group_exit_task_compare_exchange_null(self.as_ptr());
        if !cas_ok {
            exit(code);
        }
        tga.set_group_exit_code(code);
        tga.set_group_exit_task(self.as_ptr());
        // Broadcast SIGKILL to siblings (skip self).
        tga.sigkill_all(self.as_ptr());
        exit(code);
    }
}

// ---------------- list iteration as handle methods ----------------------
// NO-STANDALONE-FN: the `tg_threads` / `siginfo_queue` iterator builders and
// the `siginfo_queue_len` counter that used to be free fns are now inherent
// methods on the handles that own the underlying list head — `tg_threads` ->
// `ThreadGroupAccess::threads` (all callers passed `self.thread_list_ptr()`),
// `siginfo_queue` -> `SigPendingRef::entries` (all callers derived the head
// from a `SigPendingRef`). `siginfo_queue_len` had a single call site and is
// inlined there as `SigPendingRef::assume(sq).queue_len()`.
impl<'a> ThreadGroupAccess<'a> {
    /// A lazy [`Iterator`] over this group's `thread_list` (entries are
    /// `tg_entry` in struct thread), yielding `*mut thread`. Drive with
    /// `for t in self.threads() { … }`. (Former free fn `tg_threads`.)
    ///
    /// Safe under concurrent removal of the *current* node (Linux
    /// `list_foreach_node_safe` semantics: [`crate::list::ListIterator::next`]
    /// caches the next pointer before yielding the current one, so the loop body
    /// may detach `t` itself). An empty list yields nothing.
    #[inline]
    fn threads(&self) -> crate::list::ListIterator<'a, thread> {
        // SAFETY: `self` wraps a live `thread_group` held under `pid_lock`;
        // `thread_list_ptr()` is its real `thread_list` sentinel head and
        // `TG_ENTRY_OFF` is `thread::tg_entry`'s real member offset, upholding
        // `ListIterator::new`'s contract (a null head yields nothing).
        unsafe { crate::list::ListIterator::new(self.thread_list_ptr(), TG_ENTRY_OFF) }
    }
}

impl<'a> SigPendingRef<'a> {
    /// A lazy [`Iterator`] over this sigpending queue (embedded `ksiginfo`
    /// entries linked via `list_entry`), yielding `*mut ksiginfo_t`. (Former
    /// free fn `siginfo_queue`.)
    ///
    /// A `list_foreach_node_safe`-equivalent walk: [`crate::list::ListIterator::next`]
    /// caches each node's successor *before* yielding it, so a loop body may
    /// `detach` **and free** the yielded entry (the drain pattern used by
    /// `shared_pending_destroy` / `sigpending_empty_sig`). An empty queue yields
    /// nothing. Every touch is a raw pointer load through the iterator's cursor
    /// — no `&ksiginfo` reference is formed.
    #[inline]
    fn entries(&self) -> crate::list::ListIterator<'a, ksiginfo_t> {
        // SAFETY: `self` wraps a live `sig_pending` queue (a `list_node_t`
        // embedded by value in an already-valid `tg_shared_pending`);
        // `queue_ptr()` is its real sentinel head and `KSI_LIST_ENTRY_OFF` is
        // `ksiginfo::list_entry`'s real member offset, upholding
        // `ListIterator::new`'s contract (a null head yields nothing).
        unsafe { crate::list::ListIterator::new(self.queue_ptr(), KSI_LIST_ENTRY_OFF) }
    }
}

// ---------------- slab cache storage ------------------------------------
#[repr(transparent)]
struct CacheCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: `TG_POOL` is written in full by `slab_cache_init` (called
// once from `ThreadGroup::init` below, before any `slab_alloc` on this
// cache) and otherwise only accessed through the C slab allocator's
// own internally-synchronized primitives (`slab_alloc`/`slab_free`),
// which serialize concurrent access via the cache's embedded locks.
unsafe impl Sync for CacheCell {}
static TG_POOL: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));

// ===========================================================================
// SECTION 15.1/15.4  subsystem init + thread group lifecycle
// ===========================================================================
// NO-STANDALONE-FN: the slab-pool accessor (`tg_pool`) and the constructors
// (`thread_group_init`/`thread_group_alloc`/`thread_group_alloc_kernel`) are now
// associated functions on `ThreadGroup` itself (namespaced under the type, no
// receiver). The thin `slab_cache_init`/`slab_alloc`/`slab_free` FFI adapters
// they used are inlined here as direct `crate::mm::slab_*` calls.
impl ThreadGroup {
    /// The `'static` thread_group slab cache (former free fn `tg_pool`).
    #[inline]
    fn pool() -> *mut slab_cache_t { TG_POOL.0.get() as *mut slab_cache_t }

    /// Subsystem init: create the slab cache and allocate `initproc`'s group.
    /// (Former free fn `thread_group_init`.)
    pub(super) fn init(initproc: *mut thread) {
        // SAFETY: `crate::mm::slab_cache_init` is an `unsafe fn`. `Self::pool()`
        // points into the static `TG_POOL` cell, valid for `'static` and not
        // yet visible to any other thread this early in boot, so passing it
        // (with a `'static` name pointer and the correct
        // `size_of::<thread_group>()`) is sound.
        u! {
            crate::mm::slab_cache_init(
                Self::pool() as *mut crate::mm::slab::SlabCache,
                c"thread_group".as_ptr() as *mut c_char,
                core::mem::size_of::<thread_group>(),
                SLAB_FLAG_STATIC as _,
            );
            let ret = Self::alloc(initproc);
            if ret != 0 {
                xv6_panic(c"thread_group_init: thread_group_alloc failed".as_ptr());
            }
        }
    }

    /// Allocate a user thread_group with `leader` as its group leader.
    /// (Former free fn `thread_group_alloc`.)
    pub(super) fn alloc(leader: *mut thread) -> c_int {
        if leader.is_null() {
            xv6_panic(c"thread_group_alloc: NULL leader".as_ptr());
        }
        // SAFETY: `leader` is checked non-null above (panics otherwise).
        // `crate::mm::slab_alloc`/`memset` are `unsafe fn`; `tg` is null-checked
        // immediately after allocation, and `memset` writes exactly
        // `size_of::<thread_group>()` zeroed bytes into it before the subsequent
        // `ThreadGroupAccess::assume(tg)` operates on a valid, freshly
        // zero-initialized `thread_group`.
        u! {
            let tg = crate::mm::slab_alloc(Self::pool() as *mut crate::mm::slab::SlabCache)
                as *mut thread_group;
            if tg.is_null() { return -ENOMEM; }
            memset(tg as *mut c_void, 0, core::mem::size_of::<thread_group>());
            let lta = ThreadAccess::assume(leader);
            let tga = ThreadGroupAccess::assume(tg);
            tga.thread_list_ref().init();
            tga.list_entry_ref().init();
            tga.set_group_leader(leader);
            tga.set_tgid(lta.pid());
            tga.live_threads_store(1);
            tga.refcount_store(1);
            tga.group_exit_store(0);
            tga.set_group_exit_code(0);
            tga.set_group_exit_task(ptr::null_mut());
            tga.set_group_stop_count(0);
            tga.set_group_stop_signo(0);
            tga.set_pgroup(ptr::null_mut());
            tga.set_is_kernel(0);

            tga.shared_pending_init();

            // Link leader into the group
            lta.set_thread_group(tg);
            lta.set_tgid(lta.pid());
            lta.tg_entry_ref().init();
            lta.tg_entry_ref().push_back(tga.thread_list_ptr());
            0
        }
    }

    /// Allocate a leaderless kernel thread_group with id `tgid`, storing it in
    /// `*out_tg`. (Former free fn `thread_group_alloc_kernel`.)
    pub(super) fn alloc_kernel(out_tg: *mut *mut thread_group, tgid: pid_t) -> c_int {
        if out_tg.is_null() { return -EINVAL; }
        // SAFETY: `out_tg` is checked non-null above; `crate::mm::slab_alloc`/
        // `memset` are `unsafe fn`, and `tg` is null-checked before `memset`
        // zero-initializes exactly `size_of::<thread_group>()` bytes, so
        // `ThreadGroupAccess::assume(tg)` sees a valid struct and the final
        // `*out_tg = tg` writes through the caller-supplied non-null pointer.
        u! {
            let tg = crate::mm::slab_alloc(Self::pool() as *mut crate::mm::slab::SlabCache)
                as *mut thread_group;
            if tg.is_null() { return -ENOMEM; }
            memset(tg as *mut c_void, 0, core::mem::size_of::<thread_group>());
            let tga = ThreadGroupAccess::assume(tg);
            tga.thread_list_ref().init();
            tga.list_entry_ref().init();
            tga.set_group_leader(ptr::null_mut());
            tga.set_tgid(tgid);
            tga.live_threads_store(0);
            tga.refcount_store(1);
            tga.group_exit_store(0);
            tga.set_group_exit_code(0);
            tga.set_group_exit_task(ptr::null_mut());
            tga.set_group_stop_count(0);
            tga.set_group_stop_signo(0);
            tga.set_pgroup(ptr::null_mut());
            tga.set_is_kernel(1);
            tga.shared_pending_init();
            *out_tg = tg;
            0
        }
    }
}

// ===========================================================================
// SECTION 15.2/15.3  reference counting + shared pending signal helpers
// ===========================================================================
// NO-STANDALONE-FN: the `thread_group_put` delegator free fn is gone; callers
// construct a `ThreadGroupAccess` and invoke `.put()` (the null-`tg` no-op moves
// to the call site's `from_ptr`). `ThreadGroupAccess::inc_ref`/`put`/
// `shared_pending_init`/`shared_pending_destroy` remain methods on the handle
// (SECTION 15.9). (`thread_group_get`/`live_dec`/`get_thread_group` were deleted
// earlier as caller-less; the P3-D2b notes have been folded away here.)

// ===========================================================================
// SECTION 15.4/15.5/15.6/15.8  add / remove / queries / group-exit / signals
// ===========================================================================
// NO-STANDALONE-FN: the thin `tg`-taking delegator entry points
// (`thread_group_add` + the signal trio `tg_signal_send`/`tg_dequeue_signal`/
// `tg_sigpending_empty`) are gone — every caller now constructs a
// `ThreadGroupAccess` via `from_ptr` and invokes the corresponding method
// (`add_thread`/`signal_send`/`dequeue_signal`/`sigpending_empty_sig`, SECTION
// 15.9), mapping the null-`tg` case (panic / -EINVAL / null / no-op) at the call
// site exactly as the delegators did. The `p`-taking navigators
// (`thread_group_remove`/`thread_is_group_leader`/`thread_tgid`/
// `thread_group_exit`) became `ThreadAccess` methods (`group_remove`/
// `is_group_leader`/`resolve_tgid`/`group_exit`, defined above). The
// caller-less `tg_signal_pending`/`tg_recalc_sigpending` were deleted earlier.

// ===========================================================================
// SECTION 15.9  per-thread_group algorithms as METHODS on the handle
//
// N-METH (RUSTIFY-PROC): the per-object algorithms that used to be free
// `*(tg: *mut thread_group)` fns are now inherent METHODS on the
// `ThreadGroupAccess` *handle* (defined in `crate::proc::access`; this is an
// inherent impl on that type, legal in any module of the defining crate).
//
// FREEZE + RCU discipline (see the `freeze-noalias-hazard` /
// `gentable-scope-limit` notes): `ThreadGroup` is `Copy`/`UnsafeCell`-free =>
// `Freeze`, and its `live_threads`/`refcount`/`shared_pending`/`group_*`
// fields are mutated by other harts and read across the `pid_lock`/RCU
// window. `ThreadGroupAccess` wraps a `NonNull<thread_group>` and touches
// every field through that raw pointer, so `&self` here carries **no**
// `&ThreadGroup` -- the `noalias`/`readonly` read-hoist that a `&self` on the
// native struct would license is structurally avoided. The RCU-synchronized
// `thread_list` membership walk keeps using the read-only `tg_threads`
// iterator, which yields raw `*mut thread` (never `&thread`) and never
// converts the discipline to a lock.
// ===========================================================================
impl<'a> ThreadGroupAccess<'a> {
    /// Bump the group refcount by one. (Former free fn `thread_group_get_impl`.)
    #[inline]
    fn inc_ref(&self) {
        self.refcount_fetch_add(1);
    }

    /// Drop one group reference; on the last, tear down shared-pending state
    /// and return the slab object. (Former body of `thread_group_put`.)
    pub(super) fn put(&self) {
        if self.refcount_dec_unless(1) {
            return; // refcount > 1, still alive
        }
        self.shared_pending_destroy();
        // SAFETY: `refcount_dec_unless` returning `false` means this call
        // observed (and dropped) the last reference, so no other holder can
        // still be using the group; `crate::mm::slab_free` (`unsafe fn`) may now
        // reclaim it, and no field of `self` is touched afterward.
        unsafe { crate::mm::slab_free(self.as_ptr() as *mut c_void); }
    }

    /// Initialize the shared-pending signal state to empty.
    /// (Former free fn `tg_shared_pending_init`.)
    fn shared_pending_init(&self) {
        let sp = self.shared_pending_ref();
        sp.set_sig_pending_mask(0);
        for i in 0..NSIG {
            sp.sig_pending_ref_index(i).queue_ref().init();
        }
    }

    /// Detach and free every queued `ksiginfo` across all NSIG shared-pending
    /// queues, then clear the pending mask. (Former free fn
    /// `tg_shared_pending_destroy`; its inner `while cur != head` drain is now
    /// the `siginfo_queue` iterator.)
    fn shared_pending_destroy(&self) {
        let sp = self.shared_pending_ref();
        for i in 0..NSIG {
            for ksi in sp.sig_pending_ref_index(i).entries() {
                // SAFETY: `ksi` is a `container_of` recovery from a live node
                // of the shared-pending queue, only ever populated with real
                // `ksiginfo` entries from `ksiginfo_alloc`; `SigPendingRef::entries`
                // caches each node's successor before yielding it, so
                // detach+free of the current node here is sound.
                unsafe { KsigInfoAccess::assume(ksi) }.list_entry_ref().detach();
                ksiginfo_free(ksi);
            }
        }
        sp.set_sig_pending_mask(0);
    }

    /// Link `child` into this group and bump the live/ref counts.
    /// Caller must hold pid_wlock. (Former body of `thread_group_add`; the
    /// former delegator's null-`self` panic now lives at the call site.)
    pub(super) fn add_thread(&self, child: *mut thread) {
        if child.is_null() {
            xv6_panic(c"thread_group_add: NULL child".as_ptr());
        }
        // SAFETY: `child` is proven non-null by the diverging `xv6_panic`
        // null check above.
        let cta = unsafe { ThreadAccess::assume(child) };
        if !cta.thread_group_ptr().is_null() {
            xv6_panic(c"thread_group_add: child already in a group".as_ptr());
        }
        if xv6_pid_wholding() == 0 {
            xv6_panic(c"thread_group_add: caller must hold pid_wlock".as_ptr());
        }
        cta.set_thread_group(self.as_ptr());
        cta.set_tgid(self.tgid());
        cta.tg_entry_ref().init();
        cta.tg_entry_ref().push_back(self.thread_list_ptr());
        self.inc_ref();
        if !cta.is_zombie_state() {
            self.live_threads_fetch_add(1);
        }
    }

    /// Broadcast SIGKILL to every group member except `skip`, waking sleepers
    /// and stopped threads. (Former `unsafe fn tg_sigkill_all`; the handle's
    /// liveness invariant subsumes the old `unsafe`, so this is a safe method
    /// -- the `thread_list` walk is the RCU-read `self.threads()` iterator.)
    fn sigkill_all(&self, skip: *mut thread) {
        xv6_pid_rlock();
        for t in self.threads() {
            if t == skip { continue; }
            // SAFETY: `t` is a live, non-null `*mut thread` yielded by
            // `self.threads()` (an RCU-read walk of this group's members).
            let ta = unsafe { ThreadAccess::assume(t) };
            ta.set_killed();
            ta.set_sigpending();
            if ta.is_sleeping() {
                scheduler_wakeup_interruptible(t);
            } else if ta.is_stopped() {
                scheduler_wakeup_stopped(t);
            }
        }
        xv6_pid_runlock();
    }

    /// Pick an eligible member to handle `signo` (leader first, else the first
    /// member that has not masked it). (Former `unsafe fn tg_pick_thread`.)
    fn pick_thread(&self, signo: c_int) -> *mut thread {
        let leader = self.group_leader_ptr();
        if !leader.is_null() {
            // SAFETY: `leader` is non-null here, so `from_raw` is `Some`.
            let lta = unsafe { ThreadAccess::from_raw(leader).unwrap_unchecked() };
            if !lta.sigacts_ptr().is_null() {
                // SAFETY: `leader` is a live thread (non-null, group leader).
                let mask = unsafe { ThreadSignalAccess::assume_thread(leader) }.sig_mask();
                let st = lta.state_of();
                if !sigismember(mask, signo) && st != THREAD_ZOMBIE && st != THREAD_UNUSED {
                    return leader;
                }
            }
        }
        let mut chosen: *mut thread = ptr::null_mut();
        for t in self.threads() {
            if !chosen.is_null() { continue; }
            if t == leader { continue; }
            // SAFETY: `t` is a live `*mut thread` node yielded by `self.threads()`.
            let tt = unsafe { ThreadAccess::from_raw(t).unwrap_unchecked() };
            let st = tt.state_of();
            if st == THREAD_UNUSED || st == THREAD_ZOMBIE { continue; }
            if tt.sigacts_ptr().is_null() { continue; }
            // SAFETY: `t` is a live thread (see above).
            let tmask = unsafe { ThreadSignalAccess::assume_thread(t) }.sig_mask();
            if !sigismember(tmask, signo) {
                chosen = t;
            }
        }
        if !chosen.is_null() { return chosen; }
        leader
    }

    /// Deliver `info`'s signal to the group: update shared-pending state,
    /// queue SA_SIGINFO payloads, pick a target, and wake it. Returns 0 on
    /// success or a negative errno. (Former body of `tg_signal_send`; the former
    /// delegator's null-`self` `-EINVAL` now lives at the call site.)
    pub(crate) fn signal_send(&self, info: *mut ksiginfo_t) -> c_int {
        if info.is_null() { return -EINVAL; }
        // SAFETY: `info` is checked non-null above, so `(*info).signo` is
        // valid; `sigbad` bounds `signo` to `1..=NSIG` before any array
        // indexing. `leader` and `sigacts` are each null-checked immediately
        // before the first deref that uses them (`self.group_leader_ptr()` /
        // `lta.sigacts_ptr()`), and `sigacts` stays live for the duration of
        // this block because we hold `sigacts_lock(sigacts)` across all
        // subsequent field accesses through it, released just before each
        // early return. Thread `sig_mask`/pending fields are reached through
        // the `ThreadSignalAccess` handle (raw loads, no `&thread`).
        u! {
            if sigbad((*info).signo) { return -EINVAL; }
            if self.is_kernel() != 0 { return -EPERM; }
            let signo = (*info).signo;

            let shared = self.shared_pending_ref();
            if self.live_threads_load_acquire() <= 0 {
                return -ESRCH;
            }

            if signo == SIGKILL {
                self.sigkill_all(ptr::null_mut());
                return 0;
            }

            xv6_pid_rlock();
            let leader = self.group_leader_ptr();
            if leader.is_null() {
                xv6_pid_runlock();
                return -ESRCH;
            }
            let lta = ThreadAccess::from_raw(leader).unwrap_unchecked();
            if lta.sigacts_ptr().is_null() {
                xv6_pid_runlock();
                return -ESRCH;
            }
            let sigacts = lta.sigacts_ptr();
            sigacts_lock(sigacts);

            let stop_mask = (*sigacts).sa_sigstop;
            let cont_mask = (*sigacts).sa_sigcont;

            if sigismember((*sigacts).sa_sigignore, signo) {
                sigacts_unlock(sigacts);
                xv6_pid_runlock();
                return 0;
            }

            let is_cont = sigismember(cont_mask, signo);
            let is_stop = sigismember(stop_mask, signo);
            let is_term = sigismember((*sigacts).sa_sigterm, signo);

            if is_cont {
                shared.and_sig_pending_mask(!stop_mask);
                for t in self.threads() {
                    ThreadSignalAccess::assume_thread(t).and_sig_pending_mask(!stop_mask);
                }
            }
            if is_stop {
                shared.and_sig_pending_mask(!cont_mask);
                for t in self.threads() {
                    ThreadSignalAccess::assume_thread(t).and_sig_pending_mask(!cont_mask);
                }
            }

            let act = &raw mut (*sigacts).sa[signo as usize];
            if ((*act).sa_flags & SA_SIGINFO) != 0 {
                let sq = shared.sig_pending_ptr_index((signo - 1) as usize);
                // Inlined former `siginfo_queue_len(sq)` facade (sole call site).
                let qlen = SigPendingRef::assume(sq).queue_len();
                if qlen >= TG_MAX_SIGINFO_PER_SIGNAL {
                    let head = SigPendingRef::assume(sq).queue_ptr();
                    if !list_node_is_empty_raw(head) {
                        let first = list_node_next_raw(head);
                        let old = container_of::<ksiginfo_t>(first, KSI_LIST_ENTRY_OFF);
                        if !old.is_null() {
                            KsigInfoAccess::assume(old).list_entry_ref().detach();
                            ksiginfo_free(old);
                        }
                    }
                }
                let ksi = ksiginfo_alloc();
                if !ksi.is_null() {
                    let ka = KsigInfoAccess::assume(ksi);
                    ka.copy_from(info);
                    ka.list_entry_ref().init();
                    ka.list_entry_ref().push_back(SigPendingRef::assume(sq).queue_ptr());
                }
            } else {
                if sigismember(shared.sig_pending_mask(), signo) && !is_cont {
                    sigacts_unlock(sigacts);
                    xv6_pid_runlock();
                    return 0;
                }
            }

            shared.or_sig_pending_mask(1u64 << (signo - 1));

            let mut target: *mut thread = ptr::null_mut();
            if !is_cont {
                target = self.pick_thread(signo);
                if !target.is_null() {
                    ThreadAccess::assume(target).set_sigpending();
                }
            }

            sigacts_unlock(sigacts);

            if is_cont {
                for t in self.threads() {
                    let ta = ThreadAccess::assume(t);
                    ta.set_sigpending();
                    if ta.is_stopped() {
                        scheduler_wakeup_stopped(t);
                    } else if ta.is_interruptible() {
                        scheduler_wakeup_interruptible(t);
                    }
                }
            } else if !target.is_null() {
                let tta = ThreadAccess::assume(target);
                if is_term && tta.is_stopped() {
                    scheduler_wakeup_stopped(target);
                } else if tta.is_interruptible() {
                    scheduler_wakeup_interruptible(target);
                } else if tta.is_running() {
                    let se = tta.sched_entity_ptr();
                    let target_cpu = SchedEntityRef::assume(se).cpu_id_load_acquire();
                    if target_cpu != cpuid() {
                        ipi_send_single(target_cpu, IPI_REASON_RESCHEDULE);
                    } else {
                        // Inlined former `set_needs_resched()` facade:
                        // mycpu()->flags |= CPU_FLAG_NEEDS_RESCHED.
                        CpuLocal::current().flags_or(CPU_FLAG_NEEDS_RESCHED);
                    }
                }
            }

            xv6_pid_runlock();
            0
        }
    }

    /// Dequeue the first queued `ksiginfo` for `signo`, clearing the pending
    /// bit when the queue drains. Caller holds the sigacts lock and pid_rlock/
    /// wlock. (Former body of `tg_dequeue_signal`; the former delegator's
    /// null-`self` null-return now lives at the call site.)
    pub(super) fn dequeue_signal(&self, signo: c_int) -> *mut ksiginfo_t {
        if sigbad(signo) { return ptr::null_mut(); }
        let shared = self.shared_pending_ref();
        let sq = shared.sig_pending_ref_index((signo - 1) as usize);
        let head = sq.queue_ptr();
        let mut ksi: *mut ksiginfo_t = ptr::null_mut();
        if !list_node_is_empty_raw(head) {
            let first = list_node_next_raw(head);
            ksi = container_of::<ksiginfo_t>(first, KSI_LIST_ENTRY_OFF);
            // SAFETY: `ksi` is a `container_of` recovery from the just-verified
            // non-empty (`!list_node_is_empty_raw(head)`) queue, so it is a
            // live `ksiginfo` entry.
            unsafe { KsigInfoAccess::assume(ksi) }.list_entry_ref().detach();
        }
        if list_node_is_empty_raw(head) {
            shared.and_sig_pending_mask(!(1u64 << (signo - 1)));
        }
        ksi
    }

    /// Drain and free every queued `ksiginfo` for `signo`, then clear its
    /// pending bit. Caller holds the sigacts lock. (Former body of
    /// `tg_sigpending_empty`; its `while cur != head` drain is now the
    /// `SigPendingRef::entries` iterator.; the former delegator's null-`self`
    /// no-op now lives at the call site.)
    pub(super) fn sigpending_empty_sig(&self, signo: c_int) {
        if sigbad(signo) { return; }
        let shared = self.shared_pending_ref();
        for ksi in shared.sig_pending_ref_index((signo - 1) as usize).entries() {
            // SAFETY: `ksi` is a `container_of` recovery from a live node of
            // the queue being drained (sigacts lock held), only ever populated
            // with real `ksiginfo` entries; `SigPendingRef::entries` caches each
            // node's successor before yielding it, so detach+free is sound.
            unsafe { KsigInfoAccess::assume(ksi) }.list_entry_ref().detach();
            ksiginfo_free(ksi);
        }
        shared.and_sig_pending_mask(!(1u64 << (signo - 1)));
    }
}

// The `xv6_tgport_*` C-ABI alias layer that used to front thread_group_init/
// thread_group_get/put/live_dec, get_thread_group, tg_shared_pending_init/
// destroy, thread_group_alloc[_kernel], thread_group_add/remove,
// thread_is_group_leader, thread_tgid, thread_group_exit, tg_signal_send,
// tg_signal_pending, tg_dequeue_signal, tg_sigpending_empty,
// tg_recalc_sigpending was collapsed in the P3-1B2 sweep: every caller now
// invokes these canonical names directly (crate-path, no FFI hop).
