//! Terminal session management (POSIX).
//!
//! Rust port of `kernel/tty/session.c` (Phase 2 Wave 12, see
//! `docs/rustify/phase2_plan.md`) -- completes the `tty` module.
//!
//! A session groups process groups and optionally binds them to a
//! controlling terminal. The session leader (thread whose `tgid == sid`)
//! creates the session via `setsid()`.
//!
//! Hierarchy:  session -> pgroup -> thread_group -> thread
//!
//! Session membership (`thread->session`, `pgroup->session`) is
//! protected by the global `pid_lock` (rwlock), exactly as in the C
//! original:
//!   - `pid_wlock` for mutations (setsid, add/remove thread/pg, ctrl-tty
//!     attach/detach)
//!   - `pid_rlock` (or an already-held `pid_wlock`) for read-only access
//!     (getsid, fg_pgrp queries)
//!
//! Lock ordering:  `pid_lock` > `sigacts.lock` > `tcb_lock`. `tty->lock`
//! (a leaf spinlock, never itself blocks) may be taken while `pid_lock`
//! is held -- see [`session_set_ctrl_tty`]'s doc for why this file now
//! does that.
//!
//! # Registry: `SESSION_TABLE` (a `GenTable`) — N-R6a pilot
//!
//! The global registry of live sessions is a [`GenTable`] (the hand-rolled
//! generational slotmap built in `kernel/kstd.rs` for the full-Rust-native
//! proc/thread redesign — `RUST_NATIVE_REDESIGN.md` §3a/§5b), replacing the
//! C-shaped `session_list` intrusive `list_node_t` the C original declared.
//! Sessions still keep their slab allocation — the table stores a stable
//! `NonNull<Session>`, so nothing moves — but liveness is now a *generation
//! check*: a [`Sid`] handed out for a session that later exits and is freed
//! goes stale, and [`session_lookup`] returns `None` instead of dangling.
//! The registry is not internally synchronized; every mutation holds
//! `pid_wlock` and every scan holds `pid_lock` (read or write), matching the
//! module doc's lock discipline exactly as `session_list` did. The process-
//! hierarchy dump (`kernel/proc/proc_shims.rs::session_for_each_all`) walks
//! it through [`session_for_each`]. See the [`SESSION_TABLE`] section below.
//!
//! Cross-subsystem back-edges (`thread.session`, `pgroup.session`,
//! `tty.session`, `fg_pgrp`) remain raw `*mut session` for this pilot — the
//! session is *findable/validatable* through the table (a `Sid` +
//! [`session_lookup`]), but converting those edges to `Sid` keys would
//! cascade into thread/proc/pgroup and is deferred to the later N-R6 stages.
//!
//! # SIGINT delivery fix (mandated, Wave 12)
//!
//! The C original's `session_set_ctrl_tty()` only ever wrote
//! `session->ctrl_tty`; it never wrote the corresponding back-pointer
//! `tty->session`. That meant `kernel/tty/tty.rs`'s
//! `tty_signal_fg_pgroup()` -- which routes ^C/^\/^Z to the terminal's
//! foreground process group by reading `(*t).session` -- could *never*
//! find a session through a live console/pty tty, and silently fell
//! back to signalling `current_thread_ptr()` instead. On the console
//! that fallback thread is a kernel feeder thread, not the foreground
//! shell/job, so ^C was effectively dead (documented as a known gap in
//! `RUST_REWRITE.md` Iteration 23).
//!
//! [`session_set_ctrl_tty`] now sets both sides of the relationship and
//! keeps a [`tty_ref`]/[`tty_unref`] pair balanced against it (so the
//! tty cannot be freed while a session still designates it as the
//! controlling terminal), symmetrically cleared by `__session_detach_ctrl_tty`
//! on every teardown path: replacing an already-set `ctrl_tty`,
//! [`session_hangup`], and session finalization in [`session_unref`].
//! See the [`session_set_ctrl_tty`] doc for the exact design.
//!
//! # `SRef` + off-by-one fix (Phase 3 P3-9e, 2026-07-14)
//!
//! Two, mostly-independent changes landed together in this wave:
//!
//! 1. [`SRef`] -- an RAII smart pointer over `session.ref_cnt`, mirroring
//!    `kernel/kobject.rs`'s `KArc<T>` and `kernel/vfs/inode.rs`'s `IRef`.
//!    Completes the refcount-smart-pointer trilogy (kobject / inode /
//!    session -- the three manual-refcount families in this kernel).
//!    See the type's own doc, just below [`session_unref`].
//! 2. [`session_unref`]'s free-condition off-by-one (documented as a
//!    known, preserved-1:1 bug since Wave 12) is now fixed: see that
//!    function's doc for the exact change. A *second, deeper*
//!    baseline-reference leak found while verifying it (the empty
//!    session parked at `ref_cnt == 1` forever) is now also CLOSED as
//!    P3-BUG3 -- see [`session_unref`]'s doc and `RUST_REWRITE.md`
//!    Iteration 82.

