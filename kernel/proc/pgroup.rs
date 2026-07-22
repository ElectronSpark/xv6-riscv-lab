//! `kernel/proc/pgroup.rs` — POSIX process-group operations.
//!
//! Direct port of `kernel/proc/pgroup.c`. All struct-field access goes
//! through the C shims in `proc_rust_shims.c` because `struct pgroup`,
//! `struct thread`, and `struct thread_group` use bitfields and
//! `_Atomic int` members that bindgen cannot reproduce safely.
//!
//! Hierarchy:  session → pgroup → thread_group → thread
//! Lock ordering:  pid_lock > sigacts.lock > tcb_lock

#![allow(non_camel_case_types)]
#![allow(dead_code)]

use core::ffi::c_void;
use core::ptr;
use core::ptr::NonNull;

use crate::kstd::{GenKey, GenTable};
use crate::proc::access::{err_ptr, is_err_const};

// ---------------------------------------------------------------------------
// Kernel struct aliases — never dereferenced directly from this file.
// (P3-D1a: formerly opaque `[u8; 0]` markers; now the real `crate::bindings`
// types so the direct Rust calls into `proc_shims` type-check unchanged.)
// ---------------------------------------------------------------------------
// `Thread`/`ThreadGroup` appear in `pub(crate)` fn signatures (e.g.
// `pgroup_alloc`, `pgroup_add_thread`) so they must be `pub(crate)`; `Session`
// only appears in `pub(super)` helpers (`pgroup_session_resolve/_store`), so it
// tightens to `pub(super)`.
pub(crate) type Thread      = crate::bindings::thread;
pub(crate) type ThreadGroup = crate::bindings::thread_group;
pub(super) type Session     = crate::bindings::session;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N3.
//
// `Pgroup` IS the kernel-wide Rust definition of
// `kernel/inc/proc/pgroup_types.h`'s `struct pgroup` now: `build.rs`
// blocklists the bindgen-generated form and injects
// `pub use crate::proc::pgroup::Pgroup as pgroup;`, so every
// `crate::bindings::pgroup` path across the crate resolves here.
// `leader`/`session` pointer fields keep their bindgen pointee paths
// (`thread_group` redirects to the native in `thread_group.rs`,
// `session` to `tty/session.rs` — both this same wave); a pointer's own
// layout never depends on its pointee.
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 exited : 1; uint64 is_kernel : 1; }` inside
/// `struct pgroup` (bindgen's `pgroup__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `exited` at bit 0 and
/// `is_kernel` at bit 1 of the (8-byte) unit's byte 0 — identical to
/// bindgen's `get(0,1)`/`get(1,1)` accessors reproduced below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub(crate) struct PgroupFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl PgroupFlagBits {
    // Read out-of-proc (`tty/session.rs` reads `(*pg).flags.exited()`), so this
    // one stays `pub(crate)`; the other three are only reached in-proc
    // (`proc_shims` bit macros / `access.rs`) → `pub(super)`.
    #[inline]
    pub(crate) fn exited(&self) -> u64 {
        (self.bits & 0b01) as u64
    }
    #[inline]
    pub(super) fn set_exited(&mut self, val: u64) {
        self.bits = (self.bits & !0b01) | ((val as u8) & 0b01);
    }
    #[inline]
    pub(super) fn is_kernel(&self) -> u64 {
        ((self.bits >> 1) & 0b01) as u64
    }
    #[inline]
    pub(super) fn set_is_kernel(&mut self, val: u64) {
        self.bits = (self.bits & !0b10) | (((val as u8) << 1) & 0b10);
    }
}

const _: () = {
    assert!(core::mem::size_of::<PgroupFlagBits>() == 8, "pgroup anon bitfield size");
    assert!(core::mem::align_of::<PgroupFlagBits>() == 8, "pgroup anon bitfield alignment");
};

// ---------------------------------------------------------------------------
// `Pgid` / `PGID_TAG` — the process-group family's generational key  [N-R6d-2b]
//
// A pgroup gains a generational registry ([`PGROUP_TABLE`] below) for the first
// time this sub-step. `Pgid` is the internal generational handle into it — a
// `Copy` typed key, distinct at compile time (via `PGID_TAG`) from `Tid`/`Sid`,
// so handles cannot be crossed between tables. It is SEPARATE from the pgroup's
// numeric userspace `pgid` field (setpgid/getpgid/kill(-pgid) identity), which
// is unchanged. The two `*mut pgroup` edges converted this sub-step
// (`thread.pgroup`, `session.fg_pgrp`) store a `Pgid`; a stale one (the pgroup
// was freed) resolves to null through [`pgroup_lookup`] — the existing "pgroup
// gone" answer readers already null-check.
// ---------------------------------------------------------------------------

/// Family tag for process-group keys — an ASCII marker, only its distinctness
/// matters (it makes [`Pgid`] a different type from `Tid`/`Sid`).
const PGID_TAG: u64 = u64::from_le_bytes(*b"PGROUP\0\0");