use core::ffi::{c_char, c_int, c_void};
use core::ops::Deref;
use core::ptr;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{ksiginfo, list_node_t, pgroup, pid_t, session, slab_cache_t, thread, thread_group, tty};

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N3.
//
// `Session` IS the kernel-wide Rust definition of
// `kernel/inc/tty/session_types.h`'s `struct session` now: `build.rs`
// blocklists the bindgen-generated form and injects
// `pub use crate::tty::session::Session as session;`, so every
// `crate::bindings::session` path across the crate (thread.session,
// pgroup.session, tty.session, ...) resolves here. `fg_pgrp`/`ctrl_tty`
// pointer fields keep their bindgen pointee paths (`pgroup` redirects
// to the native in `proc/pgroup.rs` — this same wave; `tty` is still
// bindgen); a pointer's own layout never depends on its pointee.
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 exited : 1; uint64 is_kernel : 1; }` inside
/// `struct session` (bindgen's `session__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `exited` at bit 0 and
/// `is_kernel` at bit 1 of the (8-byte) unit's byte 0 — identical to
/// bindgen's `get(0,1)`/`get(1,1)` accessors reproduced below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct SessionFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl SessionFlagBits {
    #[inline]
    pub(crate) fn exited(&self) -> u64 {
        (self.bits & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_exited(&mut self, val: u64) {
        self.bits = (self.bits & !0b01) | ((val as u8) & 0b01);
    }
    #[inline]
    pub(crate) fn is_kernel(&self) -> u64 {
        ((self.bits >> 1) & 0b01) as u64
    }
    #[inline]
    pub(crate) fn set_is_kernel(&mut self, val: u64) {
        self.bits = (self.bits & !0b10) | (((val as u8) << 1) & 0b10);
    }
}

const _: () = {
    assert!(core::mem::size_of::<SessionFlagBits>() == 8, "session anon bitfield size");
    assert!(core::mem::align_of::<SessionFlagBits>() == 8, "session anon bitfield alignment");
};

#[repr(C)]
#[derive(Copy, Clone)]
pub struct Session {
    pub(crate) global_entry: crate::list::ListNode,
    pub(crate) sid: pid_t,
    pub(crate) ref_cnt: core::ffi::c_int,
    pub(crate) flags: SessionFlagBits,
    pub(crate) t_cnt: core::ffi::c_int,
    pub(crate) threads: crate::list::ListNode,
    pub(crate) pg_cnt: core::ffi::c_int,
    pub(crate) pgrps: crate::list::ListNode,
    pub(crate) fg_pgrp: *mut pgroup,
    pub(crate) ctrl_tty: *mut tty,
    // N-R6d-2a: the session caches its own generational key here, stamped by
    // `session_table_insert` right after `SESSION_TABLE.insert` hands it out.
    // The `thread.session`/`pgroup.session` back-edges (converted to `Sid`
    // this sub-step) read it via `session_key_of` to encode a `set_session`
    // in O(1) — cheaper than an O(N) `GenTable::key_of` reverse scan. A
    // `#[repr(C)]` tail field: no C consumer remains (0 `.c` files in-tree),
    // and every session is slab-allocated with `size_of::<session>()`, so
    // growing the struct 96 → 104 is self-contained. `Sid`'s internal layout
    // is unobserved externally; only its 8-byte tail placement matters (the
    // offset assert below pins it).
    pub(crate) self_sid: Sid,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// session { global_entry: list_node_t, sid: pid_t, ref_cnt: c_int,
// __bindgen_anon_1: session__bindgen_ty_1, t_cnt: c_int, threads:
// list_node_t, pg_cnt: c_int, pgrps: list_node_t, fg_pgrp: *mut
// pgroup, ctrl_tty: *mut tty }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe (rv64gc/lp64d)
// against `kernel/inc/tty/session_types.h`: size 96, align 8, offsets
// 0/16/20/[24]/32/40/56/64/80/88 (the anonymous bitfield struct
// occupies [24,32), pinned in the probe by both neighbours).
const _: () = {
    // N-R6d-2a: size 96 → 104 for the appended `self_sid: Sid` (8-byte tail).
    // `Session` align stays 8 (pointer fields dominate `Sid`'s align 4). No C
    // consumer remains, so this is a pure Rust-side growth.
    assert!(core::mem::size_of::<Session>() == 104, "session size");
    assert!(core::mem::align_of::<Session>() == 8, "session alignment");
    assert!(core::mem::size_of::<Sid>() == 8, "Sid must be 8 bytes (session.self_sid tail)");
    assert!(core::mem::offset_of!(Session, self_sid) == 96, "session.self_sid offset");
    assert!(core::mem::offset_of!(Session, global_entry) == 0, "session.global_entry offset");
    assert!(core::mem::offset_of!(Session, sid) == 16, "session.sid offset");
    assert!(core::mem::offset_of!(Session, ref_cnt) == 20, "session.ref_cnt offset");
    assert!(core::mem::offset_of!(Session, flags) == 24, "session anon bitfield offset");
    assert!(core::mem::offset_of!(Session, t_cnt) == 32, "session.t_cnt offset");
    assert!(core::mem::offset_of!(Session, threads) == 40, "session.threads offset");
    assert!(core::mem::offset_of!(Session, pg_cnt) == 56, "session.pg_cnt offset");
    assert!(core::mem::offset_of!(Session, pgrps) == 64, "session.pgrps offset");
    assert!(core::mem::offset_of!(Session, fg_pgrp) == 80, "session.fg_pgrp offset");
    assert!(core::mem::offset_of!(Session, ctrl_tty) == 88, "session.ctrl_tty offset");
};

// P3-1C mesh sweep: tty/tty.rs is in scope for this wave; converted from
// `extern "C"` redeclarations to plain crate-path items (identical
// signatures -- both real `unsafe extern "C" fn`s, every call site
// already wrapped in `unsafe`).
use crate::tty::tty::{tty_ref, tty_unref};

// ===========================================================================
// Externs -- every cross-module C-ABI symbol this file calls, declared
// locally rather than imported by Rust path. Matches the established
// convention across the crate (see `irq/trap.rs`, `irq/syscall.rs`,
// `kernel/tty/tty.rs`'s own externs) rather than reaching through
// `crate::proc::...` module paths.
// ===========================================================================

// P3-D3b: lock/rcu.rs's read-side entry points are plain safe Rust fns
// now that their `#[no_mangle]` exports are gone; reached by crate path.
use crate::lock::rcu::{rcu_read_lock, rcu_read_unlock};

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {

    // printf/panic (matches `kernel/tty/tty.rs`'s own local
    // `tty_assert_errno` extern block exactly).
}

// P3-D3a: the slab entry points are genuinely `unsafe fn` in
// `crate::mm::slab` now that their `#[no_mangle]` exports are gone; this
// file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout — see
// `cffi::raw`'s identical note). Thin cast + safe-facade wrappers
// preserve both.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}

// ===========================================================================
// Errno / signal constants (`kernel/inc/errno.h`, `kernel/inc/uabi/signo.h`).
// Duplicated locally rather than shared cross-module -- matches the
// established per-file convention (see e.g. `kernel/tty/tty.rs`'s
// identical local errno consts).
// ===========================================================================

const EPERM: c_int = 1;
const ESRCH: c_int = 3;
const ENOMEM: c_int = 12;
const EEXIST: c_int = 17;
const EINVAL: c_int = 22;

const SIGHUP: c_int = 1;
const SIGCONT: c_int = 18;

// `is_err`'s canonical home is `crate::kstd` (P3-CS2 centralization).
// P3-CS14: `get_session` now builds a `KResult<*mut session>` internally
// and encodes it to the ABI-fixed `ERR_PTR` exactly once at the `extern "C"`
// boundary via `result_to_errptr`; only the error-return encoding changed.
use crate::kstd::{is_err, result_to_errptr, Errno, GenKey, GenTable, KResult};

// ===========================================================================
// Panic helper -- replicates the C `assert(expr, fmt, ...)` macro
// expansion (`kernel/inc/printf.h`). Same pattern as
// `kernel/tty/tty.rs::tty_assert_errno` / `kernel/tty/tty_dev.rs::
// tty_dev_assert_errno`, generalised to take the function name too
// (session.c's asserts span several functions, not just one).
// ===========================================================================

// P3-2 (zero-C wave): `session_assert`/`session_assert_errno` used to take
// their assertion message as a runtime `&CStr` (the errno variant embedding
// a `%d` inside it); `core::fmt::format_args!` requires a literal format
// string, so they became macros (message/function name captured as
// `:literal` at each invocation, forwarded straight to `kprintln!`) instead
// of plain functions -- same fix as `console.rs`'s equivalent helpers, but
// kept as macros here (rather than inlined) since this file calls them from
// several different functions, not just one.
macro_rules! session_assert {
    ($cond:expr, $func:literal, $msg:literal $(,)?) => {
        if !($cond) {
            __panic_start();
            crate::kprintln!(
                "ASSERTION_FAILURE {}:{}: In function '{}':",
                "kernel/tty/session.rs",
                line!(),
                $func,
            );
            crate::kprintln!($msg);
            __panic_end();
        }
    };
}

macro_rules! session_assert_errno {
    ($cond:expr, $func:literal, $msg:literal, $errno:expr $(,)?) => {
        if !($cond) {
            __panic_start();
            crate::kprintln!(
                "ASSERTION_FAILURE {}:{}: In function '{}':",
                "kernel/tty/session.rs",
                line!(),
                $func,
            );
            crate::kprintln!($msg, $errno);
            __panic_end();
        }
    };
}

// ===========================================================================
// Doubly-linked list primitives (`kernel/inc/list.h`'s plain, non-RCU
// `static inline` helpers, reimplemented natively -- bindgen cannot
// process `static inline` bodies). Same approach as Wave 1's
// `kernel/hlist.rs`. Every helper here requires the caller to already
// hold whatever lock protects the list in question (`pid_wlock` for
// every list this file touches), matching the C originals' implicit
// contract.
// ===========================================================================

/// # Safety
/// `e` must be a valid, writable `list_node_t`.
#[inline]
unsafe fn ll_init(e: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe {
        (*e).next = e;
        (*e).prev = e;
    }
}

/// # Safety
/// `e` must be a valid, readable `list_node_t`.
#[inline]
unsafe fn ll_is_detached(e: *mut list_node_t) -> bool {
    // SAFETY: see fn doc.
    unsafe { (*e).next == e }
}

/// # Safety
/// `e` must be a valid, writable `list_node_t` that is linked into a
/// list (its `prev`/`next` neighbours must themselves be valid, writable
/// `list_node_t`s).
#[inline]
unsafe fn ll_detach(e: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe {
        (*(*e).prev).next = (*e).next;
        (*(*e).next).prev = (*e).prev;
        ll_init(e);
    }
}

/// Insert `entry` immediately after `prev` (`kernel/inc/list.h`'s
/// `list_entry_insert`).
///
/// # Safety
/// `prev` and `entry` must be valid, writable `list_node_t`s; `prev`
/// must be linked into a list (its `next` neighbour must be a valid,
/// writable `list_node_t`).
#[inline]
unsafe fn ll_insert(prev: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe {
        let next = (*prev).next;
        (*entry).prev = prev;
        (*entry).next = next;
        (*prev).next = entry;
        (*next).prev = entry;
    }
}

/// Push `entry` onto the back of the list headed by `head`
/// (`kernel/inc/list.h`'s `list_entry_push_back`).
///
/// # Safety
/// `head` and `entry` must be valid, writable `list_node_t`s; `head`
/// must be a properly initialized list head (self-linked when empty).
#[inline]
unsafe fn ll_push_back(head: *mut list_node_t, entry: *mut list_node_t) {
    // SAFETY: see fn doc.
    unsafe { ll_insert((*head).prev, entry) };
}

// ===========================================================================
// Slab cache + global session list.
// ===========================================================================

/// `UnsafeCell<MaybeUninit<T>>` + `unsafe impl Sync` for one file-scope
/// slab cache. Same pattern as `kernel/tty/tty.rs`'s `SyncCell<T>`
/// (re-declared locally here per that file's own note: "the established
/// crate convention").
struct SyncCell<T>(core::cell::UnsafeCell<core::mem::MaybeUninit<T>>);
// SAFETY: `SESSION_CACHE` is mutated exclusively through the C kernel's
// own synchronised entry points (`slab_cache_init`/`slab_alloc`
// internally lock).
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn uninit() -> Self {
        SyncCell(core::cell::UnsafeCell::new(core::mem::MaybeUninit::uninit()))
    }
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get() as *mut T
    }
}

static SESSION_CACHE: SyncCell<slab_cache_t> = SyncCell::uninit();

// ===========================================================================
// Session registry — a `GenTable` (N-R6a pilot of the proc/thread slotmap).
//
// This replaces the C-shaped `session_list` intrusive `list_node_t` that
// used to be the global registry of live sessions. Sessions still keep
// their slab allocation (the table stores a stable `NonNull<Session>`, never
// the object by value — nothing moves); what changes is that liveness is now
// a *generation check*, not a raw-pointer walk. A [`Sid`] handed out for a
// session that later exits and is freed goes stale: [`session_lookup`]
// returns `None` instead of dangling — the generational analogue of the
// P3-BUG3 "the freed session must stop being reachable" property, now
// enforced by the type/table rather than by discipline alone.
//
// The `global_entry` `ListNode` field stays in the (layout-frozen,
// `#[repr(C)]`, offset-asserted) `Session` struct — the struct is the
// kernel-wide definition of `struct session` and its size/offsets are fixed
// by the C ABI, so no field is added or removed. `global_entry` is simply no
// longer linked into a global list; `__session_alloc` still self-links it
// (harmless, keeps the field initialized).
// ===========================================================================

/// Family tag for session keys — an ASCII marker, only its distinctness
/// matters (it makes [`Sid`] a different type from any future `Pid`/`Tid`/
/// `Pgid` key so handles can't be crossed between tables).
pub(crate) const SID_TAG: u64 = u64::from_le_bytes(*b"SESSION\0");

/// Typed generational handle to a live session in [`SESSION_TABLE`],
/// distinct from the userspace numeric session id (which stays as the
/// `Session::sid` field, still returned by `getsid`). A stale `Sid` (its
/// session exited and was freed) resolves to `None` through
/// [`session_lookup`].
pub(crate) type Sid = GenKey<SID_TAG>;

/// Registry capacity. Sized to `NR_THREAD` (the kernel's hard cap on live
/// threads, `kernel/inc/param.h`): every live session has at least one
/// member thread (the baseline reference is dropped the instant a session
/// empties — P3-BUG3), so the number of concurrent sessions can never exceed
/// the number of live threads. Sizing the table at that true upper bound
/// guarantees [`GenTable::insert`] never returns `None` in practice, so
/// `session_alloc` cannot start failing where it previously succeeded — no
/// capacity regression. The `[Slot; NSESSION]` is all-`None`/zero at rest, so
/// it costs BSS, not data-segment, space.
const NSESSION: usize = 10_000;

/// `UnsafeCell` + `unsafe impl Sync` newtype so the registry can be a
/// file-scope `static`, exactly like `proc_shims`'s `PROC_TABLE`. The
/// `GenTable` is **not** internally synchronized (see its doc); all access
/// is serialized by `pid_lock` — `pid_wlock` for the `&mut self` mutators
/// (`insert`/`remove_ptr`), `pid_rlock` (or a held `pid_wlock`) for the
/// `&self` readers (`get`/`for_each`) — the identical discipline that
/// protected the retired `session_list`.
struct SessionTableCell(core::cell::UnsafeCell<GenTable<Session, NSESSION, SID_TAG>>);
// SAFETY: every access below is taken while holding `pid_lock` at the
// required strength, which serializes all mutation and excludes any writer
// during a read — so the raw `UnsafeCell` access is race-free (see the
// module doc's lock-ordering note and each helper's own comment).
unsafe impl Sync for SessionTableCell {}

static SESSION_TABLE: SessionTableCell =
    SessionTableCell(core::cell::UnsafeCell::new(GenTable::new()));

/// Insert `s` into the registry, returning its generational key (discarded
/// by `session_alloc`, which addresses the session by its stable pointer for
/// this pilot). `None` only if the table is full — impossible given
/// [`NSESSION`]'s sizing, handled defensively by the caller.
///
/// Caller must hold `pid_wlock` (matches every `session_alloc` site:
/// `session_init`/kthread-hierarchy at boot and `session_setsid`).
fn session_table_insert(s: *mut session) -> Option<Sid> {
    pid_assert_wholding();
    let nn = NonNull::new(s)?;
    // SAFETY: `pid_wlock` is held (asserted above), so this `&mut` to the
    // registry is exclusive — no other hart holds a reference into the table
    // (readers need `pid_rlock`, excluded by our write lock).
    let table = unsafe { &mut *SESSION_TABLE.0.get() };
    let sid = table.insert(nn)?;
    // N-R6d-2a: cache the freshly-issued key in the session itself so the
    // `thread.session`/`pgroup.session` back-edge accessors can stamp it in
    // O(1) (`session_key_of`) instead of an O(N) `GenTable::key_of` scan.
    // SAFETY: `s` is live and exclusively ours here — just allocated,
    // pre-publication, under `pid_wlock`; `self_sid` is a plain aligned word.
    unsafe { (*s).self_sid = sid };
    Some(sid)
}

/// Remove `s` from the registry (by its stable pointer — the cached
/// `self_sid` is only ever read while the session is live), bumping the slot
/// generation so any
/// outstanding [`Sid`] to it goes stale. A no-op if `s` was never inserted.
///
/// Caller must hold `pid_wlock` (the session teardown/free path — matches
/// where the old code unlinked `session_list`).
fn session_table_remove(s: *mut session) {
    pid_assert_wholding();
    let Some(nn) = NonNull::new(s) else { return };
    // SAFETY: `pid_wlock` is held (asserted above) — exclusive `&mut`, as in
    // `session_table_insert`.
    let table = unsafe { &mut *SESSION_TABLE.0.get() };
    table.remove_ptr(nn);
}

/// Generation-checked lookup: resolve a [`Sid`] to a live session pointer,
/// or `None` if the session it named has been freed (stale key) — the safe
/// replacement for a raw `*mut session` traversal. This composes with (does
/// not yet replace) the `SRef` refcount: `SRef` keeps a *known* session
/// pinned, while a `Sid` answers "is this session still alive at all?"
/// precisely where a raw pointer could not. Full `SRef` elimination is a
/// later wave.
///
/// Caller must hold `pid_lock` (reader or writer); the returned pointer is
/// only stable under that same protection.
#[allow(dead_code)]
pub(crate) fn session_lookup(sid: Sid) -> Option<*mut session> {
    // SAFETY: the caller holds `pid_lock` (read or write), which excludes
    // every `&mut self` mutator (they hold the write lock) — so this shared
    // reference into the registry is race-free for its (short, non-looping
    // over shared-mutable-state) lifetime. No `&Session` is formed here; the
    // raw pointer is handed straight back (freeze-hazard note in the
    // `GenTable` doc).
    let table = unsafe { &*SESSION_TABLE.0.get() };
    table.get(sid).map(|nn| nn.as_ptr())
}