/// Typed generational handle to a live pgroup in [`PGROUP_TABLE`], distinct
/// from the userspace numeric process-group id (the `Pgroup::pgid` field, still
/// used by setpgid/getpgid/kill). A stale `Pgid` (its pgroup was freed)
/// resolves to `None` through [`pgroup_lookup`].
pub(crate) type Pgid = GenKey<PGID_TAG>;

// `Pgroup` is `pub use`-re-exported crate-wide as `crate::bindings::pgroup`
// (`bindings.rs`), so the struct itself must stay `pub` (a `pub use` of a
// `pub(crate)` item is E0365). Per-field visibility is still minimized:
// `list_entry`/`pgid`/`flags`/`threads`/`session` are touched out-of-proc
// (`tty/session.rs`) → `pub(crate)`; `leader`/`t_cnt`/`p_cnt`/`thread_groups`
// only in-proc (`proc_shims` field macros) → `pub(super)`; `self_pgid` only
// in-file (the `key`/`set_key` methods) → private.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Pgroup {
    pub(crate) list_entry: crate::list::ListNode,
    pub(crate) pgid: crate::bindings::pid_t,
    pub(super) leader: *mut crate::bindings::thread_group,
    pub(crate) flags: PgroupFlagBits,
    pub(super) t_cnt: core::ffi::c_int,
    pub(crate) threads: crate::list::ListNode,
    pub(super) p_cnt: core::ffi::c_int,
    pub(super) thread_groups: crate::list::ListNode,
    // N-R6d-2a: the session back-edge converted off `*mut session` to the
    // session family's generational key [`Sid`] (`GenKey<SID_TAG>`, 8
    // bytes/align 4 — the exact span the `*mut session` occupied, so this
    // offset (88, already 8-aligned) and the struct size (96) are unchanged;
    // the layout asserts below prove it). `Sid::NONE` (`generation == 0`) is
    // the "no session" edge. Resolved to a `*mut session` through the pilot's
    // `SESSION_TABLE` by the `session` accessors
    // (`PgroupAccess::session_ptr`/`set_session` → `proc_shims::pg_session`/
    // `pg_set_session` → `pgroup_session_resolve`/`pgroup_session_store`); a
    // stale `Sid` (session freed) resolves to null — the existing "session
    // gone" semantics readers already null-check.
    pub(crate) session: crate::tty::session::Sid,
    // N-R6d-2b: the pgroup caches its own generational key here, stamped by
    // `pgroup_table_insert` right after `PGROUP_TABLE.insert` hands it out. The
    // `thread.pgroup`/`session.fg_pgrp` back-edges (converted to `Pgid` this
    // sub-step) read it via `pgroup_key_of` to encode a store in O(1) — cheaper
    // than an O(N) `GenTable::key_of` reverse scan (mirror of `Session.self_sid`
    // from N-R6d-2a). A `#[repr(C)]` tail field: no C consumer remains (0 `.c`
    // files in-tree) and every pgroup is slab-allocated with `size_of::<pgroup>()`,
    // so growing the struct 96 → 104 is self-contained. `Pgid`'s internal layout
    // is unobserved externally; only its 8-byte tail placement matters (the
    // offset assert below pins it).
    self_pgid: Pgid,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// pgroup { list_entry: list_node_t, pgid: pid_t, leader: *mut
// thread_group, __bindgen_anon_1: pgroup__bindgen_ty_1, t_cnt: c_int,
// threads: list_node_t, p_cnt: c_int, thread_groups: list_node_t,
// session: *mut session }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe (rv64gc/lp64d)
// against `kernel/inc/proc/pgroup_types.h`: size 96, align 8, offsets
// 0/16/24/[32]/40/48/64/72/88 (the anonymous bitfield struct occupies
// [32,40), pinned in the probe by both neighbours).
const _: () = {
    // N-R6d-2b: size 96 → 104 for the appended `self_pgid: Pgid` (8-byte tail).
    // `Pgroup` align stays 8 (pointer/ListNode fields dominate `Pgid`'s align 4).
    // No C consumer remains, so this is a pure Rust-side growth (mirror of the
    // N-R6d-2a `Session` 96 → 104 growth).
    assert!(core::mem::size_of::<Pgroup>() == 104, "pgroup size");
    assert!(core::mem::align_of::<Pgroup>() == 8, "pgroup alignment");
    assert!(core::mem::size_of::<Pgid>() == 8, "Pgid must be 8 bytes (pgroup.self_pgid tail)");
    assert!(core::mem::offset_of!(Pgroup, self_pgid) == 96, "pgroup.self_pgid offset");
    assert!(core::mem::offset_of!(Pgroup, list_entry) == 0, "pgroup.list_entry offset");
    assert!(core::mem::offset_of!(Pgroup, pgid) == 16, "pgroup.pgid offset");
    assert!(core::mem::offset_of!(Pgroup, leader) == 24, "pgroup.leader offset");
    assert!(core::mem::offset_of!(Pgroup, flags) == 32, "pgroup anon bitfield offset");
    assert!(core::mem::offset_of!(Pgroup, t_cnt) == 40, "pgroup.t_cnt offset");
    assert!(core::mem::offset_of!(Pgroup, threads) == 48, "pgroup.threads offset");
    assert!(core::mem::offset_of!(Pgroup, p_cnt) == 64, "pgroup.p_cnt offset");
    assert!(core::mem::offset_of!(Pgroup, thread_groups) == 72, "pgroup.thread_groups offset");
    // N-R6d-2a: `session` is now an `Sid` (`GenKey`, 8 bytes/align 4), not a
    // `*mut session` (8 bytes/align 8). Same 8-byte span at an already-8-aligned
    // offset, so this offset and the `size == 96` assert above are unchanged.
    assert!(
        core::mem::size_of::<crate::tty::session::Sid>() == 8,
        "Sid must be 8 bytes to preserve pgroup.session span"
    );
    assert!(core::mem::offset_of!(Pgroup, session) == 88, "pgroup.session offset");
};