/// Encode a `*mut session` as the generational [`Sid`] to store in a back-edge
/// (`thread.session`/`pgroup.session`): null → [`Sid::NONE`] ("no session"),
/// otherwise the session's own cached key `self_sid` (stamped at
/// [`session_table_insert`]). O(1) — the read side of the back-edge
/// `set_session` accessors, avoiding an O(N) [`GenTable::key_of`] reverse scan.
///
/// Caller holds `pid_wlock` (every `set_session` site does — the read of
/// `self_sid` on a live session is race-free under it).
pub(crate) fn session_key_of(s: *mut session) -> Sid {
    match core::ptr::NonNull::new(s) {
        None => Sid::NONE,
        // SAFETY: `s` is a live `*mut session` (caller passes an owned/looked-up
        // session under `pid_wlock`); `self_sid` is a plain aligned word read.
        Some(nn) => unsafe { (*nn.as_ptr()).self_sid },
    }
}

/// Visit every live session in the registry, passing each as a raw
/// `*mut session`. The `GenTable` replacement for walking `session_list`;
/// used by `proc/proc_shims.rs::session_for_each_all` (process-hierarchy
/// dump). Caller must hold `pid_lock` (reader or writer).
pub(crate) fn session_for_each(mut f: impl FnMut(*mut session)) {
    // SAFETY: caller holds `pid_lock` (read or write) — excludes all
    // writers, so this shared reference into the registry is race-free; the
    // scan hands the callback raw `NonNull`/`*mut` pointers, never `&Session`
    // (freeze-hazard note in the `GenTable` doc).
    let table = unsafe { &*SESSION_TABLE.0.get() };
    table.for_each(|nn| f(nn.as_ptr()));
}

// ===========================================================================
// Boot-time initialisation.
// ===========================================================================

/// # Safety
/// `initproc` must be a live `thread` with a non-null `thread_group` and
/// `pgroup` (mirrors the C original's unconditional derefs).
pub(crate) unsafe extern "C" fn session_init(initproc: *mut thread) {
    // SAFETY: see fn doc; this runs once at boot before any other
    // `session_*` entry point, so `SESSION_CACHE` is exclusively ours to
    // initialize. (The registry `SESSION_TABLE` is a const-initialized empty
    // `GenTable` — no runtime init needed, unlike the retired `session_list`
    // head this used to self-link here.)
    unsafe {
        let ret = slab_cache_init(
            SESSION_CACHE.get(),
            c"session_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<session>(),
            crate::bindings::SLAB_FLAG_STATIC as u64,
        );
        session_assert_errno!(
            ret == 0,
            "session_init",
            "session_init: failed to init session_cache, errno={}",
            ret,
        );

        let tg = (*initproc).thread_group;
        session_assert!(!tg.is_null(), "session_init", "session_init: initproc has no thread_group");

        let pg = (*initproc).pgroup;
        session_assert!(!pg.is_null(), "session_init", "session_init: initproc has no pgroup");

        let s_raw = session_alloc((*initproc).pid);
        session_assert!(!s_raw.is_null(), "session_init", "session_init: session_alloc failed");
        // P3-9e: `SRef` over `session_alloc`'s freshly-taken reference
        // (its `ref_cnt == 1` postcondition -- `SRef::from_raw`'s exact
        // contract). This is the permanent, boot-created session 1 --
        // initproc is always a member, so it never empties and the
        // baseline-drop in `session_remove_thread`/`session_remove_pg`
        // (P3-BUG3) never fires for it; the reference is simply handed
        // back out via `into_raw` below and stays held for the life of
        // the system.
        let s = SRef::from_raw(s_raw);

        session_add_pg(SRef::as_ptr(&s), pg);
        session_add_thread(SRef::as_ptr(&s), initproc);
        (*SRef::as_ptr(&s)).fg_pgrp = pg;
        let _ = SRef::into_raw(s);
    }
}

// ===========================================================================
// Allocation / reference counting.
// ===========================================================================

/// # Safety
/// The returned pointer, if non-null, is freshly allocated,
/// exclusively-owned slab memory whose `list_node_t` members are
/// initialized; every other field is left zeroed for the caller to fill
/// in (mirrors the C original's `memset` + partial `list_entry_init`
/// calls).
unsafe fn __session_alloc() -> *mut session {
    // SAFETY: `slab_alloc` returns either null or a pointer to
    // `size_of::<session>()` freshly allocated bytes from
    // `SESSION_CACHE` (initialized by `session_init` before any caller
    // can reach here).
    unsafe {
        let s = slab_alloc(SESSION_CACHE.get()) as *mut session;
        if s.is_null() {
            return ptr::null_mut();
        }
        core::ptr::write_bytes(s as *mut u8, 0, core::mem::size_of::<session>());
        ll_init(&raw mut (*s).global_entry);
        ll_init(&raw mut (*s).threads);
        ll_init(&raw mut (*s).pgrps);
        s
    }
}

pub(crate) extern "C" fn session_alloc(sid: pid_t) -> *mut session {
    // SAFETY: `s`, once non-null, is exclusively ours (just allocated by
    // `__session_alloc`) until it is registered in `SESSION_TABLE` and
    // returned to the caller.
    unsafe {
        let s = __session_alloc();
        if s.is_null() {
            return ptr::null_mut();
        }
        (*s).sid = sid;
        (*s).ctrl_tty = ptr::null_mut();
        (*s).fg_pgrp = ptr::null_mut();
        (*s).ref_cnt = 1;
        (*s).t_cnt = 0;
        (*s).pg_cnt = 0;
        // Register in the generational session table (N-R6a) — the
        // replacement for `ll_push_back(&session_list, ...)`. On the
        // impossible "table full" case (see `NSESSION`'s sizing) fail the
        // alloc cleanly: free the slab object and return null, exactly the
        // contract `session_alloc` already has for allocation failure,
        // rather than hand back an unregistered session.
        if session_table_insert(s).is_none() {
            slab_free(s as *mut c_void);
            return ptr::null_mut();
        }
        s
    }
}

/// # Safety
/// `s`, if non-null, must be a live `session` (mirrors the C original's
/// unconditional deref once past the null check).
pub(crate) unsafe extern "C" fn session_ref(s: *mut session) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc. `s->ref_cnt` is a plain (non-`_Atomic`) `int`
    // field in the C struct, RMW'd there via `atomic_inc` ==
    // `__atomic_fetch_add(..., __ATOMIC_SEQ_CST)`; `AtomicI32::from_ptr`
    // over the same field pointer reproduces that exactly (matches
    // `kernel/proc/proc_shims.rs`'s `atomic_fetch_sub_i32` precedent).
    unsafe {
        AtomicI32::from_ptr(&raw mut (*s).ref_cnt).fetch_add(1, Ordering::SeqCst);
    }
}

/// # Safety
/// `s`, if non-null, must be a live `session` (mirrors the C original's
/// unconditional deref once past the null check).
///
/// # Off-by-one fix (Phase 3 P3-9e, 2026-07-14)
///
/// The C original's `atomic_dec(&s->ref_cnt)` macro expands to
/// `__atomic_fetch_sub(..., __ATOMIC_SEQ_CST)`, which -- per the GCC/C11
/// atomic builtin contract -- returns the value *before* the
/// subtraction, not after. The C code then named that pre-decrement
/// value `new_val` and freed when `new_val == 0`, i.e. only on the call
/// that decrements ref_cnt from 0 to -1 -- one call *past* the
/// legitimate "last owner drops its reference" transition (1 -> 0).
/// Ported here 1:1 in Wave 12 (`RUST_REWRITE.md` Iteration 24) and
/// flagged as a leaks-not-double-frees bug.
///
/// Fixed here: the free condition now compares the pre-decrement value
/// against `1` (the call that actually drives `ref_cnt` to `0`),
/// matching every other refcount in this crate (`kobject_put` frees on
/// its *post*-decrement `count == 0`; `vfs_iput`'s CAS loop is
/// equivalent) instead of the C's off-by-one. The `session_assert!`
/// bound tightens from `>= 0` to `>= 1` to match: under correct pairing
/// the pre-decrement value can never legitimately be `<= 0` any more
/// (that would mean this call is decrementing a reference that had
/// already reached zero -- a double-unref/use-after-free bug in the
/// *caller*, not a normal teardown step), so a future such bug now
/// fails loudly instead of silently falling through to a (previously
/// unreachable) free branch.
///
/// No double-free risk from tightening the free condition: `fetch_sub`
/// is a single atomic RMW, so under any concurrent race at most one
/// caller ever observes the pre-decrement value `== 1` -- the same
/// "exactly one hart sees the zero transition" guarantee
/// `kobject_put`/`vfs_iput` already rely on.
///
/// # The baseline-reference leak (P3-BUG3) -- now CLOSED
///
/// Found while verifying the off-by-one fix above: fixing the comparison
/// made `session_unref` free correctly whenever the pre-decrement count
/// reaches `1`, but [`session_alloc`]'s baseline `ref_cnt = 1` was never
/// separately dropped on ordinary teardown -- only on
/// [`session_setsid`]'s `pgroup_alloc`-failure path. A successful
/// `setsid()` ends at `ref_cnt == 3` (baseline + 1 pg + 1 thread); the
/// leader exiting drops the thread ref (3->2) and the process group
/// emptying drops the pg ref (2->1), leaving the session parked at
/// `ref_cnt == 1` (the baseline) forever -- a per-session leak on every
/// ordinary interactive `setsid`.
///
/// Closed here: [`session_remove_thread`] and [`session_remove_pg`] now
/// drop the baseline reference iff their removal *emptied* the session
/// (`pg_cnt == 0 && t_cnt == 0`), captured BEFORE the member `session_unref`
/// so it reads live counts. The mechanism is use-after-free-safe by the
/// invariant `ref_cnt == baseline(1 while non-empty) + pg_cnt + t_cnt`:
/// while the departing member's own reference is still held the count is
/// `>= 2`, so the member unref can never free (it only ever lowers
/// `ref_cnt` to the baseline `1`); the explicit baseline unref on the
/// empty transition is therefore the *unique* free point, and
/// `empty -> freed` is terminal (the free sets `exited = 1`, after which
/// [`session_add_thread`]/[`session_add_pg`] reject new members with
/// `-ESRCH`). All raw `*mut session` back-pointers (`thread.session`,
/// `pgroup.session`, `fg_pgrp`, `tty.session`) are cleared before their
/// unref, so none dangles. The boot session ([`session_init`]/initproc)
/// and the kernel session (`kernel/proc/thread.rs`'s `KERNEL_SESSION`,
/// whose `is_kernel` pgroup is never removed) always retain a member, so
/// neither ever empties and neither is ever freed. See `RUST_REWRITE.md`
/// Iteration 82 (Known issue marked FIXED as P3-BUG3).
pub(crate) unsafe extern "C" fn session_unref(s: *mut session) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc; same `AtomicI32::from_ptr` reasoning as
    // `session_ref`.
    unsafe {
        let old_val = AtomicI32::from_ptr(&raw mut (*s).ref_cnt).fetch_sub(1, Ordering::SeqCst);
        session_assert!(old_val >= 1, "session_unref", "Session reference count would go non-positive");
        if old_val == 1 {
            (*s).flags.set_exited(1);
            // Make sure a still-attached controlling tty never keeps a
            // dangling `tty->session` back-pointer into slab memory this
            // call is about to free (previously "defensive"/dead code
            // under the off-by-one bug; now the live path the last
            // dropped reference actually takes).
            __session_detach_ctrl_tty(s);
            // Deregister from the generational session table (N-R6a) — the
            // replacement for the `session_list` unlink. This bumps the
            // slot's generation, so any outstanding `Sid` to this session
            // now resolves to `None` (the generational liveness guarantee);
            // `session_table_remove` asserts `pid_wlock` is held, exactly as
            // this free branch already required for the `session_list` unlink
            // and slab free.
            session_table_remove(s);
            slab_free(s as *mut c_void);
        }
    }
}

// ---------------------------------------------------------------------------
// `SRef` -- an RAII smart pointer over `session.ref_cnt` (Phase 3 P3-9e).
// Completes the refcount-smart-pointer trilogy: `KArc<T>`
// (`kernel/kobject.rs`) covers `kobject`, `IRef` (`kernel/vfs/inode.rs`)
// covers `vfs_inode`, `SRef` covers `session` -- the last manual-refcount
// family in this kernel. Session's refcount is a plain (non-kobject)
// `AtomicI32`-backed `int` field manipulated by this file's own
// `session_ref`/`session_unref` (just above) -- it cannot use `KArc<T>`,
// and unlike `vfs_inode` there is no bespoke "dup, but only if still
// live" C-ABI helper for [`SRef::try_from_raw`] to call into (the
// `vfs_idup_not_zero` analog), so [`session_try_ref`] below is `SRef`'s
// own small addition: a CAS retry loop directly on `ref_cnt`, using the
// same `Ordering::SeqCst` as `session_ref`/`session_unref` (this file's
// established single ordering for the field, not a second,
// independently-reasoned Acquire/Relaxed split like
// `kobject_try_get`'s). `SRef` mirrors `KArc<T>`/`IRef`'s shape
// (`Clone`/`Drop`/`Deref`/`from_raw`/`try_from_raw`/`into_raw`/
// `as_ptr`) but is built directly on top of the ref/unref pair rather
// than re-deriving the atomics -- it inherits their exact,
// already-proven ordering (including the off-by-one fix just above)
// instead of a second, independently-reasoned-about copy.
// `session_ref`/`session_unref`'s internals are untouched by this
// wrapper -- `SRef` only wraps the get/put pair at call sites, exactly
// as the `KArc`/`IRef` precedents wrap their own get/put pairs rather
// than reimplementing them. The existing `#[no_mangle]`
// `session_ref`/`session_unref`/`get_session` C-ABI exports are
// unaffected.
//
// # Invariant
// A live `SRef` owns exactly one held reference on the pointee
// `session` (one increment of `ref_cnt` not yet matched by a
// `session_unref`). `Clone` (-> `session_ref`) and `Drop` (->
// `session_unref`) are the only ways that invariant is created/
// destroyed internally; [`SRef::from_raw`]/[`SRef::try_from_raw`]
// accept one from the caller (the success postcondition of
// `session_alloc`/`session_ref`/a successful `try_from_raw` itself) and
// [`SRef::into_raw`] hands one back out -- the same `from_raw`/
// `into_raw` contract `Box`/`Arc`/`KArc`/`IRef` use, for call sites that
// still store one held reference as a bare `*mut session` (e.g.
// `thread.session`/`pgroup.session`, or `kernel/proc/thread.rs`'s
// `KERNEL_SESSION` static).
// ---------------------------------------------------------------------------

/// `kobject_try_get`/`vfs_idup_not_zero` analog for `session`: attempt to
/// upgrade a bare, liveness-unproven `*mut session` (e.g. one just read
/// out of `session_list` without holding a reference of its own) by
/// incrementing `ref_cnt` only if it has not already reached zero.
/// Returns `false` if a concurrent final `session_unref` already zeroed
/// (and is freeing, or has freed) `*s`.
///
/// Not part of `session.h`'s C ABI (no `#[no_mangle]`) -- this is an
/// `SRef`-only addition; `session_ref`/`session_unref` above are
/// preserved 1:1 (beyond the off-by-one fix) and untouched.
///
/// # Safety
/// `s` must be non-null and point to a `session` that is still valid to
/// dereference for the duration of this call (e.g. still under
/// `pid_rlock`/`pid_wlock`, or otherwise known live) -- the same
/// precondition `kobject_try_get`/`vfs_idup_not_zero` have.
unsafe fn session_try_ref(s: *mut session) -> bool {
    // SAFETY: see fn doc; same `AtomicI32::from_ptr` reasoning as
    // `session_ref`/`session_unref`.
    let rc = unsafe { AtomicI32::from_ptr(&raw mut (*s).ref_cnt) };
    let mut old = rc.load(Ordering::SeqCst);
    loop {
        if old <= 0 {
            return false;
        }
        // SeqCst on both success and failure: matches this field's one
        // established ordering (see the module section doc above)
        // rather than introducing a second, independently-reasoned
        // Acquire/Relaxed split.
        match rc.compare_exchange(old, old + 1, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return true,
            Err(cur) => old = cur,
        }
    }
}