// ---------------------------------------------------------------------------
// N-METH goal #1: the pgroup's own field accessors as inherent methods.
//
// These four are the ONLY places in this file that dereference a `*mut Pgroup`
// to touch a field directly (`(*pg).session` / `(*pg).self_pgid`); everything
// else goes through the `pg_*` shims. Turning the direct derefs into `&self`/
// `&mut self` methods is the sound, in-file slice of the free-fn → method
// conversion: each caller already holds `pid_lock` at the strength the borrow
// needs (readers under `pid_rlock`, writers under exclusive `pid_wlock`), the
// reads are single and non-looping (no freeze/`noalias` spin-loop hazard — see
// the `PGROUP_TABLE` section note), and no synchronization changes. The
// null-tolerant free-fn wrappers below keep their exact signatures, so no call
// site (in-file or across the crate) changes.
impl Pgroup {
    /// The pgroup's own cached generational key (`self_pgid`), stamped by
    /// [`pgroup_table_insert`]. Read side of [`pgroup_key_of`].
    #[inline]
    fn key(&self) -> Pgid {
        self.self_pgid
    }

    /// Stamp the pgroup's own cached generational key.
    #[inline]
    fn set_key(&mut self, pgid: Pgid) {
        self.self_pgid = pgid;
    }

    /// The stored `session` generational edge ([`Sid`](crate::tty::session::Sid)).
    /// Read side of [`pgroup_session_resolve`].
    #[inline]
    fn session_sid(&self) -> crate::tty::session::Sid {
        self.session
    }

    /// Set the stored `session` generational edge.
    #[inline]
    fn set_session_sid(&mut self, sid: crate::tty::session::Sid) {
        self.session = sid;
    }
}

// N-R6d-2a: the `pgroup.session` back-edge resolve/store — mirror of the
// thread edge (`thread_session_resolve`/`thread_session_store`), reading/
// writing this struct's own generational `Sid` field and delegating the
// Sid↔`*mut session` mapping to the pilot's `SESSION_TABLE`. Backing the
// `pg_session`/`pg_set_session` shims (signatures unchanged, so every caller
// — pgroup.rs itself, access.rs `PgroupAccess` — is untouched).

/// Resolve pgroup `pg`'s stored `session` [`Sid`] to a live `*mut session`, or
/// null if [`NONE`](crate::tty::session::Sid) ("no session") or stale (session
/// freed → the "session gone" answer callers null-check). Caller holds
/// `pid_lock` (reader or writer — [`session_lookup`](crate::tty::session::session_lookup)
/// requires it; single non-looping generation-checked read, no `&Session`).
// NO-STANDALONE-FN: `pgroup_session_resolve`/`pgroup_session_store` are now
// `Pgroup::session_resolve`/`session_store` associated fns.
impl Pgroup {
    pub(super) fn session_resolve(pg: *mut Pgroup) -> *mut Session {
        // SAFETY: `pg` is a live `*mut pgroup` (shim contract); forming the shared
        // `&self` for this single non-looping read of its own `session` `Sid` field
        // is race-free under the caller's `pid_lock` (writers need the exclusive
        // `pid_wlock`).
        let sid = unsafe { (*pg).session_sid() };
        crate::tty::session::session_lookup(sid).unwrap_or(ptr::null_mut())
    }

    /// Store `s` as pgroup `pg`'s `session` edge: `s == null` → `Sid::NONE`,
    /// otherwise `s`'s own cached [`Sid`] (`session.self_sid`). Caller holds
    /// `pid_wlock` (every `set_session` site does).
    pub(super) fn session_store(pg: *mut Pgroup, s: *mut Session) {
        let sid = crate::tty::session::session_key_of(s);
        // SAFETY: `pg` is a live `*mut pgroup`; the exclusive `&mut self` write of
        // its own `session` field under the caller's `pid_wlock` is race-free.
        unsafe {
            (*pg).set_session_sid(sid);
        }
    }
} // impl Pgroup

// ===========================================================================
// Pgroup registry — a `GenTable` (N-R6d-2b).
//
// The pgroup family gains its first generational registry here. Unlike the
// session pilot (which retired an intrusive `session_list`), pgroups never had
// a global registry — they were reachable only by walking `thread.pgroup` /
// `session.pgrps`. This table gives the two converted edges (`thread.pgroup`,
// `session.fg_pgrp`) a liveness-checked handle: a `Pgid` for a pgroup that later
// empties and is freed goes stale, and [`pgroup_lookup`] returns `None` instead
// of dangling. Pgroups keep their slab allocation (the table stores a stable
// `NonNull<Pgroup>`, never the object by value — nothing moves).
//
// Not internally synchronized (see the `GenTable` doc); every access is
// serialized by `pid_lock` — `pid_wlock` for the `&mut self` mutators
// (`insert`/`remove_ptr`), a held `pid_lock` for the `&self` reader (`get`).
//
// Freeze/`noalias` screening (memory `freeze-noalias-hazard`): `GenTable` is
// `Freeze`, but mutators form `&mut` only under `pid_wlock` (exclusive) and the
// resolve/store helpers do a single, non-looping generation-checked lookup —
// never a `&Freeze` field read hoisted out of a spin/wait loop. The table hands
// out only raw `NonNull`/`*mut` (never `&Pgroup`), so no `&Freeze` of a
// concurrently-mutated pointee is formed either.
// ===========================================================================

/// Registry capacity — `NR_THREAD` (`kernel/inc/param.h`, the kernel's hard cap
/// on live threads). Every live pgroup contains at least one thread group with
/// at least one live thread, so the number of concurrent pgroups can never
/// exceed the number of live threads — sizing the table at that true upper
/// bound guarantees [`GenTable::insert`] never returns `None` where a pgroup was
/// successfully allocated (no capacity regression). All-`None`/zero at rest →
/// BSS, not data-segment.
const NPGROUP: usize = 10_000;

/// `UnsafeCell` + `unsafe impl Sync` newtype so the registry can be a
/// file-scope `static`, exactly like the session pilot's `SESSION_TABLE` and
/// `THREAD_TABLE`. The `GenTable` is **not** internally synchronized; all access
/// is serialized by `pid_lock` (see the section note above).
struct PgroupTableCell(core::cell::UnsafeCell<GenTable<Pgroup, NPGROUP, PGID_TAG>>);
// SAFETY: every access below is taken while holding `pid_lock` at the required
// strength (mutators assert `pid_wlock`); the raw `UnsafeCell` access is thus
// race-free, same discipline as `SESSION_TABLE`/`THREAD_TABLE`.
unsafe impl Sync for PgroupTableCell {}

static PGROUP_TABLE: PgroupTableCell =
    PgroupTableCell(core::cell::UnsafeCell::new(GenTable::new()));

/// Register `pg` in [`PGROUP_TABLE`], returning its fresh [`Pgid`] and caching
/// it in the pgroup's own `self_pgid` (so [`pgroup_key_of`] is O(1)). `None`
/// only if the table is full — impossible given [`NPGROUP`]'s sizing, handled
/// defensively by [`pgroup_alloc`]. Caller must hold `pid_wlock` (every
/// `pgroup_alloc` site does — boot hierarchy init and `setpgid`/`setsid`).
// NO-STANDALONE-FN: the registry ops + lookup/key_of are `Pgroup::` associated
// fns (`table_insert`/`table_remove`/`lookup`/`key_of`).
impl Pgroup {
    fn table_insert(pg: *mut Pgroup) -> Option<Pgid> {
        pid_assert_wholding();
        let nn = NonNull::new(pg)?;
        // SAFETY: `pid_wlock` is held (asserted above), so this `&mut` to the
        // registry is exclusive — no other hart holds a reference into the table
        // (readers need `pid_lock`, excluded by our write lock).
        let table = unsafe { &mut *PGROUP_TABLE.0.get() };
        let pgid = table.insert(nn)?;
        // Cache the freshly-issued key so the back-edge accessors can stamp it in
        // O(1) (`pgroup_key_of`) instead of an O(N) `GenTable::key_of` scan.
        // SAFETY: `pg` is live and exclusively ours here — just allocated,
        // pre-publication, under `pid_wlock`; the `&mut self` `self_pgid` write is
        // uncontended.
        unsafe { (*pg).set_key(pgid) };
        Some(pgid)
    }

    /// Deregister `pg` from [`PGROUP_TABLE`] (by its stable pointer — the cached
    /// `self_pgid` is only ever read while the pgroup is live), bumping the slot
    /// generation so any outstanding [`Pgid`] to it (a `thread.pgroup` or
    /// `session.fg_pgrp` edge) goes stale → resolves to null. A no-op if `pg` was
    /// never registered. Caller must hold `pid_wlock` (the `__pgroup_cleanup`
    /// teardown path does).
    fn table_remove(pg: *mut Pgroup) {
        pid_assert_wholding();
        let Some(nn) = NonNull::new(pg) else { return };
        // SAFETY: `pid_wlock` is held (asserted above) — exclusive `&mut`.
        let table = unsafe { &mut *PGROUP_TABLE.0.get() };
        table.remove_ptr(nn);
    }