/// See the module section doc above.
pub(crate) struct SRef {
    ptr: NonNull<session>,
}

impl SRef {
    /// Take ownership of an existing, already-held session reference.
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a live `session` whose
    /// `ref_cnt` currently carries a reference the caller is
    /// transferring to the returned `SRef` -- the caller must not also
    /// call `session_unref` (or drop another `SRef` over the same
    /// reference) on that same held count. This is exactly the success
    /// postcondition of `session_alloc`/`session_ref`.
    pub(crate) unsafe fn from_raw(ptr: *mut session) -> Self {
        debug_assert!(!ptr.is_null(), "SRef::from_raw: ptr is NULL");
        // SAFETY: caller guarantees `ptr` is non-null (also debug-checked
        // above) and owns an already-held reference (function contract).
        SRef { ptr: unsafe { NonNull::new_unchecked(ptr) } }
    }

    /// Attempt to upgrade a bare, liveness-unproven `*mut session` into
    /// an owned `SRef` -- see [`session_try_ref`]. Returns `None` if the
    /// refcount had already reached zero (a concurrent final `Drop`/
    /// `session_unref` raced ahead of this call).
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a `session` that is still
    /// valid to dereference for the duration of this call -- same
    /// precondition [`session_try_ref`] has.
    pub(crate) unsafe fn try_from_raw(ptr: *mut session) -> Option<Self> {
        debug_assert!(!ptr.is_null(), "SRef::try_from_raw: ptr is NULL");
        // SAFETY: caller guarantees `ptr` is live for the duration of
        // this call (fn doc).
        if unsafe { session_try_ref(ptr) } {
            // SAFETY: `ptr` was just proven non-null-derived and live by
            // the successful `session_try_ref` above, which also
            // established the held reference this `SRef` now owns.
            Some(SRef { ptr: unsafe { NonNull::new_unchecked(ptr) } })
        } else {
            None
        }
    }

    /// Give up the owned reference without decrementing the refcount,
    /// returning the raw pointer -- the flip side of [`SRef::from_raw`],
    /// for a call site that stores one held reference as a bare pointer
    /// (e.g. `thread.session`/`pgroup.session`, or
    /// `kernel/proc/thread.rs`'s `KERNEL_SESSION` static).
    pub(crate) fn into_raw(this: Self) -> *mut session {
        let ptr = this.ptr.as_ptr();
        core::mem::forget(this);
        ptr
    }

    /// The raw pointer this `SRef` owns a reference on, without giving up
    /// ownership (contrast [`SRef::into_raw`]) -- for passing to the
    /// still-C-ABI-shaped helpers in this module that borrow a `*mut
    /// session` without taking or dropping a reference of their own
    /// (`session_add_pg`, `session_add_thread`, field writes like
    /// `fg_pgrp`).
    pub(crate) fn as_ptr(this: &Self) -> *mut session {
        this.ptr.as_ptr()
    }
}

impl Clone for SRef {
    fn clone(&self) -> Self {
        // Struct invariant: `self` owns a currently-held reference, so
        // `session_ref`'s precondition (an already-held reference to
        // increment from) holds; matches `KArc::clone`/`IRef::clone`'s
        // use of `kobject_get`/`vfs_idup` under the same reasoning.
        unsafe { session_ref(self.ptr.as_ptr()) };
        SRef { ptr: self.ptr }
    }
}

impl Drop for SRef {
    fn drop(&mut self) {
        // Struct invariant: `self` owns exactly one held reference,
        // which this call gives up; `session_unref` runs its own
        // finalization path (detach + free) if this was the last one.
        // Every current `SRef` call site upholds `session_unref`'s
        // precondition (`pid_wlock` held -- module doc's lock-ordering
        // note): both current call sites (`session_init`,
        // `session_setsid`) drop/into_raw while still holding it.
        unsafe { session_unref(self.ptr.as_ptr()) };
    }
}

impl Deref for SRef {
    type Target = session;
    fn deref(&self) -> &session {
        // SAFETY: struct invariant -- a live `SRef` holds a reference,
        // which keeps the pointee allocated for as long as any `SRef`
        // over it exists (mirrors `KArc<T>`/`IRef::deref`'s reasoning).
        // Shared access only: mutation of individual `session` fields
        // elsewhere in this crate is synchronized by `pid_wlock`, not by
        // unique ownership of the whole struct -- the same discipline
        // every existing `*mut session` call site already relies on.
        unsafe { self.ptr.as_ref() }
    }
}

// ===========================================================================
// Thread membership.
// ===========================================================================

/// Add a thread to a session's thread list. Sets `t->session`/`t->sid`.
///
/// # Safety
/// `s` and `t`, if non-null, must be live (mirrors the C original's
/// unconditional derefs once past the null checks). Caller must hold
/// `pid_wlock`.
pub(crate) unsafe extern "C" fn session_add_thread(s: *mut session, t: *mut thread) -> c_int {
    pid_assert_wholding();
    if s.is_null() || t.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).flags.exited() != 0 {
            return -ESRCH;
        }
        // N-R6d-2a: `thread.session` is now a generational `Sid`. A live thread
        // never holds a stale key (it is detached from its session before that
        // session can be freed), so `!is_none()` is the exact structural
        // analog of the old `!is_null()` "already in a session" guard.
        if !(*t).session.is_none() {
            return -EEXIST;
        }
        (*t).session = session_key_of(s);
        (*t).sid = (*s).sid;
        ll_init(&raw mut (*t).sid_entry);
        ll_push_back(&raw mut (*s).threads, &raw mut (*t).sid_entry);
        (*s).t_cnt += 1;
        session_ref(s);
    }
    0
}

/// Remove a thread from its session's thread list. Clears
/// `t->session`/`t->sid`.
///
/// # Safety
/// `s` and `t`, if non-null, must be live. Caller must hold `pid_wlock`.
pub(crate) unsafe extern "C" fn session_remove_thread(s: *mut session, t: *mut thread) -> c_int {
    pid_assert_wholding();
    if s.is_null() || t.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        // N-R6d-2a: compare the stored `Sid` against `s`'s own key (both name
        // the same live session iff equal) — the `Sid` analog of `!= s`.
        if (*t).session != session_key_of(s) {
            return -EINVAL;
        }
        if !ll_is_detached(&raw mut (*t).sid_entry) {
            ll_detach(&raw mut (*t).sid_entry);
        }
        (*s).t_cnt -= 1;
        (*t).session = Sid::NONE;
        // Capture the empty transition BEFORE the member unref below (counts
        // are already decremented here). While this member's own reference is
        // still held, `ref_cnt >= 2` (baseline + this ref), so the member
        // `session_unref` can never free — it only ever brings `ref_cnt` down
        // to the baseline.
        let now_empty = (*s).pg_cnt == 0 && (*s).t_cnt == 0;
        (*t).sid = 0;
        session_unref(s);
        if now_empty {
            // P3-BUG3 fix: the last member just left, so drop `session_alloc`'s
            // baseline reference — this is the unique free point for the now
            // empty session (otherwise it leaks forever at `ref_cnt == 1`).
            // MUST be the last use of `s`: it may free the pointee.
            session_unref(s);
        }
    }
    0
}

// ===========================================================================
// Process group membership.
// ===========================================================================

/// # Safety
/// `s` and `pg`, if non-null, must be live. Caller must hold `pid_wlock`.
pub(crate) unsafe extern "C" fn session_add_pg(s: *mut session, pg: *mut pgroup) -> c_int {
    pid_assert_wholding();
    if s.is_null() || pg.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).flags.exited() != 0 {
            return -ESRCH;
        }
        // N-R6d-2a: `pgroup.session` is a generational `Sid` (see the thread
        // edge above) — `!is_none()` is the structural analog of `!is_null()`.
        if !(*pg).session.is_none() {
            return -EEXIST;
        }
        (*pg).session = session_key_of(s);
        ll_init(&raw mut (*pg).list_entry);
        ll_push_back(&raw mut (*s).pgrps, &raw mut (*pg).list_entry);
        (*s).pg_cnt += 1;
        session_ref(s);
    }
    0
}