    /// Generation-checked lookup: resolve a [`Pgid`] to a live `*mut pgroup`, or
    /// `None` if the pgroup it named has been freed (stale key) — the safe
    /// replacement for a raw `*mut pgroup` traversal of the converted edges.
    ///
    /// Caller must hold `pid_lock` (reader or writer); the returned pointer is only
    /// stable under that same protection.
    pub(crate) fn lookup(pgid: Pgid) -> Option<*mut Pgroup> {
        // SAFETY: the caller holds `pid_lock` (read or write), which excludes every
        // `&mut self` mutator (they hold the write lock) — so this shared reference
        // into the registry is race-free for its (short, non-looping) lifetime. No
        // `&Pgroup` is formed here; the raw pointer is handed straight back
        // (freeze-hazard note in the section above).
        let table = unsafe { &*PGROUP_TABLE.0.get() };
        table.get(pgid).map(|nn| nn.as_ptr())
    }

    /// Encode a `*mut pgroup` as the generational [`Pgid`] to store in a back-edge
    /// (`thread.pgroup`/`session.fg_pgrp`): null → [`Pgid::NONE`] ("no pgroup"),
    /// otherwise the pgroup's own cached key `self_pgid` (stamped at
    /// [`pgroup_table_insert`]). O(1) — the write side of the converted edges,
    /// avoiding an O(N) [`GenTable::key_of`] reverse scan.
    ///
    /// Caller holds `pid_wlock` (every store site does — the read of `self_pgid` on
    /// a live pgroup is race-free under it).
    pub(crate) fn key_of(pg: *mut Pgroup) -> Pgid {
        match NonNull::new(pg) {
            None => Pgid::NONE,
            // SAFETY: `pg` is a live `*mut pgroup` (caller passes an owned/looked-up
            // pgroup under `pid_wlock`); the `&self` `self_pgid` read is single and
            // non-looping.
            Some(nn) => unsafe { (*nn.as_ptr()).key() },
        }
    }
} // impl Pgroup

// Errno constants used here (matches kernel/inc/errno.h).
const EPERM:  i32 = 1;
const ESRCH:  i32 = 3;
const ENOMEM: i32 = 12;
const EEXIST: i32 = 17;
const EINVAL: i32 = 22;

// ---------------------------------------------------------------------------
// extern shim surface
// ---------------------------------------------------------------------------
// P3-D1a: the `xv6_*`/`pg_*`/`t_*`/`tg_*` accessor shims are ordinary Rust
// fns in `proc_shims.rs`; call them via crate paths instead of `extern "C"`
// redeclarations. Call sites keep the same bare names.
use crate::proc::proc_shims::{
    pg_atomic_dec_t_cnt, pg_dec_p_cnt, pg_dec_t_cnt, pg_exited, pg_inc_p_cnt, pg_inc_t_cnt,
    pg_is_kernel, pg_list_entry_detach, pg_list_entry_init, pg_list_entry_is_detached, pg_p_cnt,
    pg_pgid, pg_session, pg_set_exited, pg_set_leader, pg_set_p_cnt, pg_set_pgid, pg_set_session,
    pg_set_t_cnt, pg_t_cnt, pg_tg_list_entry_is_detached, pg_tgs_detach, pg_tgs_init,
    pg_tgs_push_back, pg_threads_detach, pg_threads_init, pg_threads_push_back, t_parent,
    t_pg_entry_init, t_pg_entry_is_detached, t_pgid, t_pgroup, t_pid, t_session, t_set_pgid,
    t_set_pgroup, t_sid, t_thread_group, tg_list_entry_init, tg_pgroup, tg_set_pgroup,
    xv6_current_thread, xv6_panic, xv6_pgroup_slab_alloc, xv6_pgroup_slab_free,
    xv6_pgroup_slab_init, xv6_pid_rlock, xv6_pid_runlock, xv6_pid_wholding, xv6_pid_wlock,
    xv6_pid_wunlock, xv6_tg_send_signo,
};

// P3-D3b: the `pub safe fn rcu_read_lock/_unlock` extern redeclarations
// that sat here were dead — this file's RCU read-side sections go through
// the `xv6_rcu_read_lock`/`xv6_rcu_read_unlock` shims (see the comment
// below) — so they are simply deleted now that lock/rcu.rs's
// `#[no_mangle]` exports are gone.

// P3-D2b: `get_pid_thread` (proc/pid.rs) is a plain (file-private, so no E0659
// glob-reexport ambiguity) crate-path item now that its `#[no_mangle]` export
// is gone (identical signature) -- it used to be an `extern "C"` redeclaration
// in the block above.
// NO-STANDALONE-FN: `thread_tgid` is now the `ThreadAccess` method
// `resolve_tgid`; the two call sites below construct the handle via `from_ptr`.
use crate::proc::ProcTable;
use crate::proc::access::ThreadAccess;

// P3-1C mesh sweep: tty/session.rs is in scope for this wave, same
// opaque-marker reinterpret precedent used throughout this file (this
// file's `Session`/`Pgroup` are distinct-but-layout-identical stand-ins
// for the real `crate::bindings` structs).
/// SAFETY: only called with a live session and a live pgroup (both
/// non-null-checked by the caller immediately before the call) --
/// matches the real fn's contract. Its `c_int` return is discarded here
/// exactly as both call sites in this file always did.
fn session_add_pg(s: *mut Session, pg: *mut Pgroup) -> i32 {
    unsafe {
        crate::tty::session::session_add_pg(
            s as *mut c_void as *mut crate::bindings::session,
            pg as *mut c_void as *mut crate::bindings::pgroup,
        )
    }
}
/// SAFETY: see `session_add_pg`.
fn session_remove_pg(s: *mut Session, pg: *mut Pgroup) -> i32 {
    unsafe {
        crate::tty::session::session_remove_pg(
            s as *mut c_void as *mut crate::bindings::session,
            pg as *mut c_void as *mut crate::bindings::pgroup,
        )
    }
}

// `rcu_read_lock` / `rcu_read_unlock` are static-inline; use the Rust shims.
use crate::lock::rcu::{rcu_read_lock as rcu_lock, rcu_read_unlock as rcu_unlock};

#[inline]
fn pid_assert_wholding() {
    if xv6_pid_wholding() == 0 {
        panic_pgroup("pid_wlock must be held");
    }
}

#[cold]
fn panic_pgroup(msg: &str) -> ! {
    let mut buf = [0u8; 96];
    let n = msg.as_bytes().len().min(buf.len() - 1);
    buf[..n].copy_from_slice(&msg.as_bytes()[..n]);
    xv6_panic(buf.as_ptr())
}

// ---------------------------------------------------------------------------
// Rust loops written in idiomatic macro-style (replaces legacy C loop macros)
// ---------------------------------------------------------------------------

macro_rules! pg_for_each_tg {
    ($pg:expr, $tg:ident, $body:block) => {
        let (head, offset) = unsafe {
            let pg_raw = $pg as *mut crate::bindings::pgroup;
            let p_groups = &raw mut (*pg_raw).thread_groups;
            let off = core::mem::offset_of!(crate::bindings::thread_group, list_entry);
            (p_groups as *mut crate::bindings::list_node_t, off)
        };
        $crate::list_for_each!(head, offset, $tg, {
            let $tg = $tg as *mut ThreadGroup;
            $body
        });
    };
}