/// # Safety
/// `s` and `pg`, if non-null, must be live. Caller must hold `pid_wlock`.
pub(crate) unsafe extern "C" fn session_remove_pg(s: *mut session, pg: *mut pgroup) -> c_int {
    pid_assert_wholding();
    if s.is_null() || pg.is_null() {
        return -EINVAL;
    }
    // SAFETY: see fn doc.
    unsafe {
        // N-R6d-2a: `Sid` analog of `!= s` (see `session_remove_thread`).
        if (*pg).session != session_key_of(s) {
            return -EINVAL;
        }
        if !ll_is_detached(&raw mut (*pg).list_entry) {
            ll_detach(&raw mut (*pg).list_entry);
        }
        (*s).pg_cnt -= 1;
        // If the foreground pgroup is being removed, clear it.
        if (*s).fg_pgrp == pg {
            (*s).fg_pgrp = ptr::null_mut();
        }
        (*pg).session = Sid::NONE;
        // Capture the empty transition BEFORE the member unref below (counts
        // are already decremented here). While this member's own reference is
        // still held, `ref_cnt >= 2` (baseline + this ref), so the member
        // `session_unref` can never free — it only ever brings `ref_cnt` down
        // to the baseline.
        let now_empty = (*s).pg_cnt == 0 && (*s).t_cnt == 0;
        session_unref(s);
        if now_empty {
            // P3-BUG3 fix: the last member just left, so drop `session_alloc`'s
            // baseline reference — this is the unique free point for the now
            // empty session (otherwise it leaks forever at `ref_cnt == 1`).
            // MUST be the last use of `s`: it may free the pointee.
            session_unref(s);
        }
    }
    0
}

// ===========================================================================
// Controlling terminal.
// ===========================================================================

/// Detach whatever controlling terminal `s` currently has (if any),
/// clearing the tty's back-pointer and releasing the ref
/// `session_set_ctrl_tty` took. Internal teardown helper: unlike
/// `session_set_ctrl_tty`, it does not bail out on `s->exited` -- this
/// is exactly what [`session_hangup`] and [`session_unref`]'s
/// finalization branch call *while* tearing a session down.
///
/// # Safety
/// `s` must be a live `session`. Caller must hold `pid_wlock`.
unsafe fn __session_detach_ctrl_tty(s: *mut session) {
    // SAFETY: see fn doc; `old_tty`, if non-null, was itself established
    // as live by a prior `session_set_ctrl_tty` call (which took a
    // `tty_ref` on it), so it is still live here.
    unsafe {
        let old_tty = (*s).ctrl_tty;
        (*s).ctrl_tty = ptr::null_mut();
        if !old_tty.is_null() {
            (*old_tty).session = ptr::null_mut();
            tty_unref(old_tty);
        }
    }
}

/// Attach, replace, or detach (`new_tty == NULL`) a session's
/// controlling terminal.
///
/// # SIGINT-delivery fix (mandated, Wave 12 -- see the module doc)
///
/// The C original only ever did `s->ctrl_tty = tty`. That left
/// `tty->session` permanently null, which broke
/// `kernel/tty/tty.rs::tty_signal_fg_pgroup`'s only way of finding a
/// session from a `tty` -- so ^C/^\/^Z could never reach the terminal's
/// foreground process group. This port wires up both sides of the
/// relationship:
///
/// - Sets `new_tty->session = s` (the missing back-pointer) so
///   `tty_signal_fg_pgroup` can find `s` from `t` and route the signal
///   via `session_get_fg_pgid` + `pgroup_kill`, instead of falling back
///   to killing `current_thread_ptr()` (dead on the console, since that
///   fallback thread is a kernel feeder thread, not the foreground job).
/// - Takes a [`tty_ref`] on `new_tty` for as long as `s` designates it
///   as the controlling terminal, and [`tty_unref`]s whatever `old_tty`
///   this replaces (or is cleared to null) via
///   [`__session_detach_ctrl_tty`] -- so the tty can never be freed out
///   from under a session that still points to it, and the ref/unref
///   pairing stays balanced no matter how many times the controlling
///   tty is attached, replaced, or cleared. `session_hangup` and
///   `session_unref`'s finalization branch both go through the same
///   detach helper, so every teardown path clears the back-pointer too.
///
/// Re-setting the same `new_tty` that is already `s`'s `ctrl_tty` is a
/// no-op (skips the ref/unref dance entirely) rather than an unbalanced
/// unref+ref of the same tty.
///
/// # Safety
/// `s`, if non-null, must be live. `new_tty`, if non-null, must be a
/// live `tty` previously returned by `tty_alloc` (mirrors
/// [`tty_ref`]/[`tty_unref`]'s own contract). Caller must hold
/// `pid_wlock`.
pub(crate) unsafe extern "C" fn session_set_ctrl_tty(s: *mut session, new_tty: *mut tty) {
    pid_assert_wholding();
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).flags.exited() != 0 {
            return;
        }
        if (*s).ctrl_tty == new_tty {
            return;
        }
        __session_detach_ctrl_tty(s);
        (*s).ctrl_tty = new_tty;
        if !new_tty.is_null() {
            (*new_tty).session = s;
            tty_ref(new_tty);
        }
    }
}

/// # Safety
/// `s`, if non-null, must be live. Caller must hold `pid_wlock`.
pub(crate) unsafe extern "C" fn session_get_ctrl_tty(s: *mut session) -> *mut tty {
    pid_assert_wholding();
    if s.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: see fn doc.
    unsafe { (*s).ctrl_tty }
}

// ===========================================================================
// Foreground process group.
// ===========================================================================

/// # Safety
/// `s`, if non-null, must be live.
pub(crate) unsafe extern "C" fn session_set_fg_pgid(s: *mut session, pgid: pid_t) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).flags.exited() != 0 {
            return;
        }
        let pg = get_pgroup(pgid);
        if is_err(pg) {
            return;
        }
        if (*pg).flags.exited() != 0 {
            return; // cannot set an exited pgroup as foreground
        }
        // N-R6d-2a: `Sid` analog of `!= s` — the pgroup must belong to `s`.
        if (*pg).session != session_key_of(s) {
            return; // pgroup must belong to this session
        }
        (*s).fg_pgrp = pg;
    }
}

/// # Safety
/// `s`, if non-null, must be live.
pub(crate) unsafe extern "C" fn session_get_fg_pgid(s: *mut session) -> pid_t {
    if s.is_null() {
        return -1;
    }
    // SAFETY: see fn doc.
    unsafe {
        let fg = (*s).fg_pgrp;
        if fg.is_null() {
            return -1;
        }
        (*fg).pgid
    }
}

// P3-D2b: the proc-object entry points (proc/{pid,thread_group,
// pgroup}.rs) are plain crate-path items now that their `#[no_mangle]`
// exports are gone (identical signatures) -- this replaces both the
// `extern "C"` redeclarations that sat in the block above and the
// `#[link_name = "get_pgroup"]` block that used to sit here.
use crate::proc::{
    get_pgroup, get_pid_thread, pgroup_add_tg, pgroup_add_thread,
    pgroup_alloc, pgroup_remove_tg, pgroup_remove_thread,
    pid_assert_wholding, pid_wlock, pid_wunlock, tg_signal_send, thread_tgid,
};

// ===========================================================================
// setsid / getsid (POSIX).
// ===========================================================================