macro_rules! tg_for_each_thread {
    ($tg:expr, $t:ident, $body:block) => {
        let (head, offset) = unsafe {
            let tg_raw = $tg as *mut crate::bindings::thread_group;
            let t_list = &raw mut (*tg_raw).thread_list;
            let off = core::mem::offset_of!(crate::bindings::thread, tg_entry);
            (t_list as *mut crate::bindings::list_node_t, off)
        };
        $crate::list_for_each!(head, offset, $t, {
            let $t = $t as *mut Thread;
            $body
        });
    };
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Called each time a thread group is removed from a process group.
/// If the process group is empty (no thread groups), remove it from
/// its session and free it. Caller must hold pid_wlock.
// NO-STANDALONE-FN: the remaining pgroup operations are `Pgroup::` associated
// fns (cleanup/alloc/init/add_thread/remove_thread/add_tg/remove_tg/live_dec/
// migrate_tg/get/setpgid/getpgid/kill). Call sites name `Pgroup::<op>`.
impl Pgroup {
    fn cleanup(pg: *mut Pgroup) {
        if pg.is_null() {
            return;
        }
        if pg_p_cnt(pg) > 0 {
            return;
        }
        if pg_is_kernel(pg) != 0 {
            return;
        }
        pg_set_exited(pg, 1);
        let sess = pg_session(pg);
        if !sess.is_null() {
            session_remove_pg(sess, pg);
        }
        if pg_list_entry_is_detached(pg) == 0 {
            pg_list_entry_detach(pg);
        }
        // N-R6d-2b: deregister from the generational table BEFORE freeing the slab,
        // bumping the slot generation so every outstanding `Pgid` edge to this
        // pgroup (`thread.pgroup`, `session.fg_pgrp`) goes stale → resolves to null.
        Self::table_remove(pg);
        xv6_pgroup_slab_free(pg);
    }

    // ---------------------------------------------------------------------------
    // Allocation
    // ---------------------------------------------------------------------------

    pub(crate) fn alloc(
        pgid: i32,
        leader: *mut ThreadGroup,
    ) -> *mut Pgroup {
        let pg = xv6_pgroup_slab_alloc();
        if pg.is_null() {
            return ptr::null_mut();
        }
        // Zero whatever the slab returned (struct pgroup is small but we don't
        // know the layout here; rely on the field setters to clobber what we
        // care about, then explicitly zero the bitfield + counters).
        pg_set_pgid(pg, pgid);
        pg_set_leader(pg, leader);
        pg_list_entry_init(pg);
        pg_threads_init(pg);
        pg_tgs_init(pg);
        pg_set_t_cnt(pg, 0);
        pg_set_p_cnt(pg, 0);
        pg_set_exited(pg, 0);
        pg_set_session(pg, ptr::null_mut());
        // N-R6d-2b: register in the generational pgroup table (stamps `self_pgid`).
        // On the impossible "table full" case (see `NPGROUP`'s sizing) fail the
        // alloc cleanly — free the slab object and return null, exactly the contract
        // `pgroup_alloc` already has for allocation failure — rather than hand back
        // an unregistered pgroup whose `Pgid` edges would never resolve.
        if Self::table_insert(pg).is_none() {
            xv6_pgroup_slab_free(pg);
            return ptr::null_mut();
        }
        pg
    }

    // P3-1B: only caller is `proc/thread.rs::init_entry` (via its own `extern`
    // declaration typed `*mut bindings::thread`, a different opaque marker
    // type than this file's local `Thread` -- converted to a typed thin
    // wrapper at the call site, same precedent as `sysproc.rs`'s
    // `thread_clone`) -- demoted.
    pub(super) fn init(initproc: *mut Thread) {
        let ret = xv6_pgroup_slab_init();
        if ret != 0 {
            panic_pgroup("Failed to initialize pgroup slab cache");
        }
        let tg = t_thread_group(initproc);
        if tg.is_null() {
            panic_pgroup("pgroup_init: initproc has no thread_group");
        }
        let pg = Self::alloc(t_pid(initproc), tg);
        if pg.is_null() {
            panic_pgroup("pgroup_init: pgroup_alloc failed");
        }
        Self::add_tg(pg, tg);
        Self::add_thread(pg, initproc);
    }

    // ---------------------------------------------------------------------------
    // Thread membership
    // ---------------------------------------------------------------------------

    pub(crate) fn add_thread(
        pg: *mut Pgroup,
        t: *mut Thread,
    ) -> i32 {
        pid_assert_wholding();
        if pg.is_null() || t.is_null() {
            return -EINVAL;
        }
        if pg_exited(pg) != 0 {
            return -ESRCH;
        }
        if !t_pgroup(t).is_null() {
            return -EEXIST;
        }
        t_set_pgroup(t, pg);
        t_set_pgid(t, pg_pgid(pg));
        t_pg_entry_init(t);
        pg_threads_push_back(pg, t);
        pg_inc_t_cnt(pg);
        0
    }

    pub(crate) fn remove_thread(t: *mut Thread) {
        pid_assert_wholding();
        if t.is_null() {
            return;
        }
        let pg = t_pgroup(t);
        if pg.is_null() {
            return;
        }
        if pg_t_cnt(pg) <= 0 {
            panic_pgroup("Process group thread count went negative");
        }
        if t_pg_entry_is_detached(t) != 0 {
            panic_pgroup("Thread is not in the process group list");
        }
        pg_threads_detach(t);
        pg_dec_t_cnt(pg);
        t_set_pgroup(t, ptr::null_mut());
        t_set_pgid(t, 0);
    }

    // ---------------------------------------------------------------------------
    // Thread group membership
    // ---------------------------------------------------------------------------

    pub(crate) fn add_tg(
        pg: *mut Pgroup,
        tg: *mut ThreadGroup,
    ) -> i32 {
        pid_assert_wholding();
        if pg.is_null() || tg.is_null() {
            return -EINVAL;
        }
        if pg_exited(pg) != 0 {
            return -ESRCH;
        }
        if !tg_pgroup(tg).is_null() {
            return -EEXIST;
        }
        tg_list_entry_init(tg);
        pg_tgs_push_back(pg, tg);
        tg_set_pgroup(tg, pg);
        pg_inc_p_cnt(pg);
        0
    }

    pub(crate) fn remove_tg(tg: *mut ThreadGroup) {
        pid_assert_wholding();
        if tg.is_null() {
            return;
        }
        let pg = tg_pgroup(tg);
        if pg.is_null() {
            return;
        }
        if pg_p_cnt(pg) <= 0 {
            panic_pgroup("Process group thread group count went negative");
        }
        if pg_tg_list_entry_is_detached(tg) == 0 {
            pg_tgs_detach(tg);
        }
        pg_dec_p_cnt(pg);
        tg_set_pgroup(tg, ptr::null_mut());
        Self::cleanup(pg);
    }

    // P3-1B: zero callers anywhere in the tree (grep-verified). Demoted from
    // `#[no_mangle]`; `#[allow(dead_code)]` documents the gap (matches this
    // wave's `goldfish_rtc_init`/`sched_timer_add` precedent). Note: the file
    // already carries a blanket `#![allow(dead_code)]` (line 12), so this
    // per-item attribute is documentation, not a functional requirement.
    fn live_dec(pg: *mut Pgroup) {
        if pg.is_null() {
            return;
        }
        if pg_atomic_dec_t_cnt(pg) <= 0 {
            panic_pgroup("Process group live thread count went negative");
        }
    }

    // ---------------------------------------------------------------------------
    // Migration
    // ---------------------------------------------------------------------------

    // P3-1B: only called internally within this file (`pgroup_setpgid`) --
    // demoted.
    fn migrate_tg(
        tg: *mut ThreadGroup,
        new_pg: *mut Pgroup,
    ) {
        pid_assert_wholding();
        if tg.is_null() {
            panic_pgroup("pgroup_migrate_tg: NULL tg");
        }
        if new_pg.is_null() {
            panic_pgroup("pgroup_migrate_tg: NULL new_pg");
        }
        if !tg_pgroup(tg).is_null() {
            Self::remove_tg(tg);
        }
        Self::add_tg(new_pg, tg);

        tg_for_each_thread!(tg, t, {
            if t_pgroup(t) != new_pg {
                Self::remove_thread(t);
            }
            if t_pgroup(t).is_null() {
                Self::add_thread(new_pg, t);
            }
        });
    }

    // ---------------------------------------------------------------------------
    // Lookup
    // ---------------------------------------------------------------------------

    pub(crate) fn get(pgid: i32) -> *mut Pgroup {
        let t = ProcTable::get_thread(pgid);
        if is_err_const(t as *const c_void) {
            return err_ptr::<Pgroup>(-ESRCH);
        }
        let pg = t_pgroup(t);
        if pg.is_null() {
            return err_ptr::<Pgroup>(-ESRCH);
        }
        pg
    }

    // ---------------------------------------------------------------------------
    // setpgid / getpgid
    // ---------------------------------------------------------------------------

    // P3-1B: only caller is `proc/sysproc.rs::sys_setpgid` (crate-path `use`,
    // not an `extern` redeclaration) -- demoted.
    pub(super) fn setpgid(pid: i32, pgid: i32) -> i32 {
        let p = xv6_current_thread();
        let mut pid = pid;
        let mut pgid = pgid;
        let tgid = ThreadAccess::from_ptr(p).map_or(-1, |ta| ta.resolve_tgid());

        if pid == 0 {
            pid = tgid;
        }
        if pgid == 0 {
            pgid = pid;
        }
        if pgid < 0 {
            return -EINVAL;
        }

        xv6_pid_wlock();

        // Find the target thread.
        let target;
        if pid == t_pid(p) || pid == tgid {
            target = p;
        } else {
            target = ProcTable::get_thread(pid);
            if is_err_const(target as *const c_void) {
                xv6_pid_wunlock();
                return -ESRCH;
            }
        }

        // Must be self or a child of self.
        if target != p && t_parent(target) != p {
            xv6_pid_wunlock();
            return -ESRCH;
        }

        let target_tgid = ThreadAccess::from_ptr(target).map_or(-1, |ta| ta.resolve_tgid());
        // Cannot change session leader's pgid.
        if t_sid(target) == target_tgid {
            xv6_pid_wunlock();
            return -EPERM;
        }
        // Must be in the same session.
        if t_sid(target) != t_sid(p) {
            xv6_pid_wunlock();
            return -EPERM;
        }
        // Already in the right group?
        if t_pgid(target) == pgid {
            xv6_pid_wunlock();
            return 0;
        }

        let tg = t_thread_group(target);
        let new_pg: *mut Pgroup;

        if pgid == target_tgid {
            // Creating a new process group with target as leader.
            new_pg = Self::alloc(pgid, tg);
            if new_pg.is_null() {
                xv6_pid_wunlock();
                return -ENOMEM;
            }
            let sess = t_session(target);
            if !sess.is_null() {
                session_add_pg(sess, new_pg);
            }
        } else {
            // Joining an existing process group.
            new_pg = Self::get(pgid);
            if is_err_const(new_pg as *const c_void) {
                xv6_pid_wunlock();
                return -EPERM;
            }
            if pg_exited(new_pg) != 0 {
                xv6_pid_wunlock();
                return -EPERM;
            }
            if pg_session(new_pg) != t_session(target) {
                xv6_pid_wunlock();
                return -EPERM;
            }
        }

        Self::migrate_tg(tg, new_pg);
        xv6_pid_wunlock();
        0
    }

    // P3-1B: only caller is `proc/sysproc.rs::sys_getpgid` (crate-path `use`,
    // not an `extern` redeclaration) -- demoted.
    pub(super) fn getpgid(pid: i32) -> i32 {
        if pid == 0 {
            return t_pgid(xv6_current_thread());
        }
        if pid < 0 {
            return -EINVAL;
        }

        rcu_lock();
        let target = ProcTable::get_thread(pid);
        if is_err_const(target as *const c_void) {
            rcu_unlock();
            return -ESRCH;
        }
        let pgid = t_pgid(target);
        rcu_unlock();
        pgid
    }

    // ---------------------------------------------------------------------------
    // Process-group-wide signal delivery
    // ---------------------------------------------------------------------------

    pub(crate) fn kill(pgid: i32, signum: i32) -> i32 {
        xv6_pid_rlock();
        let pg = Self::get(pgid);
        if is_err_const(pg as *const c_void) || pg_exited(pg) != 0 {
            xv6_pid_runlock();
            return -ESRCH;
        }
        if pg_is_kernel(pg) != 0 {
            xv6_pid_runlock();
            return -EPERM;
        }

        let mut count = 0;
        pg_for_each_tg!(pg, tg, {
            xv6_tg_send_signo(tg, signum);
            count += 1;
        });
        xv6_pid_runlock();

        if count > 0 { 0 } else { -ESRCH }
    }
} // impl Pgroup