/// Send SIGHUP + SIGCONT to a thread's thread group.
///
/// # Safety
/// `t` must be a live `thread`.
unsafe fn __hangup_signal_tg(t: *mut thread) {
    // SAFETY: see fn doc.
    unsafe {
        let tg = (*t).thread_group;
        if tg.is_null() {
            return;
        }
        // `ksiginfo` is a `list_node_t` (two raw pointers), two `*mut
        // thread` fields, an `i32`, and a `siginfo_t` (itself all
        // `i32`/raw-pointer/union-of-primitives fields) -- every field
        // is valid when null/zero, same reasoning as
        // `kernel/proc/access.rs::zeroed_ksiginfo` (duplicated locally
        // here per this file's established cross-module convention).
        let mut info: ksiginfo = core::mem::zeroed();
        info.signo = SIGHUP;
        tg_signal_send(tg, &raw mut info);
        info.signo = SIGCONT;
        tg_signal_send(tg, &raw mut info);
    }
}

/// Mark a session as exited and send SIGHUP (then SIGCONT) to its
/// foreground process group -- or do nothing if it has none. Also
/// disassociates the controlling terminal (via
/// [`__session_detach_ctrl_tty`], which also clears the tty's
/// back-pointer -- see the module doc's SIGINT-fix section).
///
/// Called when the session leader exits or the controlling terminal is
/// disconnected. Currently has no live caller in the tree (matches the
/// C original, which was likewise never wired to a SIGHUP-on-hangup
/// trigger) -- kept and force-linked per this project's "port every
/// declared public symbol" rule; future SIGHUP wiring is out of this
/// wave's scope.
///
/// # Safety
/// `s`, if non-null, must be live. Caller must hold `pid_wlock`.
pub(crate) unsafe extern "C" fn session_hangup(s: *mut session) {
    if s.is_null() {
        return;
    }
    // SAFETY: see fn doc.
    unsafe {
        if (*s).flags.exited() != 0 {
            return;
        }
        pid_assert_wholding();

        (*s).flags.set_exited(1);

        let fg = (*s).fg_pgrp;
        if !fg.is_null() && (*fg).flags.exited() == 0 {
            let head = &raw mut (*fg).threads;
            let off = core::mem::offset_of!(thread, pg_entry);
            crate::list_for_each!(head, off, t, {
                __hangup_signal_tg(t as *mut thread);
            });
        }

        __session_detach_ctrl_tty(s);
        (*s).fg_pgrp = ptr::null_mut();
    }
}

/// Create a new session: the calling thread becomes the session leader
/// of a new session and the process-group leader of a new process
/// group. POSIX: must not be called by a process-group leader.
pub(crate) extern "C" fn session_setsid() -> pid_t {
    let p = crate::machine::current_thread_ptr();
    let tgid = thread_tgid(p);

    pid_wlock();

    // SAFETY: `p` is the live current thread (kernel invariant --
    // `current_thread_ptr` always returns a valid running thread's
    // pointer); `pid_wlock` is held from here until every return path
    // below explicitly unlocks first.
    unsafe {
        if (*p).pgid == tgid {
            pid_wunlock();
            return -EPERM;
        }

        let s_raw = session_alloc(tgid);
        if s_raw.is_null() {
            pid_wunlock();
            return -ENOMEM;
        }
        // P3-9e: `SRef` over `session_alloc`'s freshly-taken reference
        // (its `ref_cnt == 1` postcondition -- `SRef::from_raw`'s exact
        // contract). Owned locally until this function either transfers
        // it out permanently (success, via `into_raw` below -- the new
        // session's baseline reference, same as `session_init`'s) or
        // lets it `Drop` (the `pgroup_alloc`-failure branch just below,
        // replacing the previous manual `session_unref(s)` call there).
        let s = SRef::from_raw(s_raw);

        let tg = (*p).thread_group;
        let pg = pgroup_alloc(tgid, tg);
        if pg.is_null() {
            // Deliberate fix (Wave 12): the C original called
            // `slab_free(s)` directly here, bypassing the registry
            // deregistration that `session_unref` performs.
            // `session_alloc` (above) already registered `s` in the
            // global `SESSION_TABLE` (N-R6a; formerly `session_list`), so a
            // direct free would leave a dangling `NonNull<Session>` in the
            // table -- a use-after-free the next time anything scans it
            // (e.g. `session_for_each`). `s->ref_cnt == 1` here (nothing
            // else has referenced `s` yet), so dropping `s` (-> the
            // sole owner's `session_unref`) is exactly the "sole owner
            // drops its reference" case and correctly deregisters (via
            // `session_table_remove`, bumping the slot generation so any
            // stray `Sid` goes stale) before freeing. P3-9e: with the
            // `session_unref` off-by-one fixed (see that fn's doc), this
            // drop now actually frees `s` instead of leaking it -- explicit
            // `drop(s)` here (not a plain scope-end `Drop`) so the
            // free/deregister runs *before* `pid_wunlock()`, preserving the
            // original code's lock ordering (`SESSION_TABLE`/`session_unref`'s
            // finalization branch require `pid_wlock` held, same as every
            // other mutation site -- module doc).
            drop(s);
            pid_wunlock();
            return -ENOMEM;
        }

        if !tg.is_null() {
            pgroup_remove_tg(tg);
        }
        pgroup_remove_thread(p);
        // N-R6d-2a: resolve the thread's stored session `Sid` to a live pointer
        // (null if `NONE`/stale) before removing it from that session — same
        // "detach from current session if any" as the old raw-pointer read.
        let cur_sess = session_lookup((*p).session).unwrap_or(ptr::null_mut());
        if !cur_sess.is_null() {
            session_remove_thread(cur_sess, p);
        }

        session_add_pg(SRef::as_ptr(&s), pg);
        if !tg.is_null() {
            pgroup_add_tg(pg, tg);
        }
        pgroup_add_thread(pg, p);
        session_add_thread(SRef::as_ptr(&s), p);
        (*SRef::as_ptr(&s)).fg_pgrp = pg; // becomes foreground group

        // Success: hand `s`'s baseline reference back out as a bare
        // pointer without dropping it. The baseline is now released
        // automatically once the session empties -- when the leader exits
        // and its process group goes away, `session_remove_thread`/
        // `session_remove_pg` drop it on the empty transition (P3-BUG3, see
        // `session_unref`'s doc). Until then it keeps the live session
        // pinned, exactly as before.
        let _ = SRef::into_raw(s);

        pid_wunlock();
    }
    tgid
}

pub(crate) extern "C" fn session_getsid(pid: pid_t) -> pid_t {
    if pid == 0 {
        let cur = crate::machine::current_thread_ptr();
        // SAFETY: `cur` is the live current thread.
        return unsafe { (*cur).sid };
    }
    if pid < 0 {
        return -EINVAL;
    }

    rcu_read_lock();
    let target = get_pid_thread(pid);
    if is_err(target) {
        rcu_read_unlock();
        return -ESRCH;
    }
    // SAFETY: `target` is a live thread (not an ERR_PTR, checked above)
    // for as long as the RCU read-side critical section is held.
    let sid = unsafe { (*target).sid };
    rcu_read_unlock();

    sid
}

/// Look up a session by its SID.
///
/// Finds the thread whose `pid == sid` (the session leader) via
/// `get_pid_thread`, then returns its session pointer.
///
/// Returns the session pointer, or `ERR_PTR(-ESRCH)` if not found. The
/// caller must be inside an `rcu_read_lock()` section, or hold
/// `pid_lock`; the returned pointer is only stable under that same
/// protection.
pub(crate) extern "C" fn get_session(sid: pid_t) -> *mut session {
    result_to_errptr(get_session_inner(sid))
}

fn get_session_inner(sid: pid_t) -> KResult<*mut session> {
    let t = get_pid_thread(sid);
    if is_err(t) {
        return Err(Errno::Srch);
    }
    // SAFETY: `t` is a live thread (not an ERR_PTR, checked above) for
    // as long as the caller's RCU read-side section / `pid_lock` hold
    // lasts (fn doc).
    // N-R6d-2a: resolve the thread's session `Sid` to a live pointer — a stale
    // key (session freed) resolves to null → `Srch`, the correct "session gone"
    // answer this getsid path already returns for a null session.
    let s = session_lookup(unsafe { (*t).session }).unwrap_or(ptr::null_mut());
    if s.is_null() {
        return Err(Errno::Srch);
    }
    Ok(s)
}
